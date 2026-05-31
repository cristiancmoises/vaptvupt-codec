# VaptVupt v2.50.5 Performance

**SPEED PROGRAM final measurement, Sprint 32.**
**Measured on:** Linux x86_64, gcc 13.3, default `-O3 -flto` build (NOT `make perf`).
**Corpus:** full Silesia (12 fixtures, ~211 MB total).
**Methodology:** best of 3 wall-clock runs per measurement; subprocess fork/exec overhead included (same overhead for both tools).

This document supersedes any prior performance claims. It exists to make
VaptVupt's actual position vs zstd unambiguous before users build on top
of the codec.

---

## Headline numbers

VaptVupt's design tradeoffs put it in a different region of the
speed/ratio frontier than zstd. **On full Silesia, vv is slower than zstd
in every encode mode and most decode modes, and ratio-equivalent or
slightly behind. vv-fast wins decode on a few fixtures (sao, osdb, x-ray)
where the data is binary/scientific and the ANS hot loop's working set
fits L1.**

Aggregate geometric means across 12 Silesia fixtures:

| Tool         | Ratio | Encode MB/s | Decode MB/s |
|--------------|------:|------------:|------------:|
| vv -fast     | 2.36  |   89.8      |   548.4     |
| vv -balanced | 3.11  |   19.9      |   370.4     |
| vv -extreme  | 3.15  |    9.7      |   377.3     |
| zstd -1      | 2.90  |  209.6      |   763.0     |
| zstd -3      | 3.19  |  156.1      |   641.2     |
| zstd -9      | 3.61  |   41.4      |   625.1     |

## Where vv stands vs zstd, target by target

The SPEED PROGRAM defined four targets in Sprint 25:

### T1: vv-fast decode ≥ zstd-1 decode

**Result: won on 1/12 fixtures (sao).**

```
fixture     vv-fast       zstd-1       vv/zstd
─────────────────────────────────────────────────
sao         618 MB/s      585 MB/s     1.06×  ← vv wins
osdb        761 MB/s      767 MB/s     0.99×  ← tied within noise
xml         671 MB/s      833 MB/s     0.81×
others      0.6–0.8× zstd-1
```

vv-fast decode is **competitive** (geo-mean 548 vs 763 MB/s = 0.72×) but
loses on most fixtures. The wins are on binary/scientific content where
zstd's Huffman+FSE dispatch is more expensive than vv's ANS.

### T2: vv-fast encode ≥ zstd-1 encode

**Result: won on 0/12 fixtures.**

vv-fast: 90 MB/s geo-mean. zstd-1: 210 MB/s geo-mean. **Persistent
2.3× gap**, no fixture closes it. zstd's match-finder + entropy stage
is architecturally faster on encode.

### T3: vv-balanced encode ≥ zstd-3 encode

**Result: won on 0/12 fixtures.**

vv-balanced: 20 MB/s. zstd-3: 156 MB/s. **Persistent 8× gap.** Same root
cause as T2 — vv's lazy parse + ANS encode is heavier per byte than
zstd's two-stage approach.

### T4: vv-fast decode > zstd-3 decode

**Result: won on 3/12 fixtures (osdb, sao, x-ray).**

```
osdb     vv-fast 761 MB/s  > zstd-3 525 MB/s  ← vv wins by 45%
sao      vv-fast 618 MB/s  > zstd-3 502 MB/s  ← vv wins by 23%
x-ray    vv-fast 562 MB/s  > zstd-3 485 MB/s  ← vv wins by 16%
```

These are the **binary/scientific fixtures** where vv's design pays off.
On text fixtures (dickens, reymont, webster) vv-fast still loses to
zstd-3 decode.

## Ratio comparison

Geo-mean ratios:

```
vv-fast:      2.36
vv-balanced:  3.11
vv-extreme:   3.15

zstd-1:       2.90
zstd-3:       3.19   ← marginally better than vv-extreme (-1.3%)
zstd-9:       3.61   ← 12.7% better than vv-extreme
```

**vv-extreme is 1.3% behind zstd-3 and 12.7% behind zstd-9 on aggregate.**

This is **revised from a prior internal claim of "+1.07% vs zstd-3 on
aggregate."** That earlier claim was measured on a 4-fixture subset
(dickens, xml, sao, x-ray). Expanding to full Silesia inverts the
result. The honest summary is:

- vv-extreme matches zstd-3 within 1-2% on aggregate
- vv-extreme is well behind zstd-9 (gap 6-21% per fixture)
- vv-extreme **wins ratio on no Silesia fixture** vs zstd-9

This is in line with vv's design: vv's Huffman+ANS hybrid is
ratio-competitive with zstd-3, not zstd-9. Catching zstd-9 would
require optimal-parse encoding, dictionary support, and FSE-style
entropy refinement.

## Where vv is competitive

Five honest claims that survive the full Silesia measurement:

1. **Decode speed on binary/scientific content.** vv-fast on osdb, sao,
   x-ray beats both zstd-1 and zstd-3 decode. Use vv-fast if you decode
   scientific data more than you encode it.

2. **Ratio at the "balanced" level is essentially zstd-3 equivalent.**
   vv-balanced (3.11 geo-ratio) is 2.5% behind zstd-3 (3.19); vv-extreme
   (3.15) closes that to 1.3% behind. Either mode is a reasonable
   replacement for zstd-3 if you don't need zstd's encode speed.

3. **Decode speed regression isn't huge at any level.** vv-balanced
   decode is 0.58× zstd-3 (370 vs 641 MB/s). Workable for non-hot-path
   decode like archive extraction.

4. **The codec is post-quantum-ready.** vv ships as the compression
   layer beneath VaptVupt-the-application (formerly Zupt), which uses
   ML-KEM-768 + X25519 for sealed-box encryption. zstd has no equivalent
   integrated story.

5. **AGPL + commercial license model.** zstd is BSD. For users whose
   use case fits AGPL or who want a commercial license with author
   support (sac@securityops.co), vv is the answer.

## Where vv is NOT competitive (honest)

1. **Encode speed at any level vs any zstd level.** vv-balanced
   (20 MB/s) is slower than even zstd-9 (41 MB/s). This is a real
   architectural gap, not a sprint-of-tuning gap.

2. **Decode speed on text content.** dickens, reymont, webster, mr all
   show vv-fast at 0.55-0.65× zstd-1 decode. The ANS hot loop's 48 KB
   working set (three 16 KB tables) thrashes L1 on text.

3. **Top-end ratio.** zstd-9 has a 6-21% per-fixture ratio lead over
   vv-extreme. Closing it requires architectural changes (optimal parse,
   FSE, dictionary support).

## Build modes and their effects

The default `make` build uses `-O3 -flto`. Two opt-in build modes give
extra speed:

- `make perf` adds `-march=native`. Decode +4-6%, encode +2-4% on the
  build host. **Non-portable** — binary requires the build host's CPU
  features.
- `make pgo` does a 2-stage profile-guided build. Decode +2-6%,
  encode +3-8% over default. Portable. Requires `/tmp/silesia/` (or
  override with `PGO_TRAIN_DIR=...`) for the training step.

Both can be combined by users who want maximum speed on a known
target.

## What changed since v2.48.5 (SPEED PROGRAM result)

The SPEED PROGRAM ran from Sprint 25 to Sprint 32 (six executed
optimization sprints + this final-measurement sprint). Cumulative gains
on default build:

| Metric                | v2.48.5 baseline | v2.50.5 (current) | Delta |
|-----------------------|------------------|-------------------|------:|
| Decode dickens fast   | ~283 MB/s        | 418 MB/s          | +48%  |
| Encode dickens fast   | ~64 MB/s         | 70 MB/s           | +9%   |
| Decode sao fast       | ~218 MB/s        | 618 MB/s          | +183% |
| Encode sao fast       | ~47 MB/s         | 51 MB/s           | +9%   |

The decode gains (especially on binary fixtures like sao) are
substantial. The encode gains are modest — encode is dominated by
match-finding, which is harder to optimize without architectural
change.

Per-sprint contributions:

| Sprint | Change                                | Decode gain | Encode gain |
|--------|---------------------------------------|------------:|------------:|
| 26     | `-O3 -flto` default build flags       | **+14%**    | +5%         |
| 27     | ANS hot-loop OOB-fold                 | **+4%**     | 0%          |
| 28     | `make pgo` build target               | +4% (PGO)   | +6% (PGO)   |
| 29     | `matcher_insert_fast` in compress     | 0%          | **+4%**     |
| 30     | Unconditional prefetch in chain walk  | 0%          | +2.7%       |
| 31     | runtime AVX2 dispatch                 | NEGATIVE — reverted | — |
| 32     | Final measurement (this doc)          | —           | —           |

## When to pick vv over zstd

Pick vv if:

- You decode scientific/binary data more than you encode (T4 wins on
  osdb/sao/x-ray)
- You need post-quantum encryption integration via libpqvaptvupt
- AGPL + commercial license fits your model
- You don't need top-tier encode speed

Pick zstd if:

- Encode speed matters (always, at any level)
- You're decoding mostly text and want maximum decode throughput
- You need the highest ratios (zstd-9, zstd-19)
- BSD license is required

## What we will NOT claim

Six prior internal claims that the SPEED PROGRAM and this final
measurement do NOT support:

1. ~~"vv beats zstd on aggregate ratio"~~ — false on full Silesia
   (-1.3% vs zstd-3, -12.7% vs zstd-9)
2. ~~"vv is faster than zstd"~~ — false on encode at every level;
   true only on decode for 3 specific binary fixtures
3. ~~"vv-fast matches zstd-1"~~ — wrong on 11/12 fixtures for decode,
   wrong on 12/12 for encode
4. ~~"vv has industry-leading compression"~~ — vv-extreme < zstd-9
   on 12/12 fixtures
5. ~~"vv is blazing fast"~~ — encode is consistently 2-8× behind zstd
6. ~~Specific MB/s numbers without "(measured on Silesia, Linux x86_64,
   gcc 13)"~~ — all benchmarks are environment-dependent

## Reproducing these numbers

```bash
# Build the codec
cd vaptvupt-2.50.5/
make

# Download Silesia corpus
curl -O https://web.archive.org/web/silesia.zip  # or your mirror
unzip silesia.zip -d /tmp/silesia

# Run the benchmark
python3 docs/speed_program_bench.py ./vaptvupt
```

The benchmark script is included in `docs/speed_program_bench.py`.
Run it on your hardware to see how the numbers move with your CPU,
your memory, your compiler. **Don't trust these numbers blindly —
measure on the hardware that matters to you.**

---

## SPEED PROGRAM closure

The program ran Sprints 25-32 (8 sprints as planned). One target
(T1) achieved partially, three targets (T2/T3/T4) unmet at the
goal but with measurable progress.

Honest verdict: **vv stayed in vv's region of the speed/ratio
frontier**. The program achieved +20-180% decode speedups depending
on fixture, and +5-10% encode speedups. It did not close the
architectural gap to zstd.

Further closure of the encode gap (T2, T3) requires architectural
changes (match-finder rewrite, optimal parse, FSE entropy refinement,
dictionary support) that fall outside this program's scope. Future
sprints, if any, will need to commit to a wire-format change for
substantial further gains.

The program declares end here. Subsequent codec work should focus on
maintenance, fuzz coverage, and integration with the libvaptvupt and
libpqvaptvupt projects.
