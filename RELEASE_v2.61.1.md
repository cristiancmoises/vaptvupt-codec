# VaptVupt v2.61.1

**Decode performance + hardening point release (Sprint 125). Fast-mode
decode gains 55%; the SEQ decoder validates entropy tables up front and
does less per-block work. Valid-stream output is byte-identical to
v2.61.0 — this release changes no bits on the wire.**

## Fast-mode decode: +55%

The token-decode loop's literal copy is now an unconditional 16-byte
wildcopy for the dominant `ll <= 14` case, replacing `memcpy`'s branchy
variable-size dispatch. No new bounds risk: the loop's existing
safe-zone guards already reserve 48 readable bytes of input and 72
writable bytes of output at every iteration that takes this path, so
the 16-byte store is covered by margins that were already enforced.

Measured in-process on a 13 MB mixed buffer (json + logs + csv + xml +
docs + source): **1,540 → 2,430 MB/s**, byte-identical output,
reproducible across interleaved A/B rounds.

Honest scope note: this is the *fast-mode* (token-block) path only.
Balanced/extreme blocks decode through the SEQ path, which is **neutral
within noise** in this release (~985 MB/s on 1 MB blocks before and
after) — the SEQ-side changes below reduce allocations and branches but
do not measurably move its throughput. The v2.61.0 head-to-head tables
in `bench/COMPARISON.md` remain valid for balanced/extreme.

## SEQ decoder hardening

- **Structurally-invalid ANS tables are rejected up front.** Per-block
  validation now checks, once per block at header-parse time, that (1)
  no out-of-range symbol has a nonzero frequency and (2) each table's
  frequencies sum to exactly `ANS_L` (4096) — the invariant every valid
  stream satisfies by construction (`normalize_freq`). Without (2), a
  corrupt *underfull* header leaves stale scratch bytes in unfilled
  spread-table slots, and those fake "symbols" bypassed the old
  per-sequence bound check. UBSan caught exactly this hole (an
  out-of-bounds index into `ll_extra[36]`) during validation of this
  release; it is now closed. The hoist makes the old per-sequence
  `code >= VVA_*_CODES` branch tautological, so it is removed from the
  hot loop — the same guarantee, enforced earlier and stricter, at one
  less branch per sequence.
- **Encoder fails closed on a truncated sequence parse.**
  `parse_sequences` now returns an error if it exhausts sequence
  capacity with tokens remaining (previously a silent truncation risk
  by construction only), and re-checks capacity after oversize
  literal-run splits.

## Less per-block work

- The SEQ decoder's four per-block allocations (spread scratch + three
  decode tables) are fused into one; when a block has no matches, the
  ML/OF tables alias the LL table instead of being allocated and
  zeroed — two 16 KB sentinel `memset`s gone.
- `build_dec`/table builds (both encode and decode side): per-symbol
  `nb_max`/`low_count` are precomputed once per 256 symbols instead of
  recomputed (with an `ilog2` loop) for each of the 4096 slots —
  ~16× fewer `ilog2` evaluations per table build.
- Encoder sequence scratch drops from 16 bytes per *token byte* to a
  tight per-sequence bound (~3× less scratch per 1 MB block),
  byte-identical output.

## Validation

- 22/22 C test suites green under `-O3 -flto` **and** under
  `-fsanitize=address,undefined -fno-sanitize-recover=all`.
- Ratio gate ± 0 bytes on all fixtures (output is unchanged).
- Differential fuzzer 5,200/5,200 consistent (C vs Python reference).
- Negative corpus 27/27 consistent (malformed frames rejected, no
  crashes).
- ASan + UBSan + LeakSanitizer sweep: clean across the 11-file
  benchmark corpus × all 3 modes, roundtrips byte-exact, zero leaks.

## Compatibility

**Valid-stream output is byte-identical to v2.61.0.** The wire format
is unchanged (frame version 1). No decoder upgrade is required to read
v2.61.1 output, and v2.61.1 reads all prior valid streams exactly as
before. The only behavioral difference is on *corrupt* input: malformed
entropy tables that earlier versions rejected mid-decode (or decoded
into checksum-failing garbage) are now rejected at block-header
validation. No API or ABI change.

— Cristian Cezar Moisés, securityops.co. In Code We Trust.
