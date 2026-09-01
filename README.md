# VaptVupt

**[Português (Brasil)](README.pt-BR.md)**

An LZ + tANS compression codec in C11. Zero runtime dependencies, an open
wire format with [Python](reference/vv_decoder.py) and
[JavaScript](reference/vv_decoder.js) decoders that reproduce current/default
encoder output byte-exactly, and a test suite that gates every release on
byte-identical output and sanitizer-clean corrupt-input handling. The C decoder
remains canonical for the full legacy H/A/I/C entropy surface; Python retains
limited A-tag coverage, while JavaScript intentionally omits legacy tags.

Version 2.65.9. The codec library and CLI are available under
GPL-3.0-or-later or, for controlled first-party rights, a separate signed
commercial agreement. The broader VaptVupt application uses a distinct AGPL
public option. See `NOTICE`; `LICENSE-COMMERCIAL` is not itself a grant.

## Where it stands

### v2.65.9 deterministic generated-v1 suite (measured 2026-09-01)

Fresh subprocess measurements on an Intel Core i7-13700HX, Linux 7.2.2,
gcc 14.3, pinned to core 2. Each cell is the median of 7 measured runs after
1 warm-up; zstd 1.5.6 ran single-threaded and lz4 is 1.10. Every decode was
verified against the generated input by SHA-256. Cells are
`ratio @ encode/decode MB/s`; ratio = raw / compressed.

| file | vv-fast | vv-balanced | lz4-1 | zstd-1 | zstd-3 |
|---|---|---|---|---|---|
| text.txt | 3.707 @143.4/395.7 | 7.055 @61.9/343.7 | 2.907 @288.8/387.9 | 5.768 @206.2/342.5 | 6.198 @187.6/348.9 |
| records.jsonl | 3.142 @139.2/424.0 | 5.707 @53.9/355.7 | 3.505 @279.6/398.6 | 7.071 @221.2/354.5 | 6.521 @190.8/354.8 |
| records.bin | 1.347 @80.3/408.4 | 2.016 @18.7/253.7 | 1.371 @267.1/402.5 | 1.934 @193.9/343.6 | 2.183 @127.2/307.2 |
| random.bin | 1.000 @360.1/445.2 | 1.000 @267.2/438.2 | 1.000 @397.9/374.6 | 1.000 @332.3/351.6 | 1.000 @296.6/355.8 |

These four deterministic fixtures show workload-dependent trade-offs, not a
universal ordering. On this run `vv-balanced` has the best text ratio and a
small decode lead over zstd-1/3 on generated JSON, but zstd compresses both
text families much faster and has the better JSON ratio. Record-binary results
split by level, while all codecs store the random fixture effectively raw.
`vv-fast` beats lz4-1 on text ratio but not on JSON or record-binary ratio, and lz4
compresses those fixtures substantially faster. Measure the intended workload.

The suite is generated without external corpus files and requires the full
vv-fast/vv-balanced/lz4-1/zstd-1/zstd-3 matrix. JSON records host/tool
provenance; CSV records fixture hashes and measurements. The run fails on a
command error, missing required codec, or SHA-256 decode mismatch. Reproduce
with an lz4 1.10 binary first on `PATH`:

```sh
PATH=/path/to/lz4-1.10/bin:$PATH taskset -c 2 \
  python3 bench/competitive.py --generated-suite --vv ./vaptvupt \
  --runs 7 --warmups 1 --csv generated-v1.csv --json generated-v1.json
```

Full data and methodology are in
[bench/COMPARISON.md](bench/COMPARISON.md).

### Historical 11-file corpus excerpt (measured 2026-07; output revalidated 2026-09)

Single-core AVX2 x86-64, gcc 13 `-O3 -flto`, zstd 1.5.6 `--single-thread`,
lz4 1.10.0. CLI subprocess timing, best of 3, 11-file mixed corpus
(logs/JSON/CSV/XML/markdown/source/ELF/float-records/struct-records plus
random and pure-repetition controls). This compact summary and the full table
in [bench/COMPARISON.md](bench/COMPARISON.md) are distinct July timing runs in
the same stated benchmark family: ratios agree, while throughput cells retain
each run's own measurements. The comparison guide carries the full corpus and
detailed methodology.
Cells are `ratio @ encode/decode MB/s`; ratio = raw / compressed, higher is
better.

| file | vv-balanced | vv-extreme | zstd-3 | zstd-9 | lz4-1 |
|---|---|---|---|---|---|
| access.log | 6.322 @46/375 | 8.045 @2/459 | 6.496 @241/329 | 8.238 @58/551 | 4.070 @195/209 |
| data.json | **5.435 @68/552** | **6.078 @2/484** | 5.241 @150/603 | 5.801 @62/674 | 2.889 @329/216 |
| table.csv | **3.210 @26/320** | 3.728 @1/290 | 3.189 @117/292 | 3.811 @35/318 | 2.053 @242/366 |
| catalog.xml | 11.195 @51/382 | **13.559 @2/505** | 11.897 @205/228 | 12.563 @63/452 | — |
| text.md | 2.772 @29/297 | 2.812 @5/204 | 2.872 @40/199 | 3.146 @26/195 | — |
| sensors.bin (float records) | **1.398 @13/237** | — | 1.176 @119/134 | — | — |
| structs.bin (24-B records) | **1.719 @13/314** | — | 1.595 @86/250 | 1.600 @41/268 | — |

Where each side wins:

- `balanced` beats zstd-3 on ratio for JSON, CSV, and record-style binary
  (sensors 1.398 vs 1.176, +19%; structs 1.719 vs 1.595, +8%; structs even
  beats zstd-9's 1.600) and roughly ties it on logs.
- zstd-3 still wins xml/text/source ratio by 2–6%, and remains 1.5–4×
  faster at *compressing* text-family input.
- zstd-19 still wins maximum ratio on record binary (sensors 1.469,
  structs 1.902) at single-digit MB/s; `balanced` gets most of the way
  there at about twice that speed.
- `extreme` beats zstd-3 broadly and now takes xml and json from zstd-9
  on this corpus (xml 13.559 vs 12.563, json 6.078 vs 5.801); zstd-9
  keeps text/source/logs and zstd-19 keeps the maximum-ratio tier.
- `fast` beats lz4-1 on ratio on 7 of the 11 corpus files (e.g. access.log
  4.355 @44/103 vs 4.070 @195/209), and loses on xml and pure repetition —
  while lz4 remains several times faster at compressing.
- Decode leans VaptVupt's way at the default level: `balanced` decodes
  faster than zstd-3 in this run on logs (375 vs 329 MB/s), CSV (320 vs
  292), text (297 vs 199), and records (314 vs 250); on JSON the two are
  close and zstd-3 edges it (552 vs 603).
- Incompressible input encodes at 125–185 MB/s across modes in this run
  (was ~31 before v2.61.0's default skip acceleration and early-RAW bail);
  zstd-1 does 201, lz4-1 433 on the same 1 MiB random file.

This historical table retains the v2.65.0/v2.65.6 11-file corpus measurement
because the same 11 files reproduced their recorded compressed sizes on
v2.65.9. The 2026-09 release validation rechecked output sizes and correctness; its
throughput cells remain tied to the stated 2026-07 benchmark host and are not
fresh v2.65.9 timings. It complements rather than overrides the generated-v1
suite above.

Recent releases, newest first:

- **v2.65.9** — builds sequence tANS decode tables directly, removing 4 KiB
  of per-block sequence-table scratch (52 KiB to 48 KiB). Paired pinned
  in-process decode measurements improved +0.40% on text and +1.21% on JSON
  (about +0.80%
  geometric mean): modest, workload-dependent gains with identical wire
  output. Streaming decode now applies the x86 or ARM64 BCJ inverse exactly
  once at frame completion, after checksum validation or after the final
  checksumless block. The release also rejects invalid one-shot API mode enums
  and simultaneous architecture filters with `VV_ERR_PARAM`; the streaming
  encoder rejects invalid modes and BCJ options rather than silently ignoring
  filters it cannot apply. It corrects `-A 0` help and adds the deterministic
  generated-v1 benchmark matrix. A rare
  unrepresentable SEQ candidate (a >65,535-byte literal run before a later
  match) now falls back losslessly instead of emitting an undecodable frame;
  `test_seq_v2` covers the direct case and end-to-end reproducer (21/21). The
  Python and JavaScript references now handle trailing LL-only entries with a
  C-equivalent bound and perform exact x86/AArch64 BCJ inverses. The private
  one-shot BCJ input copy is explicitly scrubbed before it is freed.
- **v2.65.8** — decoder security and API hardening: closes an AVX2
  truncated-offset read after extended literals, rejects NULL streaming
  chunks, enforces stable streaming output buffers, and refreshes the
  release-facing comparison metadata.
- **v2.65.7** — security and streaming performance: repaired the SEQ
  safe-zone bound for a combined maximum literal/match sequence, validates
  the declared 10..24 window, restores streaming `accel=0` auto behavior,
  and removes quadratic streaming-decode buffer shifts. The test and CI
  gates now propagate sanitizer, OOM, and fuzz failures.

- **v2.65.6** — documentation refresh: regenerated the head-to-head
  measurement, corrected stale release-artifact version numbers, and
  brought the "recent releases" summary current (docs only; no code
  change, output byte-identical).
- **v2.65.5** — test infrastructure: a guard in `make test` forces the
  default HUFFMAN4 literal format and requires both the Python and
  JavaScript reference decoders to decode it byte-exactly.
- **v2.65.4** — fixed the broken single-file amalgamation build (it
  omitted the BCJ source added in v2.60.x) and completed both reference
  decoders with HUFFMAN4 (`lit_fmt=4`) support, so they now decode
  default output byte-exactly.
- **v2.65.2 / v2.65.3** — extreme-mode encode ~2x faster (byte-identical
  collector speedups); capped a per-block virtual allocation in the
  extreme prepass (memory hygiene for overcommit-strict systems).
- **v2.63.0-v2.65.0** — the extreme-mode optimal parser gained
  repeat-offset pricing, entropy-aware literal pricing, and
  residual-literal pricing from a greedy prepass. Net effect vs
  v2.62.0: xml -13%, json -7%, csv -2%; extreme now takes **xml and
  json from zstd-9** on this corpus, and the pure-repetition quirk is
  gone. (v2.65.1 evaluated offset-code pricing and shipped it off by
  default -- a measured negative result, documented so it is not
  re-tried blind.)
- **v2.61.0-v2.62.0** — the encoder speed/ratio program: default-on
  skip acceleration (incompressible input 31 -> 200+ MB/s), fast-mode
  decode +55%, Huffman4 decode refill hoist (+13-15% balanced decode),
  per-block allocation consolidation, and two latent-corruption fixes
  (zero-match SEQ blocks, a Path A/B scratch overlap).

`extreme` still targets ratio, not speed (~2 MB/s on its optimal-parse
path since v2.65.2's collector speedups; was ~1).

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
Reference decoders that reproduce current/default C encoder output byte-exactly
are implemented in [Python](reference/vv_decoder.py) and
[JavaScript](reference/vv_decoder.js); the differential fuzzer cross-checks C
against Python on every `make test`. The C decoder remains canonical for the
complete legacy entropy surface.

Header `flags`: bit0 = XXH64 footer present, bit1 = reserved (must be zero),
bit2 = x86 BCJ filter applied, bit3 = AArch64 BCJ filter applied. Offsets are
2 bytes for window log <= 16, 3 bytes for <= 24 (the 16 MiB maximum). The two
BCJ bits are mutually exclusive in valid encoder output. The C one-shot,
streaming, and frame-info paths reject a frame that sets both. Decoders validate
a present checksum before applying the selected inverse once; without a
checksum they apply it once after the final block.

## Testing

`make test` runs:

- 22 C test binaries (roundtrip, Huffman, tANS, streaming, edge cases, adversarial
  safe-zone, DoS reproducers, BCJ filter, and more).
- The Python reference decoder against current C output, plus the JavaScript
  reference decoder when a working `node` runtime is available. Current S/T,
  HUFFMAN4, and x86/AArch64 BCJ output are covered byte-exactly. The C decoder
  remains canonical for legacy H/A/I/C; Python retains limited A-tag support,
  while JavaScript intentionally omits those legacy tags.
- A differential fuzzer (5200 cases, fixed seed) cross-checking C and Python.
- A reference-decoder guard that forces default-format (HUFFMAN4)
  blocks and requires both the Python and JavaScript references to
  decode them byte-exactly when Node is available (added v2.65.5; fails if the
  format is not exercised).
- The negative corpus (malformed frames must be rejected, not crash).
- An OOM-robustness sweep that fails each allocation point reached by its
  baseline compress/decompress fixture in turn and asserts a clean error or
  success. AddressSanitizer detected no leak or use-after-free on the swept
  paths. The harness first verifies that the randomized baseline frame
  roundtrips before injecting any failure.
- The ratio gate (every fixture within +/- 0 bytes of the committed
  baseline) and an informational decode-speed gate.
- The competitive harness self-test and the `-w` CLI test. Release validation
  separately runs the full generated-v1 matrix, verifies every decode by
  SHA-256, and emits reproducibility metadata.
- Direct-vs-legacy tANS decode-table equivalence and whole/split streaming BCJ
  roundtrips for x86 and ARM64 with checksums both enabled and disabled.
- `test_seq_v2` 21/21, including rejection and lossless fallback for an
  oversize nonterminal literal run; both reference decoders consume trailing
  LL-only entries with C-equivalent bounds, reject dual-BCJ headers, and invert
  x86/AArch64 BCJ output with checksums on and off.

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
reference/  Python and JavaScript decoders for current/default encoder output
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

This repository's first-party codec library and CLI are available under
GPL-3.0-or-later or a separate written commercial agreement signed by the
applicable copyright holder and licensee. The public commercial notice does
not grant proprietary rights, and separately noticed/generated material keeps
its own license. The broader VaptVupt tool uses an AGPL public option with its
own commercial path. Contact `sac@securityops.co`. "In Code We Trust."
