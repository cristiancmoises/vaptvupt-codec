# Sprint 58 — Lever S3 (fast-mode encode): lean finder ships +7-12% at byte-identical output; depth-1/2/3 rejected

**Codec: v2.52.2.** Encoder-only, wire-neutral change. Fast-mode encode is
7-12% faster; output is byte-identical to v2.52.1 (decode and ratio
unchanged). The Sprint 57 recommendation — a literal lz4-style depth-1
single probe — was measured and **rejected**: it raises encode further but
degrades both ratio and decode, the two metrics the SPEED PROGRAM ranks
above encode. The shipped change is the only operating point that improves
encode at zero cost to the others.

This closes Lever S3 and, with it, the codec-speed avenue: the decode
ceiling was mapped in Sprints 55-57 and encode is now squeezed here
without a ratio/decode tradeoff. Remaining "continue" should shift to the
sibling projects (libvaptvupt, libpqvaptvupt) per Sprint 57's option (2).

## Baseline (this machine, re-measured)

1-core Intel Xeon @ 2.80 GHz, gcc 13.3, `-O3 -flto`. lz4 1.9.4, zstd 1.5.5.
The shipped v2.52.1 binary (md5 `aa021ef3…`) reproduced from a clean build
and used as the A/B baseline.

Fast mode, in-process best-of-15 (isolates the match finder from I/O):

```
fixture   ratio    enc MB/s   dec MB/s
dickens   1.992      73.3       809
xml       5.254     201.6      1572
samba     3.117     130.2      1198
```

vs competitors at the CLI (unchanged by this sprint — for context):
zstd-1 dickens ratio 2.391 / enc 161 / dec 572; lz4-1 dickens ratio 1.585
/ enc 257 / dec 667. vv fast is ratio- and decode-dominated by zstd-1; the
gap this sprint addresses is encode.

## The change

`compress_block` searched for fast-mode matches via `chain_match_ex`, which
carries machinery fast mode never exercises:

- a 4-way software-pipelined **priming prefetch** block (3 dependent chain
  loads before the walk even starts),
- per-iteration unconditional prefetches,
- `hash4`/`hash3` fallback branches that are always disabled in fast mode
  (`use_hash4`/`use_hash3` == 0).

`single_probe_match` is a stripped chain walk used only by the fast-mode
matcher (gated on a new `matcher_t.single_probe` flag). It walks the **same
hash5 chain to the same depth (4)** and selects by the **same rule** (first
strictly-longest, early-out at len ≥ 256) as `chain_match`. Because the
selection logic is identical and the prefetches are hints with no effect on
the result, it emits the **same tokens**: fast-mode output is byte-identical
to v2.52.1. The speedup is purely the omitted prefetch/branch overhead on
the hot per-position path.

The flag is set only on the real `ULTRA_FAST` matcher (one-shot +
streaming). Balanced/extreme — and the balanced/extreme window-selection
trial that internally calls `compress_block` in ULTRA_FAST mode on its own
matchers — keep `single_probe == 0` and the original path, so their output
and window decisions are bit-identical.

## Result (shipped: depth-4 lean finder)

Fast-mode encode, baseline → new:

```
                    in-process best-of-15      CLI best-of-7
  dickens   73.3 → 80.2 MB/s   (+9.4%)     66.2 → 74.0 MB/s   (+11.8%)
  xml      201.6 → 217.4 MB/s   (+7.8%)    160.4 → 171.0 MB/s   (+6.6%)
  samba    130.2 → 143.2 MB/s   (+10.0%)   112.3 → 126.6 MB/s   (+12.7%)
```

Decode, ratio, compressed size: byte-identical on all 12 Silesia fixtures,
all three modes (md5-verified). The CLI delta is smaller than in-process
because the CLI number includes fixed per-file I/O and process startup that
this change does not touch.

## The depth sweep — why depth-1/2/3 were rejected

The lever is wire-neutral, so a shallower probe is free to try. Measured
directly (lean finder, fast mode, in-process best-of-5, dickens/xml/samba),
sweeping `chain_depth`:

```
dickens, fast:
  depth  ratio    enc MB/s   dec MB/s   cmp_KB
   1     1.785      91.9       637      5577    ratio -10.4%, decode -22%
   2     1.897      87.2       704      5248
   3     1.955      83.6       750      5093
   4     1.992      80.2       819      4998    = v2.52.1 (byte-identical)

xml, fast:
   1     4.273     238.3      1195      1222    ratio -18.7%
   4     5.254     217.4      1531       994    = v2.52.1

samba, fast:
   1     2.887     158.9       968      7307    ratio  -7.4%
   4     3.117     143.2      1131      6770    = v2.52.1
```

The pattern is monotone: a shallower search finds shorter matches, which
means **more tokens per output byte**, which both **inflates the output
(worse ratio)** and **slows the decoder (more symbol/token work per byte)**.
A standalone branchless depth-1 probe (no loop bookkeeping) reached ~113
MB/s encode on dickens — the largest encode gain available — but at the same
−10% ratio / −13-22% decode cost.

The SPEED PROGRAM's stated priority order is **decode > ratio > encode**
("decode is the most-used metric; encode is one-time"). Every depth below 4
trades the two higher-priority metrics to buy the lowest-priority one. That
is the wrong trade for a general-purpose fast mode, so it is not shipped.
depth-4 — same matches as before, just a leaner search — is the only point
that is a pure win.

### Correction to the Sprint 57 premise

Sprint 57 framed single-probe as "wire-neutral, encoder-only ... plausibly a
real win." The wire format is indeed neutral, but the framing missed that
the encoder's **match-length distribution drives decode speed**: a
single-probe encoder's shorter matches regress decode, an axis Sprint 57 did
not account for. The encode-speed win is real but is not free of a decode
cost unless the match selection is preserved — which is exactly what the
shipped depth-4 lean finder does.

## Validation

- `-Wall -Wextra -Werror` clean; ASan + UBSan clean over 15K + 5K fast-mode
  cases (no OOB, no UB, no codec leak — only the throwaway harness's own
  unfreed `main()` buffers, since fixed).
- 19/19 C suites green; wire-format spec self-test green; Python reference
  decoder + encoder self-test green; negative corpus 27/27 consistent.
- `test_dos_hang` 12/12 in 0.016 s; `test_safezone_adversarial` 55/55.
- Differential fuzzer (C ↔ Python) 5200/5200 consistent.
- Changed-path fuzz: 50,000 in-process fast roundtrips (0 failures);
  1,500 ref-vs-new fast-output identity cases over a broad distribution
  incl. edge sizes 0..7 and the 1 MB block boundary (0 mismatches, 0
  roundtrip failures).
- Mode isolation: balanced byte-identical 12/12; extreme byte-identical 8/8.

## Pre-existing issue (NOT from this sprint)

`tests/bench_gate.py` fails on a stale `tests/bench_baseline.json`: the
pristine v2.52.1 binary produces the same synthetic-fixture sizes this build
does and fails the same 12 checks, including a real contract violation
`binary-pattern/extreme (1507) > balanced (852)`. Output-neutral to Sprint
58. Deliberately not `--update`d here (that would launder the extreme
regression into the baseline). Deferred to a dedicated sprint to root-cause
the `binary-pattern` extreme regression and regenerate the baseline against
the current codec. The gate is informational in `make test`.

## Recommendation for the next "continue"

The codec-speed avenue is exhausted with two shipped wins (S53 fast decode
+21-43%, S58 fast encode +7-12%) and a mapped, measured ceiling on both
ends. Either:
1. Open a dedicated **bench-baseline / extreme-`binary-pattern`** sprint to
   clear the pre-existing gate failure (small, self-contained, real bug).
2. Shift to the sibling projects (libvaptvupt bindings, libpqvaptvupt) — the
   codec's speed and ratio are both at well-characterized, defensible
   positions.
