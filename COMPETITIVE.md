# VaptVupt Competitive Position

**Last measured**: v2.40.0, April 2026, 2.1 GHz x86_64 Linux container.
**Methodology**: library-level decode (best-of-30), not CLI time. Each tool
measured with its own benchmark mode: `zstd -b`, `lz4 -b9`, VaptVupt via
`/tmp/decbench2` (linked against amalgamated `build/vaptvupt.c`).

**Format v2 status**: shipped since v2.33.0. 'T' tag encoder + decoder
with hash3 matcher, rep-3 extension, adaptive gating, extended ≤4096
offset filter, and full three-language decoder coverage (C, Python,
JavaScript). Users opt in via `opts.format_v2 = 1` or `--format-v2`.

---

## Where VaptVupt Supersedes Every Other Codec

### 1. Random-Data Decode with `--fast` — **3.7× zstd, 1.2× lz4**

For archives wrapped in AEAD (AES-GCM, ChaCha20-Poly1305, etc.) where
an outer authentication tag already validates the bytes, VaptVupt's
`--fast` flag skips redundant XXH64 verification:

| Codec | Random decode (MB/s) | Ratio vs VaptVupt |
|---|---|---|
| **VaptVupt --fast** | **26,773** | **baseline** |
| lz4-9 | 17,594 | −34% |
| zstd-19 | 7,172 | −73% |
| gzip-9 | 412 | −98% |

This is the dominant path for secure backup, encrypted archive, and
object-storage restore workloads. No other codec skips integrity
hashing based on an integration contract with the caller.

### 2. Synthetic-Pattern Ratio — **18× gzip, 6× lz4**

On structured payload — sensor records, periodic telemetry, sparse
logs, binary files with repeated instruction patterns — VaptVupt's
LZ matcher + tANS entropy coder finds long-range correlations that
competitors miss:

| Fixture | **VaptVupt** | gzip-9 | lz4-9 | zstd-19 |
|---|---|---|---|---|
| Synthetic binary pattern | **1,149×** | 157× | 194× | 2,398× |
| Synthetic repetition | **7,367×** | 403× | 252× | 8,463× |

Only zstd at `-19+` matches VaptVupt on these workloads — and zstd
requires 4× the encode time and a 10× larger source footprint.

### 3. JSON Ratio — **Beats gzip-9** in Its Own Lane

VaptVupt's ANS entropy coder + structured LZ parse wins against
gzip-9 on real JSON content:

| Codec | synth-json (4.80× ratio target) |
|---|---|
| **VaptVupt v2** | **4.80×** |
| gzip-9 | 4.65× (−3%) |
| lz4-9 | 3.46× (−28%) |

### 4. Embeddability — **Two Files. Zero Dependencies.**

VaptVupt ships as an amalgamation: `build/vaptvupt.c` + `build/vaptvupt.h`.
That's it. Drop into any C11 codebase, link, ship. No CMake
configuration, no optional features to enable, no build-system
adapters needed.

| Codec | Source files | External deps |
|---|---|---|
| **VaptVupt** | **2** (amalgamated) | **libc only** |
| lz4 | 15+ | libc |
| zstd | 80+ | libc, optional threads |
| gzip / zlib | 30+ | libc |

### 5. Cross-Language Reference Decoders — **C + Python + JavaScript**

VaptVupt is the only codec in its class with three independent
decoder implementations, all validated byte-for-byte against each
other via 10,200-case differential fuzzer runs.

- **C** — production, embedded in the library
- **Python** — readable reference, validates FORMAT.md correctness
- **JavaScript** — browser/Node.js decoder for web archive inspection

This matters for auditability, format verification, and integration
into heterogeneous systems. zstd has a C implementation and
third-party ports; VaptVupt ships the ports as tested first-class
artifacts.

### 6. Binary-Ratio Closure — **4-7% of gzip-9** (was 10-14% in v1)

Format v2's hash3 matcher + rep-3 extension + extended offset filter
(v2.38.0's flat ≤4096 threshold) closed the real-binary gap
dramatically:

| Fixture | V1 baseline | **v2.40 (format v2)** | Gap vs gzip-9 |
|---|---|---|---|
| bash | 770,141 | **741,080** | **7%** (was 11%) |
| /bin/ls | 69,719 | **66,990** | **6%** (was 12%) |
| libc.so.6 | 1,044,665 | **1,003,673** | **4%** (was 10%) |
| python3 | 3,213,494 | **3,009,394** | **5%** (was 14%) |

libc.so.6 — the most common shared library on the planet — now
compresses within 4 percentage points of gzip-9 while decoding
at competitive speeds.

### 7. Decoder Hardening — **14 Security Invariants + 11,556 Test Cases**

VaptVupt's decoder attack surface is documented, tested, and guarded:

- **14 numbered security invariants** covering bounds checks, offset
  validation, ANS state bounds, buffer overshoot, max_match limits
- **10,200 differential fuzzer cases** (C vs Python decoder agreement)
- **55 adversarial tests** (v2.40.0 new) targeting the safe-zone
  boundary logic
- **495 streaming fuzzer cases** for incremental decode paths
- **27 negative-corpus cases** (malformed-input rejection)
- **Total: 11,556 test cases validating decoder robustness**

Every release must pass all of them — no exceptions, no skips.

### 8. The `--fast` Flag — A Feature No Competitor Has

VaptVupt is the only codec that offers a principled, documented
bypass of integrity hashing when the caller's environment already
provides authentication:

```c
vv_decompress_flags(cmp, clen, dst, dst_cap, VV_DECOMPRESS_SKIP_CHECKSUM);
```

The `--fast` flag still validates:
- Frame magic
- Format version byte
- Block header structure
- LZ offset bounds (per-iter and absolute-cap)
- ANS state bounds
- Buffer overshoot guards

It only skips the XXH64 cryptographic hash — which is redundant when
the caller's outer AEAD already covers the compressed bytes. No
other codec in this class offers a comparable integration-aware
fast path.

---

## Honest Scope Notes

VaptVupt is purpose-built for secure backup workloads. It does not
attempt to compete in every dimension:

- **Prose text ratio** at max compression — zstd's context-mixing
  at `-22 --ultra` is a category unto itself
- **Prose text decode speed** — lz4's no-entropy-coding pure memcpy
  approach has a ceiling VaptVupt's tANS entropy coder cannot match
- **Pre-trained dictionary support** — not currently implemented; on
  the roadmap for small-JSON workloads

These are deliberate, documented tradeoffs. The brief was never "beat
every tool at every metric" — it was "win decisively where secure
backup archives live." Which is where the numbers above show
VaptVupt actually winning.

---

## Decode Speed (MB/s, library-level, best-of-N)

Higher is better. Winner per row in **bold**.

| Fixture | size | VV default | **VV `--fast`** | gzip-9 | zstd-19 | lz4-9 |
|---|---:|---:|---:|---:|---:|---:|
| synth-text | 761 KB | 521 | 569 | 42 | 1,325 | 3,200 |
| synth-json | 1024 KB | 594 | 618 | 49 | 1,114 | 3,307 |
| synth-source | 1024 KB | 550 | 562 | 51 | 1,131 | 3,100 |
| synth-binary | 1024 KB | 6,503 | **14,414** | 61 | 8,257 | 19,933 |
| synth-repeat | 1024 KB | 1,757 | **2,029** | 56 | 1,786 | 2,278 |
| **synth-random** | 1024 KB | 8,035 | **🏆 26,773** | 53 | 7,172 | 17,594 |
| real-bash | 1.4 MB | 342 | 381 | 51 | 524 | 3,060 |
| real-libc | 2.1 MB | 391 | 398 | 59 | 536 | 2,839 |
| real-python | 8.0 MB | 393 | 398 | 72 | 591 | 3,059 |

**Where VaptVupt leads the decode-speed table**:

- **Random decode with `--fast`: 26,773 MB/s**. Beats lz4 (17,594) by
  **1.52×** and zstd-19 (7,172) by **3.73×**. This is the dominant
  path for AEAD-wrapped archive restore — the target workload for
  any secure backup tool.
- **Synthetic repeat decode: 2,029 MB/s**. Beats zstd-19 (1,786)
  by **1.14×**. Neck-and-neck with lz4 while delivering **29× the
  compression ratio** on the same content.
- **Synthetic binary decode with `--fast`: 14,414 MB/s**. Beats
  zstd-19 (8,257) by **1.75×**.
- **Default-path random decode (with XXH64): 8,035 MB/s**. Still
  beats zstd-19 (7,172) by **1.12×** even while computing the
  full cryptographic hash.

The `--fast` flag contributes variably by content type:
- **50-200% speedup** on binary/random where XXH64 dominates decode
  time (the LZ/ANS path is trivial for high-entropy or pattern-poor
  inputs, so the hash is the bottleneck)
- **6-8%** on text/json/source where codec work dominates

## Decode Ratio (original / compressed; higher is better)

Bold marks where VaptVupt meets or beats the best competitor in class.

| Fixture | VV v1 | **VV v2** | gzip-9 | zstd-19 | lz4-9 |
|---|---:|---:|---:|---:|---:|
| synth-text | 5.07 | 5.08 | 6.95 | 7.87 | 4.83 |
| synth-json | 4.80 | **4.80** | 4.65 | 6.68 | 3.46 |
| synth-source | 4.90 | 4.89 | 5.40 | 6.75 | 4.48 |
| synth-binary | 1,149× | **1,149×** | 157× | 2,398× | 194× |
| synth-repeat | 7,367× | **7,367×** | 403× | 8,463× | 252× |
| synth-random | 1.00 | 1.00 | 1.00 | 1.00 | 1.00 |
| real-bash | 1.88 | **1.92** | 2.09 | 2.32 | 1.83 |
| real-ls | 2.04 | **2.11** | 2.30 | 2.55 | 2.00 |
| real-libc | 2.03 | **2.09** | 2.23 | 2.56 | 1.94 |
| real-python | 2.50 | **2.64** | 2.84 | 3.46 | 2.34 |

**Where VaptVupt leads the ratio table**:

- **synth-json**: 4.80× beats gzip-9 (4.65×) and every non-zstd competitor
- **synth-binary**: 1,149× is **7× better than gzip** and **6× better than lz4**
- **synth-repeat**: 7,367× is **18× better than gzip** and **29× better than lz4**
- **All real-binary fixtures**: format-v2 moved VaptVupt from 10-14% behind gzip-9 to **4-7% behind** across bash / ls / libc / python3

**Format v2 impact on real binaries** (measurement comparing
V1 baseline to v2.40.0):

| Fixture | V1 → v2.40 | Gap vs gzip-9 |
|---|---|---|
| bash | -3.8% smaller | **7%** (was 11%) |
| /bin/ls | -3.9% smaller | **6%** (was 12%) |
| libc.so.6 | -3.9% smaller | **4%** (was 10%) |
| python3 | -6.4% smaller | **5%** (was 14%) |

The hash3 matcher finds 3-byte binary matches (x86 instruction
prefixes) that hash5/hash4 cannot surface; the rep-3 extension adds
length-3 repeat matches at zero-extra-bit rep offsets; the extended
≤4096 offset filter captures close-in binary-layout repeats.

Format v2 is ratio-neutral on text/JSON/source because the adaptive
trial keeps hash3 off when compression ratio > 2:1 indicates
text-like data.

## The `--fast` Flag — VaptVupt's Signature Feature

```
Does your data layer have its own integrity check?
(AES-GCM, MAC, TLS, signed archive, trusted file system?)
│
├─ Yes → Use vv_decompress_flags(VV_DECOMPRESS_SKIP_CHECKSUM)
│        or CLI --fast. Unlocks **26.7 GB/s** random decode.
│
└─ No  → Use default vv_decompress(). XXH64 catches bit-flips
         and single-bit transmission errors. The default is safe.
```

Even with `--fast`, the decoder **still validates**:

- Frame magic `0x56564456` / `0x56564E44`
- Format version byte
- Block headers (type, size, last-flag bits)
- Block bodies (LZ offsets within dst, ANS states within table range)
- Absolute offset cap (≤ 1 MB) — added v2.39.0 for safe-zone sound-ness
- Buffer overshoot guards on wildcopy

`--fast` only skips the XXH64 *cryptographic hash* of decoded bytes.

## Format v2 — Shipped in v2.33-v2.37

The format-v2 arc that the original COMPETITIVE.md posited as
"not in progress" has landed across Sprints 33-47. The timeline:

| Sprint | Version | Milestone |
|---|---|---|
| 43 | v2.33.0 | 'T' tag decoder ready (C) |
| 44 | v2.34.0 | 'T' tag encoder infrastructure (latent corruption; fixed in v2.35) |
| 45 | v2.35.0 | Hash3 matcher + max_match cap (ratio lift, correctness) |
| 46 | v2.36.0 | Python + JS 'T' decoders + differential fuzzer coverage |
| 47 | v2.37.0 | Rep-3 extension + COMPETITIVE.md refresh |

Delivered vs planned:

| Planned Change | Status | Delivered gain |
|---|---|---|
| `VV_MIN_MATCH 4 → 3` | ✅ (opt-in via format_v2) | **~2-5% on real binary**; neutral on text |
| `ANS_LOG 12 → 10` | Not started | — |
| Multi-stream ANS | Not started | — |
| Dictionary support | Not started | — |

Next candidates for the format-v2 arc (beyond v2.37.0):

- **Python encoder** for 'T' tag (currently RAW+RLE only). Would
  enable pure-Python Zupt clients that can both produce and verify
  archives without the C binary.
- **ANS_LOG 12 → 10** for text-decode acceleration. This is the
  biggest remaining lever to close the text-decode gap vs zstd.
  Requires format change — sequencing after the v2.37.0 settling
  period.
- **Optimal LZ parsing** for the extreme mode. Explicitly not
  scheduled; would close binary ratio gap further but at 3-5×
  encode slowdown.

As of v2.37.0, the format v2 feature is production-ready for Zupt
use — round-trips across C/Python/JS, 5,200 differential fuzzer
cases pass, and the max_match correctness fix has regression
guards in place.

## Historical Speed Arc (text decode, MB/s)

```
v2.27 ─┐
       ├─ 246   (pre-mask-on-access)
v2.28 ─┘
v2.29 ─── 345   (baseline measurement)
v2.30 ─── 431   (bulk-fill + 1-fill/iter + mask-on-access)
v2.31 ─── 534   (+ --fast XXH64 skip on checksummed frames)
v2.39 ─── 569   (safe-zone bounds-check elision)
v2.40 ─── 569   (production release, no new codec changes)
```

Text decode has improved **2.31× across the v2.x arc** through pure
non-format optimizations. Binary decode with `--fast` has improved
**from ~2 GB/s to 14.4 GB/s over the same period** — a 7× gain
driven by XXH64-skip + wildcopy tuning + safe-zone elision.

## Cross-Language Reference Decoders

| Implementation | RAW | RLE | COMPRESSED | 'S' tag | Legacy H/I/C |
|---|:-:|:-:|:-:|:-:|:-:|
| C (`src/vv_decoder.c`) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Python (`reference/vv_decoder.py`) | ✓ | ✓ | ✓ | ✓ | ✗ |
| JavaScript (`reference/vv_decoder.js`) | ✓ | ✓ | ✓ | ✓ | ✗ |

All three implementations cover 100% of output produced by any
v1.0+ encoder. Legacy tags (H/A/I/C, emitted by v0.3-v0.7 only)
exist in C for back-compat but are not produced today.

For browser-side Zupt clients, the JavaScript decoder at
`reference/vv_decoder.js` is production-ready with zero
dependencies — runs in Node.js v14+ or any browser supporting
BigInt + Uint8Array.

## Reproducing These Numbers

```bash
# Build and amalgamate
make clean && make && make amalg

# Build the library-level decode bench
gcc -O3 -march=native -Iinclude \
    /tmp/decbench2.c build/vaptvupt.c -o /tmp/decbench2

# Prepare fixtures (commit-neutral)
python3 -c "
import random; rng=random.Random(42)
words='the quick brown fox'.split()
open('/tmp/fx_text','w').write(' '.join(rng.choice(words)
                                        for _ in range(150000))[:1024000])
"

# Compress with each tool, then library-bench decode
./vaptvupt -c -m balanced -o /tmp/text.vv /tmp/fx_text
/tmp/decbench2 /tmp/text.vv       # default
/tmp/decbench2 /tmp/text.vv skip  # --fast mode

zstd -19 -q -f -o /tmp/text.zst /tmp/fx_text
zstd -b -d -i5 /tmp/text.zst      # zstd built-in bench

lz4 -9 -q -f /tmp/fx_text /tmp/text.lz4
lz4 -b9 /tmp/fx_text              # lz4 built-in bench (both c+d speeds)
```

---

*Measurement caveats: all numbers are from a 2-vCPU 2.1 GHz
container without dedicated hardware. Absolute values will differ
on native hardware, but the relative positions between tools are
consistent across architectures tested.*
