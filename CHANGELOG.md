# Changelog

All notable changes to VaptVupt are documented in this file.

## [1.4.0] - 2026-04-05

### Added
- **Cross-block dictionary carry**: hash chain now spans block boundaries.
  The encoder passes absolute positions to `compress_block()` so matches
  can reference data from previous blocks. The decoder accepts cross-block
  offsets via a `dst_base` parameter threaded through all decode functions.
  Large logs (7MB, 7 blocks): **5.73:1** — previously each block was
  independent and cross-block patterns were lost.
- **Context model decode prefetch**: `__builtin_prefetch` in the order-1
  context decode loop hides L2/L3 latency for the 4MB context tables.
- `CHANGELOG.md` covering all versions from v1.0.0.

### Changed
- `compress_block()` signature: now accepts `start_pos` parameter for
  absolute positioning within the full input buffer.
- All decoder functions (`decode_block_tokens`, `decode_stripped_tokens`,
  `decode_block_huffman`, `decode_block_ans`, `decode_block_ans4`,
  `decode_block_ctx`, `vva_decode_sequences`) now accept a `dst_base`
  parameter for cross-block offset validation.
- `vva_decode_sequences()` in `vv_ans.h`: added `dst_base` parameter.

### Performance (balanced mode)
| File | v1.3.0 | v1.4.0 | gzip-9 | vs gzip |
|------|--------|--------|--------|---------|
| Source code (531K) | 59.1:1 | 59.5:1 | 51.7:1 | ✅ +15% |
| JSON (232K) | 10.7:1 | 10.7:1 | 8.8:1 | ✅ +21% |
| Logs small (438K) | 3.6:1 | 3.6:1 | 4.9:1 | gap 26% |
| Logs 7MB (7.5MB) | — | **5.7:1** | 7.5:1 | gap 24% |
| XML markup (641K) | 18.1:1 | 18.1:1 | 14.6:1 | ✅ +24% |
| CSV tabular (596K) | 6.1:1 | 6.1:1 | 6.5:1 | gap 6% |
| Long-range (800K) | 5.7:1 | 5.7:1 | 1.4:1 | ✅ +307% |
| Binary struct (1.2MB) | 1.3:1 | 1.3:1 | 1.4:1 | gap 11% |

---

## [1.3.0] - 2026-04-05

### Added
- **Cross-block dictionary carry** (encoder side): `compress_block()`
  refactored to use absolute positions via `start_pos` parameter.
  Hash chain persists across block boundaries.

### Changed
- Adaptive window trial reduced from full-block lazy parse to 256KB
  greedy (depth=4). Encode speed improved **2.6×** (logs: 5.8 → 15.3 MB/s).

---

## [1.2.0] - 2026-04-03

### Added
- **Zupt integration API** (`include/vaptvupt_api.h`, `src/vaptvupt_api.c`):
  - `vvz_compress(src, len, dst, cap, level)` — level 1/5/9
  - `vvz_decompress(src, len, dst, cap)`
  - `vvz_compress_bound(len)`
- **Amalgamation build**: `make amalg` produces `build/vaptvupt.c` +
  `build/vaptvupt.h` for Zupt drop-in embedding.
- Context model decode prefetch for improved extreme-mode throughput.

### Changed
- Adaptive window trial now uses greedy depth=4 on 256KB sample
  (was full lazy parse on entire first block). 2.6× faster encode.

---

## [1.1.0] - 2026-04-03

### Added
- **Rep-match encoding**: OF codes 0-2 reserved for repeated offsets
  (0 extra bits each). `VVA_OF_CODES` increased from 24 to 27.
  JSON improved **78%** (5.97:1 → 10.67:1), XML improved **65%**
  (11.0:1 → 18.13:1). Rep-matches account for 20-40% of all matches
  in structured data.
- **Adaptive window selection**: encoder trial-compresses first block
  at wlog=16 and wlog=20. If wlog=20 saves ≥3%, uses wider window.
  Long-range data: 1.00:1 → 5.73:1 (auto-detected).
  Logs stay at wlog=16 (no regression).
- Forward rep-match tracking in `vva_encode_sequences()`: precomputes
  OF codes with rep-match detection in a forward pass, then uses them
  in the backward ANS encoding pass.
- Decoder rep-match tracking in `vva_decode_sequences()`: maintains
  `dec_rep[3]` array during decode, resolving OF codes 0-2 to stored
  recent offsets.

### Changed
- `of_encode()`: explicit offsets now use codes 3-26 (shifted by 3
  to make room for rep codes 0-2).
- `of_decode()`: codes 0-2 return 0 (caller resolves via rep array).
- `of_extra[]` array: prepended 3 zeros for rep codes.
- `vva_encode_sequences()` signature: added `off_bytes` parameter.

---

## [1.0.0] - 2026-03-29

### Added
- **Variable-width offsets**: 2-byte offsets for wlog≤16, 3-byte
  offsets for wlog>16. Changes across encoder, decoder, and ANS
  sequence coding.
- `wlog` field added to `matcher_t` struct.
- Match distance limit now derived from window log:
  `(1 << wlog) - 1` instead of hardcoded 65535.
- `off_bytes` parameter added to `emit_seq()`, `compress_block()`,
  `decode_block_tokens()`, `decode_stripped_tokens()`, all
  `decode_block_*()` wrappers, `extract_literals()`,
  `parse_sequences()`, and `vva_encode_sequences()`.
- Decoder reads `wlog` from frame header and computes
  `off_bytes = (wlog > 16) ? 3 : 2` for all decode paths.

### Fixed
- `bitpair_t.val` widened from `uint16_t` to `uint32_t` to safely
  hold up to 23 offset extra bits for wlog>16. Silent truncation
  caused data corruption on offsets >65535.

### Notes
- Default wlog remains 16 for all modes (backward compatible).
- Users can set `opts.window_log = 20` (1MB) or `22` (4MB) for
  large files with long-range patterns.
- Verified: wlog=20 gives 2.85:1 on long-range data vs 1.00:1
  at wlog=16.

---

## Pre-1.0 History

### v0.9.0 — SIMD Fix + Winner-Takes-All
- Replaced scalar byte-by-byte match copy with `vv_copy_match()` SIMD
  in sequence decoder. Decode speed restored: 1,536 → 2,579 MB/s.
- Winner-takes-all: encoder compares 'S' (sequence) vs 'I' (literal-only)
  compressed sizes, picks smaller per block.
- Logs ratio restored from 3.30:1 to 3.53:1.
- GPL-3.0-or-later license headers applied to all files.

### v0.8.0 — Sequence Coding
- Three ANS tables per block: literals + ML codes (36) + OF codes (24).
  New block tag 'S' (0x53).
- Source code ratio jumped 50.1:1 → 60.6:1 (+21%).
- JSON: 5.24:1 → 5.97:1. Logs regressed: 3.53:1 → 3.30:1.

### v0.7.0 — Order-1 Context Model
- 256 per-byte ANS tables (block tag 'C'). Contexts with <16 observations
  inherit the global table. 4MB decode memory footprint.
- Context model gives 72% smaller literals than global ANS on JSON standalone.

### v0.6.0 — Sparse Header + 4-Way Interleaved
- Adaptive sparse/dense ANS header format (VVA_HDR_SINGLE/SPARSE/DENSE).
  Saves 400+ bytes per block on small alphabets.
- 4-way interleaved ANS encode/decode: 4 independent states per iteration,
  hides L1 table lookup latency.
- Winner-takes-all block selection introduced.

### v0.5.0 — tANS Replaces Huffman
- Asymmetric Numeral Systems (tANS) with L=4096 (12-bit table).
  Replaced Huffman as primary entropy coder. Huffman retained for
  backward compatibility (block tag 'H').

### v0.1.0–v0.4.0 — Foundation
- LZ engine with 5-byte multiply-shift hash, depth-limited chain.
- AVX2 match extension (32 bytes/cycle).
- SIMD tiered match copy (32B/16B/8B/scalar).
- Canonical Huffman coding (later superseded by tANS).
- XXH64 checksum, frame format, CLI tool.
- 3 rep-match offsets checked before hash probe.
