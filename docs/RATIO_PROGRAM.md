# RATIO PROGRAM — beating zstd on compression ratio

**Started:** Sprint 42
**Status (Sprint 50): CLOSED.** Final ratio release is v2.52.0 (md5 93a73ccd…). vv-extreme geomean 3.4604: beats zstd-1 by +20.15%, beats zstd-3 by +8.51%, within −4.15% of zstd-9, −15.29% of zstd-19. All 12 Silesia fixtures beat the program-start baseline. Sprints 47-50 measured the architectural ceiling (price-model and match-finder-parameter levers exhausted, 4 consecutive measured negatives). The only remaining ratio lever is a structural binary-tree match-finder rewrite (multi-sprint, not started by decision). See RATIO_PROGRAM_CLOSEOUT.md for the full decision and reopen conditions.
**Codec shipped:** v2.52.0 (Sprint 46) — unchanged since.

---

## Why this program exists

The user's goal: "be better than zstd." The honest measurement (PERFORMANCE.md, re-confirmed Sprint 42) shows vv currently loses to zstd on all three axes:

| Axis | vv vs zstd | Realistic to win? |
|---|---|---|
| Encode speed | 0.3–0.5× zstd-1 | **No.** zstd has a decade of hand-tuned x86 asm. |
| Decode speed | 0.6–0.9× zstd-1 | **Unlikely.** Same reason; SIMD lever exhausted (Sprints 31, 41). |
| **Compression ratio** | **−22.9% vs zstd-19 geomean** | **Yes — this is an algorithm-quality question.** |

Ratio is the only axis where "better than zstd" is achievable, because ratio is determined by parse quality + entropy coding, not by micro-optimized machine code. **The ratio program targets this axis.**

## The gap, measured (Sprint 42, vv-extreme vs zstd-19)

```
fixture       vv/zstd-19 ratio    gap%
nci              0.594           +68.5   ← worst: repetitive, exceeds 16MB window
reymont          0.687           +45.6
xml              0.707           +41.4
webster          0.720           +38.8
samba            0.741           +34.9
dickens          0.746           +34.0
mozilla          0.781           +28.1
mr               0.826           +21.0
ooffice          0.843           +18.7
osdb             0.868           +15.2
x-ray            0.877           +14.1
sao              0.924            +8.2   ← best: dense binary, little to gain
geomean: vv-extreme 3.148  vs  zstd-19 4.085   →  vv -22.9%
```

Diagnosis: the gap is worst on **repetitive/structured** data and smallest on **dense binary**. Three causes, in priority order:

1. **Parse quality** — vv uses greedy + lazy-1; zstd-19 uses an optimal (btopt) parser. This is the biggest lever and the Sprint 42 focus.
2. **Window size** — vv-extreme caps at 16 MB (2^24); nci (33 MB) exceeds it, so long-range matches are unreachable. zstd-19 default window plus `--long` reaches further.
3. **Entropy coding** — vv uses 4-stream Huffman for literals + ANS for sequences; zstd uses FSE throughout with more adaptive table selection.

## Sprint 42: optimal parser foundation

Built a forward dynamic-programming optimal parser (`compress_block_optimal`), gated to extreme mode only, wire-format-neutral (emits the same token stream `emit_seq` consumes — verified: a correct run roundtrips byte-perfect and decodes with the unchanged decoder).

### Price model

Formalizes the Sprint 119 cost-aware-lazy heuristic into explicit bit prices:
- literal: ~6 bits/byte
- match: `cost_const(14) + log2(offset) + ml_extra(len)`, rep matches ~2 bits

This is the same accounting the lazy decision used, applied globally via DP instead of one-position-ahead.

### Measured result (prototype, NOT shipped)

On text-like data the optimal parser **wins**, confirming the approach:

| Fixture | greedy/lazy | optimal proto | delta |
|---|---:|---:|---:|
| dickens | 3,818,656 | 3,690,415 | **−3.4%** |
| reymont | 1,962,799 | 1,890,031 | **−3.7%** |
| osdb | 3,570,160 | 3,442,338 | **−3.6%** |
| mr | 3,767,068 | 3,683,507 | **−2.2%** |

But on repetitive/long-match data it **regresses badly**:

| Fixture | greedy/lazy | optimal proto | delta |
|---|---:|---:|---:|
| mozilla | 19,353,491 | 23,618,749 | **+22.0%** |
| ooffice | 3,083,782 | 3,616,622 | **+17.3%** |
| samba | 5,270,964 | 6,172,261 | **+17.1%** |
| nci | 2,845,605 | 3,194,190 | **+12.3%** |

Aggregate: **+9.2% (worse).** Not shippable.

### Two blockers identified (both fixable, next sprint)

1. **Correctness: position-accounting bug on window-spanning long matches.**
   The windowed DP (4096-position window) handles a match that runs past
   the window boundary by storing the full length in the boundary node
   and clamping the backtrack step. The clamp produces a token sequence
   whose lengths don't sum to the consumed span, corrupting the stream
   (decode returns -2). Text fixtures happened to roundtrip in the first
   prototype only because their matches rarely hit the window boundary.

2. **Ratio regression on long-match data.**
   Even setting aside correctness, the windowing fragments long matches.
   Repetitive data (mozilla, nci) wants a few very long tokens; the
   4096-window forces them to be split, and the per-token overhead
   explodes the size. zstd avoids this because its optimal parser
   operates over the whole block with proper long-match handling.

### Root cause of both: naive windowing

The 4096-position DP window was chosen to bound O(n × candidates)
complexity, but it's the wrong abstraction for long matches. The correct
design (zstd btopt, lzma) handles arbitrarily long matches inside the DP
by:
- pricing a match of length L as a single edge from i to i+L (no window
  truncation)
- when L exceeds remaining DP buffer, flushing the optimal path computed
  so far and re-anchoring at i+L cleanly (not mid-match)
- keeping the DP buffer large enough (zstd uses ZSTD_OPT_NUM = 2048 but
  with explicit "match longer than buffer → take it and restart" logic
  that is position-correct)

## Sprint 43 result — SHIPPED in v2.51.0

The windowing was abandoned. The whole-block single-edge DP design fixed
both blockers (correctness + long-match regression) in one move. Result:

| Aggregate (full Silesia, extreme mode) | old | new | delta |
|---|---:|---:|---:|
| Geomean ratio | 3.148 | 3.235 | **+2.75%** |
| Total compressed bytes | 67,679,941 | 66,402,208 | **−1.89%** |
| Per-fixture | — | — | **7 wins, 5 losses** |
| vs zstd-3 geomean (3.189) | −1.3% | **+1.45%** | — |
| vs zstd-19 geomean (4.085) | −22.9% | **~−20.8%** | gap narrowed |

Per-fixture: xml −7.85%, reymont −7.44%, dickens −6.92%, webster −6.77%,
samba −5.60%, osdb −3.87%, mr −2.94%, mozilla +0.63%, nci +2.04%, sao
+1.78%, ooffice +2.91%, x-ray +3.02%.

**Validation gates all passed:**
- All 19 test suites green (24/24 roundtrip, 42/42 edge_cases, 55/55
  safezone adversarial, 12/12 DoS hang, plus 15 more)
- 25,203 libFuzzer roundtrip iterations: 0 findings (0 crashes / 0 leaks
  / 0 OOM / 0 timeouts)
- 25,200 differential fuzzer cases across 6 strategies: 0 mismatches
- 12 Silesia byte-perfect roundtrip
- Wire format unchanged, C ABI unchanged, `-Werror` clean

**Encode-speed cost documented:** ~2.4× to ~14× slower in extreme mode
(~1 MB/s avg). Acceptable for the "max ratio, will wait" tier; zstd-19
has a comparable profile. Balanced/fast modes unchanged.

**Worst-case bounded** by the long-match short-circuit: any match of
length ≥ 512 is taken immediately as a single edge, skipping interior
DP — both bounds DoS resistance AND is the correct optimal choice.

### Implementation key points

- `compress_block_optimal()` in `src/vv_encoder.c`, ~210 lines
- Whole-block forward DP over `price[0..N]` (1 MB block → 4 MB int32, per-block)
- Single-edge match relaxation (no windowing): `price[i+L] = min(price[i+L], price[i] + match_price(off,L))`
- `opt_collect()` gathers candidates (longest per distinct offset, max 16)
- `opt_match_price()` formalizes Sprint 119 cost model
- `opt_lit_price()` flat 6 bits/byte (Sprint 44 target)
- Gated to `VV_MODE_EXTREME` in `emit_block()`; balanced/fast unchanged
- csz==0 from optimal path flows into the raw-store branch (alloc/overflow safe)

## Sprint 44 plan (next): two-pass entropy-aware repricing

The 5 fixtures that still lose (mozilla, nci, sao, ooffice, x-ray) share
a signature: dense binary or highly-repetitive data where flat 6 bits/byte
literal price is wrong. Real entropy of these fixtures' literals is closer
to 7-8 bits/byte. The parser under-prices literals and over-uses them in
places where matches would be cheaper.

Standard btopt refinement (zstd, lzma both do this):

1. First pass: optimal parse with flat literal cost (current behavior)
2. Build the actual Huffman table from the literals the first pass chose
3. Re-price literals using the real per-byte Huffman costs
4. Re-parse with the corrected prices

Expected outcome: ratio improvement on all 12 fixtures, not just 7. The
mispriced regressions should reclaim, and the existing wins should widen.

If two-pass repricing closes the dense-binary loss but more is needed,
Sprint 45 targets window-size lever (vv-extreme caps at 2^24 = 16 MB;
nci 33 MB exceeds it). Increasing to 2^27 reaches zstd `--long`-class
window for the few fixtures where long-range matches dominate.
