# RATIO PROGRAM — Closeout (Sprints 42-50)

**Decision: the RATIO PROGRAM is closed. v2.52.0 (md5 93a73ccd…) is the
final ratio release.** The price-model and match-finder-parameter levers
are exhausted (measured, not assumed). The only remaining ratio lever is
a structural match-finder rewrite (binary tree), which is a multi-sprint,
encode-speed-affecting effort whose ROI does not justify starting without
an explicit decision to commit to it. This document records the program,
the final position, and the conditions under which the program would
reopen.

## Final competitive position (full Silesia, extreme mode, geomean ratio)

```
vv-extreme v2.52.0:   3.4604
  vs zstd-1  (2.88):  +20.15%   vv wins
  vs zstd-3  (3.189): +8.51%    vv wins
  vs zstd-9  (3.61):  -4.15%    vv behind
  vs zstd-19 (4.085): -15.29%   vv behind
```

All 12 Silesia fixtures beat the v2.50.11 program-start baseline. Encode
is ~1 MB/s in extreme (45-152 s on the large fixtures); decode is
unaffected (294-460 MB/s). Balanced/fast modes unchanged throughout the
program.

This is a strong, honest, defensible position: **vv-extreme beats zstd-3
decisively and sits within ~4% of zstd-9** at a comparable encode-speed
tier, having started the program behind zstd-3.

## Program trajectory

| Sprint | Version | Lever | Result |
|---|---|---|---|
| 42 | — | windowed-DP optimal parser | NEGATIVE (broke roundtrip), reverted |
| 43 | 2.51.0 | whole-block optimal parser | +2.75% geomean, beats zstd-3 first time |
| 44 | 2.51.1 | literal price 6→8 | +0.87% more, 10/12 fixtures win |
| 45 | — | large-window scaling | NEGATIVE (broke roundtrip), reverted |
| 46 | 2.52.0 | large-window + decoder cap fix | **+9.91% total, all 12 win, +8.51% vs zstd-3** |
| 47 | — | rep-offset analysis | diagnosis: rep usage 0.1-3.8% vs zstd ~40% |
| 48 | — | rep-aware DP | NEGATIVE — rep usage→32.6% but ratio regressed |
| 49 | — | calibrated ANS offset cost | NEGATIVE — text regressed, parse near-optimal |
| 50 | — | match-finder param sweep | NEGATIVE — depth/long-match/cand all <0.3%, noise |

The program's single biggest win (Sprint 46, +9.91%) came from turning a
Sprint 45 negative result's precise diagnosis into a one-line decoder fix.
The two real shipped levers were the optimal parser (43) and the large
window (46); literal-price calibration (44) was a profitable one-character
tune. Everything after 46 measured the architecture's ceiling.

## Why the remaining levers are exhausted (measured)

### Price model (Sprints 47-49)

The extreme-mode optimal parser is at or near a LOCAL OPTIMUM for its
candidate set and entropy backend:

- **Rep-offsets (48):** vv supports 3 rep offsets in the wire + decoder,
  and the parser was made rep-aware (rep usage 3.8%→32.6% on xml). Ratio
  REGRESSED. vv's adaptive ANS already entropy-codes recurring offsets
  efficiently, so the rep discount is largely illusory.
- **Offset cost calibration (49):** the heuristic over-prices large
  offsets by 10-28 bits vs their true ANS cost. Correcting it REGRESSED
  text (even using a fixture's own measured costs) because "correct"
  post-hoc costs over-select large offsets and displace the
  literal/short-match choices the entropy coder handles best. The
  heuristic's pessimism was accidentally protective.

Re-pricing the SAME candidate set cannot help a parse that is already
near-optimal for those candidates.

### Match-finder parameters (Sprint 50)

The hash-chain finder's tunable knobs are spent:

```
LONG_MATCH threshold 512→8192:   xml -0.22%, reymont 0.00%  (rarely fires on text)
chain depth 256→1024:            reymont -0.28% at +44% encode time
combined depth384/lm2048:        -0.031% subtotal (noise; regresses sao/mr)
```

Deeper chains and higher long-match thresholds barely move ratio because
the chain finder is not MISSING candidates that matter — the candidates it
finds are already well-used by the optimal parse. Parameter tuning is the
match-finder analogue of Sprint 41's exhausted SIMD-on-existing-algorithm
lever for speed.

## The one remaining lever (not started, by decision)

**Structural match-finder rewrite (binary tree / btopt).** zstd's high
levels find more and longer matches via a binary-tree match finder than
vv's depth-256 hash chain. Better candidates are the only input that could
let the parser improve — and Sprint 49 showed the parse itself is not the
bottleneck. This is:

- multi-sprint (a correct, fuzz-clean btree finder is substantial),
- encode-speed-affecting (btree changes the encode cost profile),
- wire-neutral (it only changes which matches are chosen),
- the plausible path to closing the zstd-9 gap and starting on zstd-19.

It is NOT started because the disciplined position is that the current
+8.51%-vs-zstd-3 / −4.15%-vs-zstd-9 Pareto point is a legitimate shipping
target, and committing multiple sprints to a btree rewrite should be a
deliberate decision, not a default. The measurements above are preserved
so that work, if undertaken, starts with accurate pricing data and a clear
target.

## Reopen conditions

The RATIO PROGRAM should reopen only if:

1. A decision is made to invest the multi-sprint effort in the binary-tree
   match finder (Lever C), with beating zstd-9 as the explicit target; or
2. A new fixture class or use case shows the current Pareto point is
   insufficient for a real need; or
3. The entropy backend is replaced (a different class of change), which
   would re-open the price-model questions of Sprints 47-49 under new
   cost structure.

Absent one of these, v2.52.0 stands as the ratio release and further
"continue" effort is better spent on the other program tracks (libvaptvupt
bindings, libpqvaptvupt, packaging/distribution, security review) than on
re-probing the exhausted ratio levers.

## Honesty note

This closeout reports four consecutive negative results (47-50) without a
shipped codec change since Sprint 46. That is the correct outcome: the
ratio program found its architectural ceiling and stopped at it rather
than shipping regressions or inflating marginal noise into "wins." The
"beat zstd-19 in all aspects" framing from the original request was
explicitly unachievable (separate Pareto corners; see PROGRAM_PROMPT.md
§0); what WAS achieved — decisively beating zstd-3 and closing to within
4% of zstd-9 from a starting deficit — is the honest, measured result.
