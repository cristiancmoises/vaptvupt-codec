# VaptVupt

An LZ + tANS compression codec in C11. Zero runtime dependencies, an open
wire format with byte-exact reference decoders in Python and JavaScript, and
a test suite that gates every release on byte-identical output and
sanitizer-clean corrupt-input handling.

Version 2.64.0. License: this repository (the vaptvupt-codec library and
CLI) is GPL-3.0-or-later; the VaptVupt tool built on it (formerly Zupt) is
dual-licensed AGPL-3.0 + commercial (sac@securityops.co).

## Where it stands

### v2.64.0 head-to-head (measured 2026-07)

Single-core AVX2 x86-64, gcc 13 `-O3 -flto`, zstd 1.5.6 `--single-thread`,
lz4 1.10.0. CLI subprocess timing, best of 3, 11-file mixed corpus
(logs/JSON/CSV/XML/markdown/source/ELF/float-records/struct-records plus
random and pure-repetition controls).
Full data and methodology in [bench/COMPARISON.md](bench/COMPARISON.md).
Cells are `ratio @ encode/decode MB/s`; ratio = raw / compressed, higher is
better.

| file | vv-balanced | vv-extreme | zstd-3 | zstd-9 | lz4-1 |
|---|---|---|---|---|---|
| access.log | 6.322 @72/396 | 8.029 @1/425 | 6.496 @231/413 | 8.238 @57/435 | 4.070 @311/269 |
| data.json | **5.435 @67/528** | 5.659 @1/513 | 5.241 @223/447 | 5.801 @62/626 | 2.889 @394/453 |
| table.csv | **3.210 @32/376** | 3.699 @1/321 | 3.189 @91/272 | 3.811 @42/392 | 2.053 @257/338 |
| catalog.xml | 11.195 @108/505 | **13.303 @1/462** | 11.897 @254/243 | 12.563 @66/437 | — |
| text.md | 2.772 @28/286 | 2.856 @4/217 | 2.872 @95/219 | 3.146 @36/130 | — |
| sensors.bin (float records) | **1.398 @15/204** | — | 1.176 @240/348 | — | — |
| structs.bin (24-B records) | **1.719 @16/279** | — | 1.595 @83/269 | 1.600 @41/338 | — |

Where each side wins:

- `balanced` beats zstd-3 on ratio for JSON, CSV, and record-style binary
  (sensors 1.398 vs 1.176, +19%; structs 1.719 vs 1.595, +8%; structs even
  beats zstd-9's 1.600) and roughly ties it on logs.
- zstd-3 still wins xml/text/source ratio by 2–6%, and remains 1.5–4×
  faster at *compressing* text-family input.
- zstd-19 still wins maximum ratio on record binary (sensors 1.469,
  structs 1.902) at single-digit MB/s; `balanced` gets most of the way
  there at about twice that speed.
- `extreme` beats zstd-3 broadly and takes xml from zstd-9 (13.304 vs
  12.563 in this run, a 5.6% size margin); zstd-9 keeps text/source/
  json/logs and zstd-19 keeps the maximum-ratio tier. Extreme targets
  ratio, not speed (≈1 MB/s on its optimal-parse path).
- `fast` beats lz4-1 on ratio on 7 of the 11 corpus files (e.g. access.log
  4.355 @118/320 vs 4.070 @259/342), ties the two incompressible controls,
  and loses on xml and pure repetition — while lz4 remains 1.5–4× faster
  at compressing.
- Decode now leans VaptVupt's way at the default level: `balanced` decodes
  faster than zstd-3 in this run on logs (406 vs 354 MB/s), CSV (243 vs
  168), text (210 vs 117), records (271 vs 147), and JSON (321 vs 273).
- Incompressible input encodes at 230–325 MB/s across modes in this run
  (was ~31 before v2.61.0's default skip acceleration and early-RAW bail);
  zstd-1 does 196, lz4-1 362 on the same 1 MiB random file.

v2.61.0 also fixed two latent encoder bugs (zero-match SEQ blocks emitted no
LL bitstream; a Path A/B scratch-buffer overlap could corrupt block output
before winner selection) — see [CHANGELOG.md](CHANGELOG.md).

v2.61.1 raises fast-mode decode ~55% (1540 → 2430 MB/s in-process on a
13 MB mixed buffer) via a 16-byte literal wildcopy in the token loop, with
byte-identical output; balanced/extreme decode is unchanged. It also
hardens the SEQ decoder's table validation — see
[CHANGELOG.md](CHANGELOG.md) and [SECURITY.md](SECURITY.md).
v2.61.2 consolidates per-block scratch allocations (encoder 7→2 mallocs
per block, decoder 2→1) with byte-identical output and unchanged
throughput.
v2.64.0 adds entropy-aware literal pricing to the same parser (per-byte
prices from the block histogram, blended 50/50 with the flat prior):
extreme gains another 2.6% on xml, 1.8% on json and logs, and gives back
1.3% on plain text — a measured, documented trade. The table above is a
fresh v2.64.0 measurement.
v2.63.0 gives the extreme-mode optimal parser real repeat-offset pricing
(per-position rep histories matching the wire's per-block rep state):
xml −12.6%, csv −2.1%, logs −1.0% at unchanged speed, and the
pure-repetition quirk (567 B vs balanced's 165 B) is gone. The table
above is a fresh v2.63.0 measurement.
v2.62.0 hoists the Huffman4 literal decoder's per-symbol refill into one
bulk refill per 3-symbol round: balanced-mode decode +13–15% on
literal-heavy and text content (in-process: sensors 243→281 MB/s, text
468→529), output bytes unchanged. The table above is a fresh v2.62.0
measurement.

### v2.52-era Silesia measurement

Earlier numbers on the Silesia corpus, measured on a single-core Intel Xeon
@ 2.80 GHz (gcc 13, AVX2) against gzip 1.12, zstd 1.5.5, lz4 1.9.4, xz 5.4.5
(kept for continuity; see [bench/COMPARISON.md](bench/COMPARISON.md)):

| file | gzip-9 | zstd-3 | zstd-19 | xz-9 | vv-extreme |
|---|---|---|---|---|---|
| xml | 8.071 | 8.363 | 11.751 | 11.793 | 9.647 |
| reymont | 3.640 | 3.413 | 4.918 | 5.031 | 3.863 |
| dickens | 2.646 | 2.778 | 3.576 | — | 2.992 |
| libc.so (x86, `--bcj`) | 2.230 | 2.150 | 2.556 | 2.759 | 2.251 |

- On text and structured text, `extreme` beats gzip-9, zstd-1, zstd-3, and
  every lz4 level. It does not beat zstd-19 or xz-9, which target maximum
  ratio at much lower encode speed.
- On x86 machine code, the opt-in `--bcj` filter raises `extreme` past
  gzip-9 (2.251 vs 2.230 on libc); without it, vv trails gzip-9 on binary.
- On AArch64 machine code, the opt-in `--bcj-arm64` filter (BL + ADRP) adds
  +2.4–4.6%; it narrows the gap to gzip-9 but does not close it.
- `--auto-filter` picks the right filter from the file's ELF/PE/Mach-O header,
  so the binary-ratio wins above need no manual architecture flag.
- zstd-19 and xz-9 win on binary ratio overall; vv does not target that tier.

Throughput, dickens, in-process best-of-7 (MB/s, v2.52-era — predates the
v2.61.0 encoder speed work):

| codec | ratio | encode | decode |
|---|---|---|---|
| lz4-1 | 1.585 | 247 | 669 |
| zstd-1 | 2.391 | 130 | 469 |
| zstd-3 | 2.778 | 92 | 346 |
| vv-fast | 1.992 | 64 | 588 |
| vv-balanced | 2.647 | 10 | 410 |
| vv-extreme | 2.992 | 0.3 | 470 |

Decode is competitive: `extreme` decode is on par with zstd-1 while
compressing better. Encode remains the weaker axis — `balanced` is typically
1.5–4× slower than zstd-3 on text because it walks a depth-24 match chain to
buy the ratio above — but since v2.61.0 it is no longer pathological on
incompressible or record-style input (see the head-to-head table above).

## Build

```sh
make            # -O3 -flto, default build
make test       # full suite (see Testing)
```

Requires a C11 compiler with AVX2 (gcc 13+/clang). Build variants:

```sh
make ENABLE_THREADS=1   # multi-threaded encode
make pgo                # profile-guided build (needs a corpus in /tmp/silesia)
```

Debian/Ubuntu: `apt install build-essential`. Arch: `pacman -S base-devel`.
Fedora: `dnf install gcc make`. The build uses only the C standard library.

## Use

CLI:

```sh
vaptvupt -c -m extreme -o file.zupt file     # compress (modes: fast, balanced, extreme)
vaptvupt -d -o file.out file.zupt           # decompress
vaptvupt -c -m balanced -w 24 -o big.zupt big # larger window (long-range data)
vaptvupt -c -m extreme --bcj -o code.zupt prog # x86 BCJ filter (machine code)
vaptvupt -c -m extreme --bcj-arm64 -o a.zupt a  # AArch64 BCJ filter (BL + ADRP)
vaptvupt -c -m extreme --auto-filter -o o.zupt f # detect ELF/PE/Mach-O, pick the filter
```

`-w N` sets the window log (10-24 = 1 KiB-16 MiB, 0 = auto). `--bcj`
(`--filter x86`) and `--bcj-arm64` (`--filter arm64`) apply the reversible
x86 and AArch64 branch filters; they are mutually exclusive. `--auto-filter`
(`--filter auto`) sniffs the input's executable header and selects the
matching filter automatically, or none if unrecognised. `--fast` skips the
XXH64 footer. All of `-w`, `--bcj`, `--bcj-arm64`, and `--auto-filter` are
opt-in and do not change default output.

Compressed files use the `.zupt` extension by default (`vaptvupt -c file`
writes `file.zupt`; `vaptvupt -d file.zupt` writes `file`). The on-disk frame
format is unchanged, so legacy `.vv` files still decode and `vaptvupt`
recognises a frame by its header regardless of the filename.

`-D N` / `--depth N` overrides the match-finder chain depth (1..4096; 0 keeps
the per-mode default of fast=4, balanced=24, extreme=256), trading encode
speed for ratio along a smooth monotonic curve. It is opt-in and decodable by
any decoder; `-D 0` is byte-identical to the mode default. Measured on dickens
(balanced):

| `-D` | ratio | encode MB/s |
|---|---|---|
| 4 | 2.520 | 15.1 |
| 8 | 2.574 | 13.3 |
| 16 | 2.625 | 11.0 |
| 24 (default) | 2.647 | 9.6 |
| 48 | 2.665 | 7.7 |
| 128 | 2.670 | 5.4 |

Returns diminish past `-D 48`. Note the honest limit (v2.60-era numbers,
before the v2.61.0 speed work): encode speed is bound by per-position
overhead, not chain depth — `-m fast -D 1` reached ~79 MB/s (vs zstd-1 at
~130 and lz4 at ~247). v2.61.0 narrows but does not close that gap on
compressible text. Decode is competitive (vv-extreme ≈ zstd-1). See
`bench/COMPARISON.md`.

`-A N` / `--accel N` (0..64) controls lz4-style position-skip acceleration:
after a run of consecutive no-match positions the parser advances by more
than one byte, skipping the hash/insert work on unmatchable regions.
**Since v2.61.0 the default `0` means auto**: fast mode uses ramp factor 2,
balanced/extreme use 1 with the stride capped at 8; the ramp resets on every
match, so compressible regions are parsed exactly as before. An explicit
`-A` value is honored unchanged. The encoder also stores a block raw
immediately when 128 KB pass without a single match (early-RAW bail),
instead of finishing a doomed parse. Output remains decodable by any
decoder. On incompressible or already-compressed input the speedup is large
(~31 → 120–190+ MB/s); on compressible input the auto setting's ratio cost
is negligible-to-zero (the ratio gate holds at ±0 bytes). Measured before
the new default, in fast mode (`-A 0` here is the old hard-off):

| input | `-A 0` (old off) | `-A 8` | `-A 32` |
|---|---|---|---|
| random 8 MiB | 1.000 @ 60 MB/s | 1.000 @ 547 MB/s | 1.000 @ 569 MB/s |
| gzip'd text | 1.000 @ 53 MB/s | 1.000 @ 500 MB/s | 1.000 @ 516 MB/s |
| dickens (text) | 1.992 @ 69 MB/s | 1.988 @ 68 MB/s | 1.930 @ 72 MB/s |

A larger explicit value (e.g. `-A 8`+) remains useful with `-m fast` on
mixed/already-compressed data where maximum skip throughput matters.

Format v2 is now adaptive: since v2.61.0, balanced and extreme auto-enable
`--format-v2` (min_match=3, `'T'` entropy blocks) when the input is detected
as binary — measured +19% ratio on float-record data and +8% on struct
records. Output produced this way requires a v2.33.0+ decoder; library users
who must stay readable by older decoders can set
`vv_options_t::compat_v246_5_decoder`, which suppresses the auto-enable (and
lit_fmt=4). Explicit `--format-v2` still forces min_match=3 for any input.

`--no-rep` disables rep-match probing in the parser. In fast mode (which has no
entropy stage, so a rep match's short-offset code is not actually cheaper) the
greedy rep preference can block better chain matches. The effect is **strongly
data-dependent and specialized**: it helps data with many short repeated
structures (logs, JSON, CSV, delimited records), and hurts most general text
and binary. It is opt-in (default keeps rep enabled, byte-identical). Measured
in fast mode:

| input | rep (default) | `--no-rep` | Δ ratio |
|---|---|---|---|
| recs.ndjson | 4.851 | 5.027 | **+3.5%** |
| data.csv | 1.997 | 2.055 | **+2.8%** |
| app.log | 3.306 | 3.360 | **+1.6%** |
| samba | 3.117 | 3.142 | +0.8% |
| xml | 5.254 | 5.210 | −0.85% |
| mozilla | 2.114 | 2.098 | −0.76% |
| nci | 6.660 | 6.611 | −0.74% |
| ooffice | 1.598 | 1.593 | −0.31% |
| dickens | 1.992 | 1.992 | ~0% |

On the full Silesia corpus `--no-rep` is **net-negative** (7 of 12 files
regress), so it is not a general improvement and not a default candidate — it is
a targeted knob for log/JSON/CSV-style workloads with heavy short-repeat
structure. Speed is also data-dependent (dickens +8%, samba +4%, but
recs.ndjson −8%, where rep was cheaply skipping chain walks). On
balanced/extreme, which entropy-code (making rep offsets genuinely cheaper), rep
stays beneficial; `--no-rep` is intended for `-m fast` on structured data only.

Library (one-shot):

```c
#include "vaptvupt.h"

vv_options_t opt;
vv_default_options(&opt);            /* balanced, checksum on */
opt.mode = VV_MODE_EXTREME;

int64_t n = vv_compress(src, src_len, dst, dst_cap, &opt);
if (n < 0) { /* negative is an error code */ }

int64_t m = vv_decompress(dst, (size_t)n, out, out_cap);
```

`vv_compress_bound(src_len)` gives a safe destination capacity. Streaming
(`vv_cstream_*`), multi-threaded (`vv_compress_mt`), and context-reuse APIs
are declared in `include/vaptvupt.h`.

## Wire format

The format is specified in [FORMAT.md](FORMAT.md): a 16-byte frame header
(magic, version, flags, mode hint, window log, content size), one or more
blocks (raw, RLE, token, or entropy-coded), and an optional XXH64 footer.
Reference decoders that reproduce the C decoder byte-for-byte live in
`reference/vv_decoder.py` and `reference/vv_decoder.test.js`; the differential
fuzzer cross-checks C against Python on every `make test`.

Header `flags`: bit0 = XXH64 footer present, bit1 = dictionary frame,
bit2 = x86 BCJ filter applied, bit3 = AArch64 BCJ filter applied. Offsets are
2 bytes for window log <= 16, 3 bytes for <= 24 (the 16 MiB maximum).

## Testing

`make test` runs:

- 20 C suites (roundtrip, Huffman, tANS, streaming, edge cases, adversarial
  safe-zone, DoS reproducers, BCJ filter, and more).
- The Python and JavaScript reference decoders against the C output.
- A differential fuzzer (5200 cases, fixed seed) cross-checking C and Python.
- The negative corpus (malformed frames must be rejected, not crash).
- An OOM-robustness sweep that fails each allocation site in compress and
  decompress in turn and asserts the codec never crashes (returns a clean
  error or succeeds). Under AddressSanitizer the same sweep also proves no
  leak or use-after-free on any allocation-failure path.
- The ratio gate (every fixture within +/- 0 bytes of the committed
  baseline) and an informational decode-speed gate.
- The competitive harness self-test and the `-w` CLI test.

Corrupt-input handling is checked under AddressSanitizer and
UndefinedBehaviorSanitizer; releases that touch the decoder run a
corrupt-input sweep (12,000+ cases) clean under both. The v2.61.1 release
was additionally validated with the full 22-suite run under
`-fsanitize=address,undefined -fno-sanitize-recover=all` and an
ASan+UBSan+LeakSanitizer roundtrip sweep (11 corpus files × 3 modes,
byte-exact, no leaks). The build is `-Wall -Wextra -Werror`.

Selected code is additionally formally verified with CBMC (`make verify`, or
`sh verification/verify.sh`): for all inputs up to a bounded size, the x86 and
AArch64 BCJ filters are proven memory-safe and lossless
(`inverse(forward(x)) == x`); the executable-header detector is proven
memory-safe on arbitrary/truncated input; the decoder's variable-length
integer reader (`read_ext_len`) is proven never to read past the input end;
and the block-header pack/unpack is proven a lossless round trip with
in-range accessors for any header. A second tier of Frama-C/Eva
abstract-interpretation analyses (run with `frama-c-base`) independently
confirms `read_ext_len` and the block-header accessors raise no runtime error
for any input. See [verification/README.md](verification/README.md).

## Repository layout

```
src/        codec (encoder, decoder, tANS, Huffman, BCJ, xxh64, API, CLI)
include/    public header (vaptvupt.h) and internal headers
reference/  byte-exact Python and JavaScript reference decoders
tests/      C suites + Python fuzzer/gate/CLI tests + OOM-robustness sweep
bench/      competitive harness (competitive.py) and COMPARISON.md
verification/ CBMC formal-verification harnesses for the BCJ filters
```

## Documentation

- [FORMAT.md](FORMAT.md) - wire-format specification.
- [bench/COMPARISON.md](bench/COMPARISON.md) - measured ratio/throughput vs
  gzip, zstd, lz4, xz, plus the `--bcj` and `-w` results.
- [SECURITY.md](SECURITY.md) - threat model and what the codec does and does
  not protect against.
- [CHANGELOG.md](CHANGELOG.md) - per-release history.
- [DEPLOY.md](DEPLOY.md) - release procedure.

## License

This repository — the vaptvupt-codec library and CLI — is licensed
GPL-3.0-or-later. The VaptVupt tool built on this codec (formerly Zupt) is
dual-licensed: AGPL-3.0 or a commercial license for uses the AGPL does not
permit; contact sac@securityops.co. "In Code We Trust."
