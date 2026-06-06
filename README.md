# VaptVupt

An LZ + tANS compression codec in C11. Zero runtime dependencies, an open
wire format with byte-exact reference decoders in Python and JavaScript, and
a test suite that gates every release on byte-identical output and
sanitizer-clean corrupt-input handling.

Version 2.60.0. License: GPL-3.0-or-later (commercial license available:
sac@securityops.co).

## Where it stands

Numbers are measured on a single-core Intel Xeon @ 2.80 GHz (gcc 13, AVX2)
against gzip 1.12, zstd 1.5.5, lz4 1.9.4, xz 5.4.5. Full data and the
reproduction command are in [bench/COMPARISON.md](bench/COMPARISON.md).

Ratio (raw / compressed, higher is better):

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

Throughput, dickens, in-process best-of-7 (MB/s):

| codec | ratio | encode | decode |
|---|---|---|---|
| lz4-1 | 1.585 | 247 | 669 |
| zstd-1 | 2.391 | 130 | 469 |
| zstd-3 | 2.778 | 92 | 346 |
| vv-fast | 1.992 | 64 | 588 |
| vv-balanced | 2.647 | 10 | 410 |
| vv-extreme | 2.992 | 0.3 | 470 |

Decode is competitive: `extreme` decode (~470 MB/s) is on par with zstd-1
while compressing better. Encode is the weak axis — `balanced` encodes
roughly 14x slower than zstd-1 because it walks a depth-24 match chain to buy
the ratio above; that tradeoff is documented and is not a free lever to
recover (CHANGELOG v2.53.3).

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

Returns diminish past `-D 48`. Note the honest limit: encode speed is the
codec's weak axis and is bound by per-position overhead, not chain depth —
even `-m fast -D 1` reaches only ~79 MB/s (vs zstd-1 at ~130 and lz4 at ~247).
Decode is competitive (vv-extreme ≈ zstd-1). See `bench/COMPARISON.md`.

`-A N` / `--accel N` (0..64; 0 = off, the default) enables lz4-style
position-skip acceleration: after a run of consecutive no-match positions the
parser advances by more than one byte, skipping the hash/insert work on
unmatchable regions. It is opt-in (default `0` is byte-identical to prior
releases), most useful with `-m fast`, and its output is decodable by any
decoder. On incompressible or already-compressed input the speedup is large;
on compressible input the cost is a small ratio loss. Measured (fast mode):

| input | `-A 0` | `-A 8` | `-A 32` |
|---|---|---|---|
| random 8 MiB | 1.000 @ 60 MB/s | 1.000 @ 547 MB/s | 1.000 @ 569 MB/s |
| gzip'd text | 1.000 @ 53 MB/s | 1.000 @ 500 MB/s | 1.000 @ 516 MB/s |
| dickens (text) | 1.992 @ 69 MB/s | 1.988 @ 68 MB/s | 1.930 @ 72 MB/s |

Use a moderate `-A 8` for mixed/already-compressed data; leave it off (`0`) for
normal compressible input where the default parse already skips via matches.

`--no-rep` disables rep-match probing in the parser. In fast mode (which has no
entropy stage, so a rep match's short-offset code is not actually cheaper) rep
mostly perturbs the greedy parse, so dropping it is a measured **ratio**
improvement on text/structured data, with a small ratio cost on some binaries.
It is opt-in (default keeps rep enabled, byte-identical). Measured (fast mode):

| input | rep (default) | `--no-rep` | Δ ratio | Δ speed |
|---|---|---|---|---|
| recs.ndjson | 4.851 | 5.027 | +3.6% | −8% |
| app.log | 3.306 | 3.360 | +1.6% | −1% |
| samba | 3.117 | 3.142 | +0.8% | +4% |
| dickens | 1.992 | 1.992 | 0% | +8% |
| mozilla | 2.114 | 2.098 | −0.8% | — |
| libc.bin | 1.757 | 1.747 | −0.6% | — |

So `--no-rep` is a fast-mode ratio knob for text/structured data (logs, CSV,
JSON); the speed effect is data-dependent (faster on text, slower on highly
repetitive input where rep was cheaply skipping chain walks). It is not made
the default because it regresses some binaries, which the ratio gate forbids.
On balanced/extreme (which do entropy-code, making rep offsets genuinely
cheaper) rep stays beneficial — `--no-rep` is intended for `-m fast`.

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
corrupt-input sweep (12,000+ cases) clean under both. The build is
`-Wall -Wextra -Werror`.

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

GPL-3.0-or-later. A commercial license is available for use the GPL does not
permit; contact sac@securityops.co. "In Code We Trust."
