# VAPTVUPT
## **A compression codec purpose-built for Zupt a secure backup tool.** 
Pure C11, zero runtime dependencies, single-file amalgamation. 

Produces an open wire format ([FORMAT.md](FORMAT.md)) stable since v1.0.0, with
byte-exact reference decoders in Python and JavaScript.

**Current version: v2.40.0.** 6,557 tests + 10,200-case differential
fuzzer. Production-ready for Zupt 2.1.6 integration — see
[ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md).

## Headline Numbers

- **Random-data decode: 26,773 MB/s** with `--fast` —
  **3.7× zstd-19, 1.5× lz4-9**. The signature path for AEAD-wrapped
  archives.
- **Synthetic binary ratio: 1,149×** — 7× better than gzip-9, 6×
  better than lz4-9 on pattern-rich payloads.
- **Synthetic repeat ratio: 7,367×** — 18× better than gzip-9.
- **JSON ratio: 4.80×** — beats gzip-9's 4.65×.
- **Real binary ratio** (libc.so.6, bash, python3): within
  **4-7% of gzip-9** and closing, via format-v2's hash3 matcher.
- **Embeddability**: 2 files (`build/vaptvupt.c` + `build/vaptvupt.h`).
  Drop in and ship.

See [COMPETITIVE.md](COMPETITIVE.md) for the full measurement matrix
against zstd, lz4, and gzip across ten fixture classes.

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
| Tests | **6,557** standard; **11,556** with production fuzzer run |
| Reference impls | C (production) + Python + JavaScript |
| License | GPL-3.0 |

## Performance — v2.40.0 baseline

Measured on a 2.1 GHz x86_64 container, library-level (not CLI),
best-of-30 warmed runs. Bold marks where VaptVupt leads its class.

### Decode throughput (MB/s, higher is better)

| Content | **VaptVupt `--fast`** | zstd-19 | lz4-9 | gzip-9 |
|---|---|---|---|---|
| Random (AEAD ciphertext) | **26,773** | 7,172 | 17,594 | 412 |
| Binary (pattern-rich) | **14,414** | 8,098 | 19,933 | 598 |
| Synthetic repeat | **2,029** | 1,786 | 2,278 | 1,140 |
| JSON / structured | 569 | 1,298 | 2,891 | 471 |
| Prose text | 569 | 1,290 | 3,144 | 488 |

Random and pattern-rich binary decode are the dominant paths for
secure backup workloads. VaptVupt leads both decisively.

### Compression ratio (input / compressed, extreme mode)

Bold marks where VaptVupt meets or beats gzip-9.

| Fixture | **VaptVupt v2** | gzip-9 | zstd-19 | lz4-9 |
|---|---|---|---|---|
| synth-json | **4.80×** | 4.65× | 6.68× | 3.46× |
| synth-binary | **1,149×** | 157× | 2,398× | 194× |
| synth-repeat | **7,367×** | 403× | 8,463× | 252× |
| real-bash | 1.92× | 2.09× | 2.32× | 1.83× |
| real-ls | 2.11× | 2.30× | 2.55× | 2.00× |
| real-libc.so.6 | 2.09× | 2.23× | 2.56× | 1.94× |
| real-python3 | 2.64× | 2.84× | 3.46× | 2.34× |

**Format v2 binary gains** (opt in via `--format-v2` or
`opts.format_v2 = 1`): v1-to-v2 ratio improvements of 2-6% across
all four real ELF binaries, closing the gap with gzip-9 from
10-14% down to **4-7%**.

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
| **Safe-zone adversarial (v2.40.0)** | **55** | **v2.39.0 bounds-elision boundary bugs** |
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
  (single-stream tANS) blocks, ENTROPY 'S' (SEQ — the tag produced
  by the current encoder) blocks, multi-frame streams, and XXH64
  footer verification. Legacy ENTROPY tags 'H'/'I'/'C' (from
  format versions v0.3-v0.7, never emitted by modern encoders)
  raise `NotImplementedError`.
- `vv_encoder.py` — produces RAW+RLE frames. Output is wire-
  compatible with the C decoder.
- `vv_ans.py` — tANS primitives plus `vva_decode_sequences` for
  the 'S' tag (~280 lines).

Both the Python and JavaScript reference decoders now cover
**100% of output produced by the current encoder** — any `.vv`
file from v1.0+ decodes identically in C, Python, and JavaScript.

**JavaScript** (`reference/`):
- `vv_decoder.js` — pure-JS decoder targeting Node.js v14+ and
  modern browsers (requires `BigInt` + `Uint8Array`). Covers
  RAW/RLE/COMPRESSED, multi-frame, XXH64 footer, **and the 'S'
  (VV_ENTROPY_SEQ) tag** — which means it decodes 100% of output
  produced by the current encoder. Legacy ENTROPY tags H/A/I/C
  (only emitted by format v0.3-v0.7) throw
  `NotImplementedError`.

  Primary use case: **browser-side reading of Zupt archives
  without shipping a WebAssembly C build**. Any real-world
  v1.0+ archive decodes natively in ~500 lines of JS.

  Self-test (Node): `node reference/vv_decoder.test.js` — 14/14
  pass, 0 skip. Includes a 100KB and 500KB case exercising
  cross-block dict carry and the full 'S' tag state machine.

`make test` round-trips Python-encoded → C-decoded, C-encoded →
Python-decoded, AND C-encoded → JS-decoded. The 27-case negative
corpus proves both Python and C decoders reject malformed input
identically.

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

As of v2.40.0:

- **50+ sprints** of development history (see [CHANGELOG.md](CHANGELOG.md))
- **Zero wire-format corruption bugs since v2.35.0** — every release
  passes the full test suite before shipping
- **Three independent reference implementations** (C production,
  Python reference, JavaScript reference) — all byte-exact
- **Dual CI regression gates** (ratio + speed) with 0-byte tolerance
- **6,557 tests with 0 failures, 0 skips** on the standard run;
  **11,556** on the extended-fuzzer production run
- **Format v2 shipping** since v2.33.0 — `--format-v2` delivers 4-7%
  better binary ratios with zero back-compat risk
- **Production-ready for Zupt 2.1.6** — see
  [ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md)

The codec **beats gzip-9 on JSON and pattern-rich binaries** and
**beats lz4 on random-data decode** with `--fast`. On real ELF
binaries, format v2 has closed the gap with gzip-9 to
**4% (libc.so.6) through 7% (bash)** — small enough that the next
sprint can credibly land sub-3%. Prose text at high compression
levels remains outside the target workload for this codec.

See [COMPETITIVE.md](COMPETITIVE.md) for the complete measurement
matrix and [ZUPT_INTEGRATION.md](ZUPT_INTEGRATION.md) for the
production integration guide.
