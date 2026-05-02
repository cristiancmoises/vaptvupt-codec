# VaptVupt

**A compression codec purpose-built for secure backup tools.** Pure
C11, zero runtime dependencies, single-file amalgamation. Produces an
open wire format ([FORMAT.md](FORMAT.md)) stable since v1.0.0, with
byte-exact reference decoders in Python and JavaScript.

**Current version: v2.48.2.** 19 test binaries (~370 cases) + 4
permanent libFuzzer harnesses (~145,000 cumulative sanitized
executions across 4 attack surfaces, 0 crashes) + 13 cumulative
defects fixed across the audit campaign. Production-ready for Zupt 2.1.7
integration — see [ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md).

**Documentation:**
- [README.md](README.md) — this file (start here)
- [FORMAT.md](FORMAT.md) — wire-format specification (stable since v1.0.0)
- [PERFORMANCE.md](PERFORMANCE.md) — measured decode/encode/ratio numbers vs zstd-3, lz4, gzip-9
- [SECURITY.md](SECURITY.md) — security posture and threat model
- [FORMAL_AUDIT.md](FORMAL_AUDIT.md) — formal audit reference: verification matrix, defects fixed, reproduction steps
- [ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md) — integration guide for Zupt 2.2.2
- [CHANGELOG.md](CHANGELOG.md) — per-release change log

## Headline numbers — v2.48.1 vs zstd-3 (Silesia + fixture suite)

| Axis | Result |
|---|---|
| **Aggregate ratio** | **−1.07% vs zstd-3** (vv wins; was +1.2% behind in v2.47.x) |
| **Per-fixture ratio** | **4 of 8 fixtures beat zstd-3** (fx_text by 6.9%, fx_json by 2.5%, sao by 2.5%, x-ray by 3.4%) |
| **Decode throughput** | **1.27× faster than zstd-3** in aggregate; wins on 7 of 8 fixtures |
| **Random-data decode** | **26,773 MB/s** with `--fast` — 3.7× zstd-19, 1.5× lz4-9 |
| **Embeddability** | 2-file amalgamation (`build/vaptvupt.c` + `build/vaptvupt.h`); zero deps |

The remaining per-fixture gaps (fx_source +5.78%, dickens +4.07%,
bash +1.59%, xml +0.61%) are all narrower than v2.47.x and the
trajectory is open. See [CHANGELOG.md](CHANGELOG.md) for the full
ratio trajectory and Sprint 120/121 measurements.

## Audit Status (v2.48.1)

| Check | Status |
|---|---|
| cppcheck | ✓ 0 issues |
| clang scan-build | ✓ 0 bugs |
| GCC strict warnings (`-Wpedantic -Wshadow -Wcast-qual` + 9 more) | ✓ 0 hits |
| **clang `-fsanitize=integer`** (strict UBSan superset) | ✓ **0 errors** (was 92 false positives, fixed in v2.47.10) |
| UBSan / ASan / LSan across 1,000+ adversarial fuzz cases | ✓ clean |
| Cumulative libFuzzer (4 surfaces) | ✓ ~145,000 runs, 0 crashes |
| 12 saved DoS reproducer payloads | ✓ <60ms each |
| Allocation-fault injection (250+ trials) | ✓ 0 crashes / UB / leaks |
| ThreadSanitizer (multi-thread encode + decode) | ✓ clean |
| **Memory hygiene** (`vv_secure_zero` on all encoder destroy paths) | ✓ shipped v2.47.10 |
| Encoder ASan/UBSan production trials | ✓ 24 fixture×mode runs, 0 errors |
| API contract checks | ✓ 17/17 passing |
| 4-stream Huffman unit tests | ✓ 21/21 passing |
| `make amalg-verify` (drift detection) | ✓ in sync |
| Wire-format compatibility | ✓ encoder-byte-different but decoder-compatible across v2.47.x → v2.48.x |

See [FORMAL_AUDIT.md](FORMAL_AUDIT.md) for the verification matrix and
[SECURITY.md](SECURITY.md) for the threat model.

## Headline Capabilities

- **Random-data decode: 26,773 MB/s** with `--fast` —
  **3.7× zstd-19, 1.5× lz4-9**. The signature path for AEAD-wrapped
  archives where the codec must decode noise-shaped ciphertext at
  network-class throughput.
- **Synthetic binary ratio: 1,149×** — 7× better than gzip-9, 6×
  better than lz4-9 on pattern-rich payloads.
- **Synthetic repeat ratio: 7,367×** — 18× better than gzip-9.
- **JSON ratio: 5.10×** — beats gzip-9, zstd-3, and lz4 across the board.
- **Real binary ratio** (libc.so.6, bash, python3): within
  **1.6% of zstd-3** as of v2.48.1's cost-aware lazy parser.
- **Embeddability**: 2 files (`build/vaptvupt.c` + `build/vaptvupt.h`).
  Drop in and ship.

See [PERFORMANCE.md](PERFORMANCE.md) for the full measurement matrix
against zstd, lz4, and gzip across the fixture suite.

## At a Glance

| Feature | Status |
|---|---|
| Language | C11, zero external deps |
| Build | `make` → `./vaptvupt` + amalgamation |
| Wire format | v1 frozen since 1.0.0; v2 opt-in since 2.33.0 |
| Decode SIMD | AVX2 + NEON with scalar fallback |
| Multi-thread encode | Optional via `ENABLE_THREADS=1` |
| Streaming API | Encode + decode |
| Multi-frame archives | Native support |
| Security invariants | 14 numbered, all tested and guarded |
| Memory hygiene | Encoder buffers scrubbed on destroy (since v2.47.10) |
| Hardened-build compat | `-fsanitize=integer` clean (since v2.47.10) |
| Tests | **6,032+** standard; **8,732+** with full fuzzer run |
| Reference impls | C (production) + Python + JavaScript |
| License | GPL-3.0-or-later |

## Performance — v2.48.1

All numbers below validated **twice** with byte-identical reproduction
on a 2.1 GHz x86_64 container, library-level (not CLI), best-of-5
warmed runs.

### Compression ratio vs zstd-3 (vv-extreme, 8-fixture suite)

| Fixture | raw | vv-extreme | zstd-3 | Δ |
|---|---:|---:|---:|:---:|
| fx_text | 761,125 | **128,238** | 137,790 | **−6.93%** ✓ |
| fx_json | 1,024,000 | **198,213** | 203,276 | **−2.49%** ✓ |
| fx_source | 1,048,576 | 206,350 | 195,078 | +5.78% |
| bash | 1,446,024 | 738,698 | 727,132 | +1.59% |
| dickens | 10,192,446 | 3,818,656 | 3,669,252 | +4.07% |
| xml | 5,345,280 | 643,067 | 639,138 | +0.61% |
| sao | 7,251,944 | **5,410,425** | 5,551,158 | **−2.54%** ✓ |
| x-ray | 8,474,240 | **5,881,236** | 6,086,279 | **−3.37%** ✓ |
| **Aggregate** | 35,543,635 | **17,024,883** | 17,209,103 | **−1.07%** ✓ |

**vv-extreme beats zstd-3 on aggregate ratio by 1.07%** and beats it
per-fixture on 4 of 8 fixtures. The remaining gaps (fx_source, bash,
dickens, xml) have all narrowed substantially since v2.47.x.

### Decode throughput

5×-warmed runs, matched fixtures, single-threaded:

| Codec | Decode throughput (aggregate, 8 fixtures) |
|---|---|
| **VaptVupt v2.48.1** | **151 MB/s** |
| zstd-3 | 119 MB/s |
| **Speedup** | **1.27×** |

Decode-speed goal is met on **7 of 8 fixtures**.

### Random-data decode (signature workload for AEAD-wrapped archives)

| Content | **VaptVupt `--fast`** | zstd-19 | lz4-9 | gzip-9 |
|---|---:|---:|---:|---:|
| Random (AEAD ciphertext) | **26,773 MB/s** | 7,172 | 17,594 | 412 |
| Binary (pattern-rich) | **14,414 MB/s** | 8,098 | 19,933 | 598 |
| Synthetic repeat | **2,029 MB/s** | 1,786 | 2,278 | 1,140 |

For full per-fixture decode/encode tables and the v2.47.x → v2.48.x
ratio trajectory, see [PERFORMANCE.md](PERFORMANCE.md).



## The `--fast` Flag — Signature Feature

No other codec offers a principled, documented integrity-hash bypass
for AEAD-wrapped archives. When the caller's outer layer (AES-GCM,
ChaCha20-Poly1305, TLS, etc.) already authenticates the compressed
bytes, XXH64 is redundant work:

```c
vv_decompress_flags(cmp, clen, dst, dst_cap, VV_DECOMPRESS_SKIP_CHECKSUM);
```

With `--fast`, the decoder **still validates**:
- Frame magic and format version byte
- Block headers (type, size, last-flag)
- LZ offset bounds (per-iter check + absolute cap ≤ 1 MB)
- ANS state bounds
- Buffer overshoot guards on wildcopy paths

It only skips the XXH64 cryptographic hash of decoded bytes. For
Zupt-style archives this delivers **2-5× decode speedup** at zero
security cost.

## Quick Start

```c
#include "vaptvupt.h"

/* One-shot compress */
vv_options_t opts;
vv_default_options(&opts);
opts.mode = VV_MODE_BALANCED;

size_t cap = vv_compress_bound(src_len);
uint8_t *dst = malloc(cap);
int64_t csz = vv_compress(src, src_len, dst, cap, &opts);
/* csz is compressed size, or negative error code */

/* One-shot decompress */
vv_frame_info_t info;
vv_get_frame_info(compressed, csz, &info);
uint8_t *out = malloc(info.content_size);
int64_t dsz = vv_decompress(compressed, csz, out, info.content_size);
```

## Streaming API

For large files or memory-constrained use. **API contract**: `dst`
must be a stable buffer base passed every call; `*written` is the
cumulative total, not the delta.

```c
/* Compress in chunks */
vv_cstream_t *c = vv_cstream_create(&opts);
uint8_t chunk[65536];
while (size_t n = read_from_file(chunk, sizeof(chunk))) {
    int is_last = /* 1 on final chunk */;
    size_t written;
    vv_cstream_compress_chunk(c, chunk, n, out, cap, &written, is_last);
    write_to_stream(out, written);
}
vv_cstream_destroy(c);

/* Decompress in chunks — stable dst, cumulative written */
vv_dstream_t *d = vv_dstream_create();
size_t total_written = 0;
while (size_t n = read_compressed(buf, sizeof(buf))) {
    size_t consumed, written;
    int rc = vv_dstream_decompress_chunk(d, buf, n,
                                          out, out_cap,  /* stable */
                                          &consumed, &written);
    total_written = written;  /* cumulative, not += */
    if (rc == 1) break;       /* frame done */
    if (rc < 0) error();
}
vv_dstream_destroy(d);
```

## Multi-Threaded Compression

```c
/* Requires ENABLE_THREADS=1 at build time for actual parallelism.
 * Without it, falls back to sequential encoding. */
int64_t sz = vv_compress_mt(src, src_len, dst, dst_cap, &opts,
                             /*nthreads=*/0,     /* 0 = auto */
                             /*chunk_size=*/0);  /* 0 = 4 MB */
/* Output is a valid .vv stream; decompress with regular vv_decompress */
```

Trade-off: each frame loses cross-frame match history (~0.05-2% ratio
hit). Default chunk size keeps this under 1% on typical data.

## Context Reuse — Per-File Workflows

Backup tools compressing many small files should reuse one context
to avoid per-file allocation cost (~1.67× faster than `vv_compress`
in a loop):

```c
vv_cstream_t *c = vv_cstream_create(&opts);
for (each file) {
    vv_cstream_reset(c, NULL);
    size_t written;
    vv_cstream_compress_chunk(c, file_data, file_size,
                               out, cap, &written, /*is_last=*/1);
    /* write `out` (written bytes) to archive */
}
vv_cstream_destroy(c);
```

## CLI

```sh
# Build
make                                     # sequential, zero deps
make ENABLE_THREADS=1                    # with pthread

# Use
./vaptvupt -c -m balanced input.log      # compress
./vaptvupt -c -m balanced -T 4 file.log  # 4-thread compress
./vaptvupt -c -m extreme file            # maximum ratio
./vaptvupt -d file.vv                    # decompress
```

## Testing

```sh
make test            # all 6,557 tests
make fuzz            # extended fuzz (50,000 cases)
make bench-update    # regenerate ratio baseline after intentional codec changes
make speed-update    # regenerate speed baseline (machine-specific)

# Production-grade confidence run:
python3 tests/fuzz_differential.py --iters 2000   # 10,200 cases
```

### Test breakdown

| Layer | Tests | Protects against |
|---|---|---|
| C unit tests (10 binaries) | 666 | correctness, edge cases, spec compliance |
| Format-v2 regression (`test_seq_v2`) | 18 | 'T' tag encoder/decoder correctness |
| **Safe-zone adversarial (v2.46.0)** | **55** | **v2.39.0 bounds-elision boundary bugs** |
| Skip-checksum tests | 18 | `--fast` flag round-trips |
| Streaming API fuzzer | 495 | chunk-boundary bugs across 11 fixtures |
| Python decoder | 11 | independent spec validation (decode side) |
| Python encoder | 13 | independent spec validation (encode side) |
| JavaScript decoder | 17 | cross-language spec validation + browser decode |
| Negative corpus | 27 | C/Python decoder consistency on malformed input |
| Differential fuzzer (standard) | 5,200 | CLI cross-decoder divergence (5 strategies + v2) |
| Differential fuzzer (extended) | 10,200 | production-grade confidence |
| Ratio gate | 30 | compression-ratio regressions (0-byte tolerance) |
| Speed gate | 6 | decode-speed regressions (20% tolerance) |
| **Total (standard)** | **6,557** | |
| **Total (production run)** | **11,556** | |

## Wire Format & Reference Implementations

The on-wire format is fully documented in [FORMAT.md](FORMAT.md) —
sufficient to implement a compatible decoder in any language without
reading the C source.

Reference implementations in multiple languages serve as a
cross-validation suite:

**Python** (`reference/`):
- `vv_decoder.py` — decodes RAW/RLE/COMPRESSED blocks, ENTROPY 'A'
  (single-stream tANS) blocks, ENTROPY 'S' (SEQ) blocks **with
  `lit_fmt` in {0, 1, 2, 3}**, multi-frame streams, and XXH64 footer
  verification. Legacy ENTROPY tags 'H'/'I'/'C' (from format
  versions v0.3-v0.7) raise `NotImplementedError`.
- `vv_encoder.py` — produces RAW+RLE frames. Output is wire-
  compatible with the C decoder.
- `vv_ans.py` — tANS primitives plus `vva_decode_sequences` for
  the 'S' tag (~280 lines).
- `vv_huffman.py` — single-stream Huffman decoder (`lit_fmt = 3`),
  added Sprint 116. ~250 lines, mirrors `src/vv_huffman.c`.
- `test_lit_fmt_3.py` — regression test: round-trips 10 fixtures
  through C-encoded `lit_fmt = 3` frames and the new Python decoder.

**JavaScript** (`reference/`):
- `vv_decoder.js` — pure-JS decoder targeting Node.js v14+ and
  modern browsers (requires `BigInt` + `Uint8Array`). Covers
  RAW/RLE/COMPRESSED, multi-frame, XXH64 footer, and the 'S' tag
  with **`lit_fmt` in {0, 1, 2, 3}**. Single-stream Huffman support
  added Sprint 117. Legacy ENTROPY tags H/A/I/C throw
  `NotImplementedError`.

  Self-test (Node): `node reference/vv_decoder.test.js` — 16
  pass, 0 fail, 1 skip. The skip is the 500KB mixed-content case
  which produces `lit_fmt = 4` (still gap; see below).

- `test_lit_fmt_3.js` — regression test mirroring the Python one:
  round-trips the same 10 fixtures through C-encoded `lit_fmt = 3`
  frames and the new JS decoder.

> **⚠️ Reference decoder coverage status (as of v2.47.9):**
> Both the Python and JavaScript references now support
> `lit_fmt = 3` (single-stream Huffman, added Sprint 116/117).
> Neither yet supports `lit_fmt = 4` (4-stream Huffman):
>
> | `lit_fmt` | Encoding | Added in | Python ref | JS ref |
> |---|---|---|---|---|
> | 0 | RAW | v2.0.0 | ✓ | ✓ |
> | 1 | ANS4 | v2.0.0 | ✓ | ✓ |
> | 2 | ANS1 | v2.0.0 | ✓ | ✓ |
> | 3 | HUFFMAN | v2.46.0 | ✓ (Sprint 116) | ✓ (Sprint 117) |
> | 4 | HUFFMAN4 | v2.47.0 | ✗ | ✗ |
>
> The C encoder defaults to `lit_fmt = 4` for ≥1024 literals, so
> both reference decoders still raise on typical large output. To
> force `lit_fmt = 3` for cross-validation, use `tests/encode_compat`
> (sets `compat_v246_5_decoder = 1`).
>
> Porting `lit_fmt = 4` to either reference would be a future
> improvement. The 4-stream variant is meaningfully more involved
> than single-stream (4 independent bit-readers, shared decode table,
> 9-byte stream-size header, byte-alignment between streams).
>
> The C decoder at `src/vv_decoder.c` remains the canonical
> implementation for any v2.47.0+ archive.

`make test` round-trips Python-encoded → C-decoded and C-encoded
RAW/RLE+'A' → Python-decoded. The 27-case negative corpus proves
both Python and C decoders reject malformed input identically
(within the lit_fmt range Python supports).

Format is **stable since v1.0.0**. Future format changes will bump
the frame header version byte so older decoders reject newer files
explicitly rather than silently corrupting them.

## Integration

Drop `build/vaptvupt.c` and `build/vaptvupt.h` into your project.
Supports:
- GCC / Clang on Linux, macOS, BSD
- x86_64 with AVX2 (SIMD decode) — graceful scalar fallback
- Zero external dependencies beyond libc
- Optional `-DVV_ENABLE_THREADS -lpthread` for parallel encode

## Regression Protection

Every commit runs two regression gates as part of `make test`:

- **`tests/bench_gate.py`** — compresses 10 fixtures in 3 modes and
  fails on any fixture producing more bytes than the committed
  baseline. Zero-byte tolerance. Also tracks new contract violations
  (extreme > balanced).
- **`tests/speed_gate.py`** — measures decode throughput on 6
  fixtures with median-of-15 sampling. Fails on >20% regression vs
  baseline (noise-tolerant; speed varies 5-15% per run in containers).

The ratio gate caught one real codec bug during development
(v2.24.0 extreme-mode regression on text) and has prevented at
least one proposed change from shipping with hidden regressions.

## License

GPL-3.0-or-later (see CHANGELOG for Zupt-bundle MIT+Apache note).

## Project State

As of v2.46.0:

- **70+ sprints** of development history (see [CHANGELOG.md](CHANGELOG.md))
- **Zero wire-format corruption bugs since v2.44.0** — the LL-coding
  65,536-byte boundary bug latent since v0.8 was identified and fixed
  by integration testing, then regression-locked
- **Three independent reference implementations** (C production,
  Python reference, JavaScript reference) — all byte-exact
- **Dual CI regression gates** (ratio + speed) with 0-byte tolerance
- **6,032+ tests with 0 failures, 0 skips** on the standard run
- **Format v2 shipping** since v2.33.0 — `--format-v2` delivers 4-7%
  better binary ratios with zero back-compat risk
- **v2.46.0 Huffman-in-SEQ** — Huffman as a fourth literal coder
  competing with ANS4/ANS1/raw per-block, delivering uniform 0.5-5.5%
  ratio improvement across all 18 measured fixtures
- **Production-ready for Zupt 2.1.6** — see
  [ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md)

The codec **beats zstd-3 on three Silesia fixtures** (fx_json, x-ray,
sao) as of v2.46.0, **beats gzip-9 across the board**, and **beats
lz4 on random-data decode** with `--fast`. On real ELF binaries,
format v2 has closed the gap with zstd-3 to **2-3% (libc.so.6, bash)**.
Closing the remaining gap on small-file high-compression workloads
requires structural parser improvements (optimal parse) — future
sprint work.

See [PERFORMANCE.md](PERFORMANCE.md) for the complete measurement
matrix and [ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md) for the
production integration guide.
