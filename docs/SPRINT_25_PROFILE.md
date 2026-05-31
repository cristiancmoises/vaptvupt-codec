# Sprint 25 — SPEED baseline and decode profile

## Outcome

**Sprint 25 = measurement infrastructure for the SPEED PROGRAM.** No codec changes. Establishes a reproducible baseline and identifies concrete Sprint 26 targets through profiling.

## Baseline (Linux x86_64, gcc 13.3.0, Silesia corpus)

```
fixture    tool             enc_ms   enc_MB/s   dec_ms   dec_MB/s   ratio
------------------------------------------------------------------------------
dickens    vv -fast          201.0       50.7     32.7      311.7    1.99
dickens    vv -balanced     1047.1        9.7     48.1      211.9    2.65
dickens    vv -extreme      2252.2        4.5     46.9      217.2    2.67
dickens    zstd -1            80.5      126.6     22.1      460.7    2.39
dickens    zstd -3           103.4       98.6     30.4      334.8    2.78
dickens    zstd -9           459.4       22.2     32.0      318.1    3.11

xml        vv -fast           43.2      123.8     10.6      505.1    5.25
xml        vv -balanced      133.6       40.0     12.4      430.1    8.00
xml        vv -extreme       280.9       19.0     12.1      440.3    8.31
xml        zstd -1            23.3      229.9      9.3      575.4    7.66
xml        zstd -3            23.8      224.2     10.1      530.5    8.36
xml        zstd -9           102.1       52.4      9.8      548.2   10.29

sao        vv -fast          205.5       35.3     17.4      416.1    1.20
sao        vv -balanced     1496.6        4.8     44.3      163.7    1.33
sao        vv -extreme      2026.5        3.6     44.6      162.5    1.34
sao        zstd -1            62.2      116.6     16.7      433.5    1.16
sao        zstd -3           101.7       71.3     20.8      348.6    1.31
sao        zstd -9           295.6       24.5     26.4      274.8    1.39

x-ray      vv -fast          271.6       31.2     22.9      369.8    1.16
x-ray      vv -balanced     1627.3        5.2     54.6      155.3    1.44
x-ray      vv -extreme      1970.0        4.3     54.8      154.6    1.44
x-ray      zstd -1            48.6      174.5     15.3      553.1    1.25
x-ray      zstd -3           120.5       70.3     25.3      335.1    1.39
x-ray      zstd -9           332.6       25.5     39.3      215.7    1.58
```

Reproduce with `python3 bench/bench.py` after `make`.

## Decode profile (dickens, balanced mode, 10 runs)

Build: `cc -pg -O2 ... ` (gprof instrumentation).
Profiler: gprof flat profile from `gmon.out`.

```
flat profile (cumulative across 10 decode runs):
  100.00%  vva_decode_sequences_impl     (40 ms total, ~4 ms/run)
    0.00%  build_dec
    0.00%  build_enc
    0.00%  assign_canonical_codes
    0.00%  build_dec_table
    0.00%  read_header
    0.00%  vva_decode_sequences          (passthrough)
    0.00%  vvh_decode4                   (Huffman 4-stream literal decode)
    0.00%  vv_decompress_flags           (top-level entry)
```

**Finding: virtually all decode time (>99%) is in `vva_decode_sequences_impl`.** Everything else is below gprof's sampling resolution. The Huffman literal decode (`vvh_decode4`) is below noise floor — already fast.

This matches the Sprint 19 byte-level profile, which showed 92.2% of compressed output is the ML/OF/LL ANS sequence bitstream. Time spent decoding is proportional to bytes decoded.

## Top-3 optimization targets identified for Sprint 26

`vva_decode_sequences_impl` is a 480-line function. Inside it, the per-sequence inner loop (lines 2298–2510) executes ~1.28M times for dickens. Each iteration:

1. **Refills the bitstream buffer** (`ans_br_fill`, ~50% of iters)
2. **Reads three ANS table entries** from `dec_ll`, `dec_of`, `dec_ml` (4 KB × 4 bytes = 16 KB each → 48 KB total tables; **larger than typical 32 KB L1 D-cache**)
3. **Three `ans_br_read` bit-shift extractions** (LL, OF, ML codes)
4. **One literal copy** (memcpy of 0–N bytes from `lit_buf` to output)
5. **One match copy** (LZ self-overlap copy, vectorized via `vv_copy_match`)
6. **State updates** for three ANS streams

### Target A — Cache-pack the three ANS tables (highest leverage)

The three 16 KB tables (`dec_ll`, `dec_of`, `dec_ml`) total 48 KB. On most x86_64 cores L1 D-cache is 32 KB. Each iteration accesses all three tables at unpredictable indices → expected 3 L1 misses every 1–2 iterations. Even with hardware prefetch, this is a 2–3 cycle stall per miss.

**Sprint 26 candidate change:** Interleave the three tables into a single struct-of-arrays where the same state index pulls all three entries from one cache line. Reduces 3 cache lines touched per iteration to 1 (if state indices align) or 2.

Risk: only works cleanly if the three states are correlated (they're not, since they're independent ANS streams). Realistic gain: 1.05–1.15× decode speedup. May not justify the wire format / table-build complexity.

### Target B — Branchless safe-zone validation (medium leverage)

Lines 2316–2358 contain four conditional `VV_UNLIKELY(...)` validators:
- `litlen + lit_pos > total_lits` (corrupt)
- `op + litlen > op_end` (overflow, gated by safe_zone)
- `match_count check` (similar pattern)
- `offset > op - dst_base` (similar pattern)

The `in_safe_zone` flag is computed once per iteration but the explicit branches remain. A real speedup is possible by hoisting the safe-zone test to amortize across multiple iterations, but this requires unrolling the inner loop and is invasive.

**Sprint 26 candidate change:** unroll the inner loop 2× or 4× and check safe-zone only once per group. Realistic gain: 1.05–1.10× decode speedup.

### Target C — Specialize the literal-copy path (low risk, modest leverage)

Line 2364 already has the `litlen ≤ 16` fast path with a single 16-byte memcpy. But there are TWO sequential checks (`litlen ≤ 16 && op + 16 <= op_end`) and the memcpy is still a function call (even if inlined). A SIMD intrinsic (`_mm_loadu_si128 + _mm_storeu_si128`) would replace the memcpy with two instructions.

**Sprint 26 candidate change:** replace `memcpy(op, lit_buf + lit_pos, 16)` with explicit SSE2 load/store. Realistic gain: 1.02–1.05× decode speedup.

## Sprint 26 recommendation

**Target B (loop unroll + branchless safe-zone)** is the best risk-adjusted bet:
- Lowest complexity (no wire format change)
- Lowest correctness risk (existing semantics preserved exactly)
- Real measurable gain (1.05–1.10× on text-heavy fixtures)
- Sets up Target C as a follow-up that compounds

**Skip Target A for now.** The cache-pack would touch table layout (wire format + build code) and the gain estimate (1.05–1.15×) doesn't justify the change-surface area until simpler wins are exhausted.

## What this sprint did NOT do

- No codec changes. Compressed output byte-identical to v2.48.5 on all four fixtures.
- No encoder profiling — that's Sprint 28's job.
- No claims of "faster than zstd" — the gap is currently 1.5–2.2× on decode and 5–15× on encode.
- No estimation of total program duration. Real speed work happens 1 sprint at a time.

## v2.49.0 release notes

- `bench/bench.py` — reproducible baseline measurement script
- `bench/profile_decode.sh` — gprof wrapper for decode profiling
- `docs/SPEED_PROGRAM.md` — multi-sprint program plan
- `docs/SPRINT_25_PROFILE.md` — this document

No codec changes. Byte-identical to v2.48.5.

## Next sprint

**Sprint 26 = decode optimization round 1.** Target B (loop unroll + branchless safe-zone). Acceptance: measurable speedup on ≥ 2 of 4 fixtures, no regression on others, compressed-output byte-identical.

If Sprint 26 measurement shows < 5% gain, ship as docs-only negative result and reassess. If it shows ≥ 5% gain, ship as v2.49.1.
