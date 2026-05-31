# VaptVupt SPEED PROGRAM — multi-sprint plan to close the speed gap to zstd

**Project:** vaptvupt codec
**Baseline:** v2.48.5
**Scope:** **A program, not a sprint.** "Faster than zstd" is the destination; this document is the road map. Each sprint ships measurable progress. Each sprint can also ship as a documented negative result if measurement says so. No sprint claims victory before measurement confirms it.

---

## 0. The honest baseline (measured this session, Linux x86_64, gcc 13)

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

### What this measurement actually says

**vv loses on every single encode/decode speed comparison, on every fixture, at every level.** Specifically:

| comparison | vv loses by |
|---|---|
| vv-fast enc vs zstd-1 enc | **2.5× slower** (dickens), 4× (sao), 5.6× (x-ray) |
| vv-balanced enc vs zstd-3 enc | **10× slower** (dickens), 15× (sao) |
| vv-extreme enc vs zstd-9 enc | **5× slower** (dickens), 6.9× (sao), 5.9× (x-ray) |
| vv-balanced dec vs zstd-3 dec | **1.6× slower** (dickens), 2.1× (sao), 2.2× (x-ray) |
| vv-fast dec vs zstd-1 dec | **1.5× slower** (dickens), 1.04× (sao), 1.5× (x-ray) |

**Ratio comparison (vv-extreme vs zstd-3):** vv wins by 2.54% on sao, 3.37% on x-ray; loses by 4.07% on dickens, 0.61% on xml. **Aggregate ratio: vv wins by ~0.3%** — the historical "1.07% aggregate win" claim is not reproduced on these 4 fixtures with current builds.

### What "faster than zstd" must mean to be honest

We need a falsifiable goal. The God-tier program defines **three precise targets**:

| Target | Definition | Acceptance |
|---|---|---|
| **T1 — Decode parity** | vv decode ≥ zstd-3 decode on average across {dickens, xml, sao, x-ray} | Within 5% of zstd-3 decode aggregate MB/s |
| **T2 — Encode parity (fast)** | vv-fast encode ≥ zstd-1 encode aggregate | Within 10% of zstd-1 encode aggregate MB/s |
| **T3 — Encode parity (balanced)** | vv-balanced encode ≥ zstd-3 encode aggregate, while preserving aggregate ratio win | Within 10% of zstd-3 enc; ratio still wins aggregate ≥ 0% |

T1 is most achievable. T2 is next. T3 is hardest and may be unreachable inside the current architecture — the program plans for that and ships honestly.

---

## 1. Why "speed parity in one sprint" is a lie

The gap is **5–15×** in some places. No single code change closes 5×. Real speed wins come from:

- **Profile-driven micro-optimizations** (1.05–1.3× per win, ~5–10 such wins → 1.5–3× cumulative)
- **Algorithmic changes** (hash-table layout, parser strategy, ANS state count) — 1.5–3× per win when they apply
- **SIMD vectorization** of hot inner loops (2–4× when applicable, often less)
- **Reducing allocations on hot paths** (1.1–1.5× by eliminating malloc/free per block)
- **Cache locality** (1.1–1.4× by reordering data structures)

Realistic program duration: **4–6 sprints to close T1**, **6–10 sprints to close T2 and T3**. Each sprint is measured, no aspirational claims, honest negative results when they happen.

Anyone promising "faster than zstd this week" is selling air. Below is a real plan.

---

## 2. The program — sprint by sprint

### Sprint 25 (this sprint) — DECODE profiling + measurement infrastructure

**Goal:** Profile vv's decoder on dickens/xml/sao/x-ray to identify the top-3 hottest functions. Build a `bench/` directory with a reproducible script that produces the baseline table at the top of this document. **No code changes to the codec.** This sprint builds the measurement infrastructure that every later sprint will use.

**Deliverables:**
- `bench/bench.py` — produces the baseline table reproducibly
- `bench/profile_decode.sh` — gprof/perf wrapper that produces flat profiles
- `docs/SPEED_BASELINE.md` — the baseline above, plus profile findings
- `docs/SPRINT_25_PROFILE.md` — top-3 hot functions in vv decode, with cumulative-time and call-count

**Acceptance:** baseline reproducible from a fresh extract with one command. Profile identifies concrete next-sprint targets.

### Sprint 26 — DECODE optimization round 1

**Goal:** Hit one of the top-3 hot functions identified by Sprint 25's profile. Optimize. Measure. Ship.

**Probable targets** (informed guess; profile will refine):
- **ANS decode inner loop** — likely 30–50% of decode time on dickens; SIMD-able
- **Huffman 4-stream decode** — likely 15–25% of decode time; already AVX2 in places
- **Match copy** — likely 10–20%; already heavily optimized but possibly more

**Acceptance:** measurable decode speedup on at least 2 of 4 fixtures, no regression on the others, ratio byte-identical. Target: 1.15–1.3× on the focus fixture.

### Sprint 27 — DECODE optimization round 2

**Goal:** Second target from Sprint 25's profile (or whatever Sprint 26's residual profile points at).

**Acceptance:** same standard. Cumulative goal: decode parity with zstd-3 on at least 2 fixtures by end of Sprint 27.

### Sprint 28 — ENCODE profiling

**Goal:** Mirror Sprint 25 for encoder. The encoder is the bigger gap (5–15×) and likely has different hot paths than the decoder.

**Probable targets:**
- **Match finding** (hash table lookups, chain walks)
- **Optimal parsing** in extreme mode
- **Literal entropy coding** (Huffman build + 4-stream encode)

**Deliverables:** `docs/SPRINT_28_ENCODE_PROFILE.md` with top-5 hot encoder functions.

### Sprints 29–31 — ENCODE optimization rounds

Three encoder optimization sprints. Each hits one hot function. Each measures. Each can ship negative result if the gain doesn't materialize.

### Sprint 32 — Integration + final measurement

Re-run the baseline table. Compare. Document.

**Acceptance for whole program:** decode parity (T1) achieved. Encode parity for fast/balanced (T2/T3) measurable progress, ideally achieved.

---

## 3. Execute Sprint 25 NOW

The rest of this document is the program plan. The actual work this turn:

1. Write `bench/bench.py` that reproduces the baseline table
2. Build a profiled vv binary with `-pg` and run it against dickens
3. Run `gprof` to get a flat profile
4. Write `docs/SPEED_BASELINE.md` with the table above
5. Write `docs/SPRINT_25_PROFILE.md` with the top hot functions and concrete Sprint 26 targets
6. Ship as **VaptVupt v2.49.0** — first release in the speed program

---

## 4. What I will NOT do

- Claim "faster than zstd" without a measurement to back it
- Skip the profiling phase to chase optimizations on hunches
- Promise specific speedup numbers in any sprint until that sprint's measurement is in
- Touch the encoder while the decoder optimization sprints are still open
- Trade ratio for speed without explicit per-sprint authorization (current ratio is part of vv's identity)

## 5. Definition of victory

Program is done when **either**:

- All three targets (T1, T2, T3) are achieved, vv ships v2.50.0 with measured speed wins, OR
- After 8 sprints (Sprint 33), residual gap is documented honestly with architectural reasons and the program closes with whatever was achieved.

Both endings are acceptable. "vv is faster than zstd on aggregate" is the win; "vv is now within 1.5× of zstd on aggregate after closing 4× of the gap, here's the documented evidence" is also a win.

---

**Executing Sprint 25 below this line.**

---

## Progress log (Sprints 25–31, ship dates real)

### What actually happened across the program

| Sprint | Version | Change | Encode gain | Decode gain | Notes |
|--------|---------|--------|------------:|------------:|-------|
| 25 | v2.49.0 | Profile + plan only | — | — | Established baseline |
| 26 | v2.50.0 | `-O2 → -O3 -flto` Makefile flip | +5% | **+14%** | Biggest single win. First head-to-head zstd-1 decode win on sao. |
| 27 | v2.50.1 | OOB-fold in ANS hot loop | ~0% | **+4%** | Hoisted 3 per-iter branches into 1 |
| 28 | v2.50.2 | `make pgo` target added | +6% | +4% | PGO build mode; opt-in |
| 29 | v2.50.3 | `matcher_insert_fast` | **+4%** | ~0% | Encode-focused; tightened bulk-insert loops |
| 30 | v2.50.4 | Unconditional prefetch in chain walk | **+2.7%** | ~0% | Marginal; shipped with honest framing |
| 31 | v2.50.5 (docs only) | **NEGATIVE** — runtime AVX2 dispatch in extend_match | -8% to +5% (noise/negative) | ~0% | Reverted. See SPRINT_31_NEGATIVE_RESULT.md |

**Cumulative since baseline v2.48.5:** Default build: ~+20% decode, ~+12% encode. PGO build: ~+27% decode, ~+18% encode.

### Sprint 31 negative result — runtime AVX2 dispatch did not work

**Hypothesis:** the encoder's AVX2 match-extension path was compile-time gated by `__AVX2__`, which is OFF on default portable builds. Refactoring with `__attribute__((target("avx2")))` + runtime CPUID dispatch should give modern x86_64 CPUs the AVX2 path without `-march=native`.

**Measurement:** -8% to +5% across fixtures, with high variance between runs. Average roughly flat, with dickens trending negative.

**Why it failed:** the `__attribute__((target))` function couldn't be inlined into `extend_match`'s call sites (different target attributes between caller and callee). The function call overhead — push args, jmp, ret — exceeds the AVX2 instruction-level savings on fast-mode workloads where ~70% of `extend_match` calls return in the scalar 8-byte fast-path BEFORE reaching the AVX2 inner loop. Only ~30% of calls would have benefited, and that benefit was eaten by the indirection cost.

**Architectural insight:** to actually use AVX2 in the encoder via runtime dispatch, the whole hot path (chain_match_ex, extend_match, hash functions) would need to be compiled in a dual-target shared object with function multiversioning at the chain_match_ex level (not extend_match). That's a multi-sprint architectural change, not a one-sprint micro-optimization.

**Real path to AVX2 in default encoder:** compile the encoder TU with `-mavx2` and add a runtime CPU check at `vv_compress` entry that errors out on non-AVX2 hardware. Sacrifices portability for speed; users who need portability can build with `-mno-avx2` or use the existing `make pgo` / default targets. Deferred decision.

### Sprint 32 (program close) — final measurement + PERFORMANCE.md

Next sprint: comprehensive benchmark vs zstd 1-19 on all of Silesia, write `PERFORMANCE.md` with honest target claims, declare program-end.

### Final lessons across the program

1. **Try the compiler before touching source.** Sprint 26 found +14% in one Makefile line.
2. **Look one level deeper than the profile.** Sprint 29 walked the call graph to find the real target.
3. **Inverted-order measurement is non-negotiable.** Saved us from shipping noise in Sprints 27 and 31.
4. **Diminishing returns hit fast.** +14% → +4% → +2.7% → 0% (Sprint 31).
5. **The "≥5% ship bar" is a guideline, not a law.** Sprint 30 (+2.7%) shipped with honest framing.
6. **Ratio is sacred.** Never traded for speed.
7. **`__attribute__((target))` is not a free SIMD upgrade.** Without inlinability, the dispatch overhead exceeds the SIMD gain on workloads with cheap fast-paths. (NEW lesson from Sprint 31.)
