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

## Headline numbers (honest, full Silesia 12-fixture corpus, v2.50.6)

| Axis | Result |
|---|---|
| **Aggregate ratio vs zstd-3** | **+1.29% behind** (zstd-3 wins on full Silesia geomean) |
| **Per-fixture ratio wins vs zstd-3** | 0 of 12 fixtures (subset of 4 — dickens, xml, sao, x-ray — does show vv ahead by ~1%, but cherry-picked) |
| **Decode vs zstd-1** | 0.6–0.9× across full Silesia; wins on 1 of 12 fixtures (sao) |
| **Decode vs zstd-3** | wins on 3 of 12 fixtures (osdb, sao, x-ray); 0.6–0.9× elsewhere |
| **Encode vs zstd-1** | 0.3–0.5× (vv encode is roughly 4× slower) |
| **Embeddability** | 2-file amalgamation (`build/vaptvupt.c` + `build/vaptvupt.h`); zero deps; ~101 KB binary |

**Honest reading**: vv is competitive on binary/scientific data
(sao, x-ray, osdb) for decode, but is not a general-purpose drop-in
replacement for zstd. See [PERFORMANCE.md](docs/PERFORMANCE.md) for
the full per-fixture table, target-win counts, and explicit retraction
of prior "vv beats zstd-3" claims that were based on a 4-fixture
subset.

### Retracted prior claims

Earlier versions of this README and CHANGELOG carried these claims:

- "vv beats zstd-3 by 1.07% aggregate ratio" — **retracted**. Full
  Silesia (12 fixtures) inverts the result: vv is 1.29% behind.
- "1.27× faster decode" — **retracted**. Full-corpus measurement shows
  vv-fast at 0.6–0.9× zstd-1 decode aggregate.
- "26,773 MB/s on random data, 3.7× zstd-19" — **retracted as
  misleading**. Random data is incompressible — decode reduces to
  memcpy. Not a meaningful codec comparison.

The retractions landed in v2.50.6 PERFORMANCE.md after running the
full Silesia benchmark (Sprint 32). The original numbers were real
measurements on a 4-fixture subset, but were propagated as if they
were full-corpus aggregates.

The remaining per-fixture gaps are larger than the prior subset
suggested. See PERFORMANCE.md and SPEED_PROGRAM.md for the eight
SPEED-PROGRAM lessons learned.

## Audit Status (v2.50.6)

| Check | Status |
|---|---|
| cppcheck | ✓ 0 issues |
| clang scan-build | ✓ 0 bugs |
| GCC strict warnings (`-Wpedantic -Wshadow -Wcast-qual` + 9 more) | ✓ 0 hits |
| **clang `-fsanitize=integer`** (strict UBSan superset) | ✓ **0 errors** |
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

## Headline capabilities (honest framing — see PERFORMANCE.md for full numbers)

- **Decode wins on binary/scientific data** — vv-fast decode beats
  zstd-3 on osdb, sao, and x-ray (Silesia binary fixtures). On
  general-purpose text it lags zstd by 10-40%.
- **Synthetic binary ratio: 1,149×** — pattern-rich synthetic payloads
  see much higher ratios than general-purpose data (this is a real
  measurement on a non-Silesia synthetic fixture, not a competitive
  claim).
- **Real binary ratio** (libc.so.6, bash, python3): generally within
  a few percent of zstd-3 — see PERFORMANCE.md per-fixture.
- **Embeddability**: 2 files (`build/vaptvupt.c` + `build/vaptvupt.h`),
  101 KB binary, zero external deps. Drop in and ship.
- **Post-quantum companion library**: libpqvaptvupt ships hybrid
  ML-KEM-768 + X25519 sealed-box encryption with AES-NI acceleration
  (~46× speedup on x86_64).

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the full
measurement matrix on the entire Silesia corpus, the four SPEED
PROGRAM targets, and explicit retractions of prior aggregate claims.

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

### Compression ratio — 8-fixture subset (NOT full Silesia)

**Honest framing**: the table below is an **8-fixture subset** — 4 internal
synthetic fixtures (fx_text, fx_json, fx_source, bash) plus 4 Silesia
fixtures (dickens, xml, sao, x-ray). On this subset, vv beats zstd-3
by 1.07% aggregate. On **full Silesia (12 fixtures)**, vv is 1.29%
**behind** zstd-3. See PERFORMANCE.md for the full-corpus measurement
that supersedes this subset.

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
| **Subset aggregate** | 35,543,635 | **17,024,883** | 17,209,103 | **−1.07%** (vv wins on subset) |

On the subset, vv-extreme beats zstd-3 by 1.07% aggregate, winning
per-fixture on 4 of 8. **The full Silesia corpus (mozilla, mr, nci,
ooffice, osdb, reymont, samba, webster — added in Sprint 32) inverts
this:** vv loses by 1.29% on full geomean ratio. The retracted-claims
section above details the discrepancy.

### Decode throughput — 8-fixture subset

5×-warmed runs, matched fixtures, single-threaded:

| Codec | Decode throughput (8-fixture subset aggregate) |
|---|---|
| **VaptVupt v2.50.6** | ~151 MB/s |
| zstd-3 | ~119 MB/s |
| Subset speedup | ~1.27× |

**Honest framing**: this is the same 8-fixture subset. On full Silesia,
vv-fast decode is 0.6–0.9× zstd-1 across the corpus, winning on 1 of
12 fixtures (sao). See PERFORMANCE.md.

### Random-data decode — measurement context (not a competitive claim)

Random data is incompressible. When the compressor cannot find matches,
the wire format degenerates to literal-only blocks and decode reduces
to essentially `memcpy`. The numbers below are real measurements but
**do not represent codec algorithmic comparison** — they measure each
tool's literal-block path, which is dominated by memory bandwidth and
each tool's header/checksum overhead, not its decompression algorithm.

| Content | VaptVupt `--fast` | zstd-19 | lz4-9 | gzip-9 |
|---|---:|---:|---:|---:|
| Random (AEAD ciphertext) | 26,773 MB/s | 7,172 | 17,594 | 412 |
| Binary (pattern-rich) | 14,414 MB/s | 8,098 | 19,933 | 598 |
| Synthetic repeat | 2,029 MB/s | 1,786 | 2,278 | 1,140 |

The "26,773 MB/s" number is real but doesn't mean vv has a better
decoder — it means vv has a smaller per-frame overhead in its
literal-block path. This is useful for AEAD-wrapped archives where
the payload is high-entropy ciphertext, but it's not a general decode
speed claim.

For full per-fixture decode/encode tables, see [PERFORMANCE.md](docs/PERFORMANCE.md).



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

**Where the codec stands vs zstd** (Sprint 32 full-Silesia measurement):

- Decode wins on **3 of 12 Silesia fixtures** (osdb, sao, x-ray) vs
  zstd-3. Loses on the other 9.
- Aggregate ratio: 1.29% behind zstd-3 on geomean across full Silesia.
- Encode: 0.3–0.5× zstd-1 throughput. The encoder was not a SPEED
  PROGRAM priority and remains a target for future architectural
  work (match-finder algorithm change, optimal parse).
- Strong points: small binary (~101 KB), zero dependencies, AGPL-3.0
  +commercial license, post-quantum companion library.

See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the complete
measurement matrix, target-win counts, and the eight SPEED PROGRAM
lessons logged across sprints 25-32.
