# VaptVupt v2.61.0

**Performance + correctness release (Sprint 124). Encoder gets a
benchmark-driven speed/ratio program and two latent-corruption fixes;
the decoder gets a faster Huffman bit reader. Upgrade is recommended for
every encoder consumer.**

## The two defects

Both are encoder-side logic defects, not memory-unsafety; both were latent
in every release carrying the SEQ path and were exposed (one
deterministically) by this release's own entropy-gate change. Full
analysis in SECURITY.md (document version 1.9).

1. **Zero-match SEQ blocks omitted the LL bitstream.** The entire
   sequence-bitstream write — including the LL codes — sat inside
   `if (match_count > 0)` in `vva_encode_sequences_impl`. A block whose
   token stream is one pure literal run wrote the LL table header but no
   bitstream, while the decoder unconditionally decodes one LL code per
   sequence → corrupt decode. Unreachable by construction in prior
   releases (`csz >= braw` token streams always stored RAW); the relaxed
   entropy gate below made it reachable, and the BCJ roundtrip suite
   caught it at 190/5,576 failures. The LL bitstream is now written
   whenever `nseq > 0`.

2. **Path B could clobber Path A's output before winner selection.**
   Both block-encoding candidates share `ent_buf`: SEQ writes at the
   front, the literal-only path at `ent_buf + ent_cap/2`. Weak-block SEQ
   output can reach `vva_bound(braw)` — the *whole* buffer — and Path B
   runs exactly when SEQ is weak, so the halves overlapped and a
   SEQ-selected block could carry overwritten bytes. `ent_buf` is now
   `2 × vva_bound`; the streaming context's `stripped` buffer is sized
   to `tcap` for the same reason.

## Encoder speed program

Measured on the 11-file head-to-head corpus (text, source, JSON, logs,
CSV, XML, ELF, sensor floats, record structs, random, repetition);
balanced mode typically **+30-100%**, incompressible input **~4-6×**:

- **Skip acceleration on by default.** `opts->accel == 0` now means
  *auto*: fast mode gets the lz4-style ramp (2), balanced/extreme a
  gentle ramp (1) with the stride capped at 8 — the cap protects ratio
  on sparse-match record data. An early-RAW bail abandons a block's
  parse after 128 KiB with zero matches. Random data: 31 → 120-190 MB/s.
  Explicit `--accel N` values are honored unchanged.
- **Path B demoted to a true fallback** (runs only when SEQ output is
  weak, ≥ 7/8 of raw) and the CTX order-1 coder removed from it: it
  burned up to 50% of encode wall on low-redundancy binary and never
  won a block (Sprint 53 measurement, reconfirmed).
- **Two-finalist literal race.** One histogram plus analytic size
  estimates (exact-given-lengths Huffman; table-quantized ANS) select at
  most two real encodes — ANS4 and the better Huffman variant — instead
  of four. The estimates are provably optimistic, so `estimate ≥ raw`
  short-circuits incompressible literal blocks straight to raw with
  zero encode passes.
- **O(1) tANS symbol encode.** `enc_sym`'s linear slot scan (average
  f/2 iterations, up to ~2048 for a dominant symbol) replaced by a
  direct window computation; bit-identical output.
- **8-byte match-extension stride** past the first 8 bytes (the encoder
  TU deliberately builds without AVX2; extension was 1 byte/iteration).
- **Watermark-bounded secure-zero** (the Sprint 117 scrub now covers
  only bytes actually written — up to 14% of encode wall returned on
  fast inputs), duplicate lazy-probe hash insert eliminated, chain-walk
  prefetch priming gated to depth ≥ 8, trial matchers use acceleration.
- **Decoder:** the Huffman bit reader refills with one masked 8-byte
  load instead of up to 7 dependent byte loads.

## Ratio program

- **Adaptive format v2.** min_match=3 (`'T'` blocks) auto-enables for
  binary-detected input in balanced/extreme, where it is a measured win:
  sensor-float records 1.18 → **1.40** (zstd-3: 1.18), record structs
  1.52 → **1.72** (zstd-3: 1.60). Text/JSON keep `'S'` (v2 slightly
  hurts them). Requires a v2.33.0+ decoder; suppressed by
  `compat_v246_5_decoder`; explicit `format_v2` still forces it.
- **Offset-cost-aware match acceptance** in the hash5/hash4 chain
  walks: a farther candidate must pay for its extra offset bits
  (~6 bits per extra matched byte), protecting rep-offset streaks and
  OF-code entropy under larger windows.
- **Extreme routes v2 (binary) input to the deep greedy/lazy parser.**
  The optimal DP prices every match at full log2(offset) cost — it has
  no rep-offset model — and lost 15-20% to the rep-aware greedy path on
  record data; the extreme large-window scaling is skipped for v2 input
  for the same reason. Extreme on sensors: 1,110,027 → 927,147 bytes;
  structs −20%. Text-like input keeps the optimal parser, where it
  wins 3-11% over greedy.

## Validation

- Full suite: 22 test binaries green, ratio gate ± 0 on all fixtures,
  differential fuzzer 5,200/5,200 consistent, BCJ suite 5,576/5,576.
- 20,000-iteration randomized roundtrip sweep over the defect-1 trigger
  class (mostly-incompressible 8-16 KiB inputs), clean.
- Full 11-file benchmark corpus roundtrips byte-exact in all modes.

## Compatibility

**Default-mode output changed.** v2.61.0 default output is *not*
byte-identical to v2.60.4 (acceleration default, literal-coder
selection, match acceptance). Additionally, balanced/extreme output for
binary-detected input now uses `'T'` blocks and therefore **requires a
v2.33.0+ decoder** unless `compat_v246_5_decoder` is set. The wire
format itself is unchanged (frame version 1); all prior valid streams
decode exactly as before. No API or ABI change: `accel == 0`
reinterpreted as "auto" is the only behavioral option change, and
explicit values keep their meaning.

— Cristian Cezar Moisés, securityops.co. In Code We Trust.
