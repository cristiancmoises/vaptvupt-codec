# Sprint 19 — Profile and negative result

## Outcome

**Negative result.** The dickens +4.07% gap to zstd-3 was profiled and a candidate fix (tuning the cost-aware lazy parser's `literal_bits` constant) was tested across a sweep. The sweep showed the parser is **already at a local optimum** with the current architecture. No code change shipped. Documented next-sprint directions below.

## Profile

For dickens (10.2 MB → 3.82 MB compressed; zstd-3 produces 3.67 MB):

| Section | Bytes | % of vv output | Optimizability |
|---------|------:|---------------:|---------------|
| Literal payload (lit_fmt=4) | 294,432 | 7.7% | Near-optimal (4-stream Huffman) |
| ML/OF/LL sequence bitstream (ANS) | 3,521,783 | 92.2% | This is where the 149 KB gap lives |
| Headers, states, overhead | 2,441 | 0.1% | Already negligible |

**Per-sequence cost analysis:**

- 1,283,049 sequences across 10 blocks
- 3,521,783 bytes ÷ 1.28M sequences = ~2.74 bytes/sequence = ~22 bits per (ML_code, OF_code, LL_code, extras)
- Most of this is the offset (OF) section: log2(typical offset) ≈ 9–12 bits per non-rep match
- Rep-matches and short matches are already cheap; long-offset matches dominate the cost

## Hypothesis tested

**Hypothesis:** The cost-aware lazy parser uses `literal_bits = 6` as a constant in its shift/no-shift decision. Dickens's actual avg literal cost (after 4-stream Huffman) is 5.11 bits/byte — a 17% overestimate. The bias should make the parser too conservative about shifting and thus emit more, shorter matches.

**Sweep (Δ vs v2.48.2 baseline):**

| `literal_bits` | dickens | xml | sao | x-ray |
|:-:|--:|--:|--:|--:|
| 4 | +0.193% | +0.081% | +0.025% | −0.017% |
| 5 | **−0.029%** | +0.036% | +0.029% | −0.013% |
| **6 (baseline)** | 0.000% | 0.000% | 0.000% | 0.000% |
| 7 | +0.980% | +0.017% | −0.007% | +0.023% |

**Interpretation:**

- The optimum is between 5 and 6. literal_bits=5 improves dickens by 1.1 KB (0.029%) but the other fixtures move within the noise floor in both directions. The improvement is real but tiny — not gap-closing.
- The cost-aware lazy parser is essentially at its local optimum. Sprint 119/120/121 already tuned this aggressively (per the comments in `vv_encoder.c`).
- The remaining 4.07% gap is structural, not a tuning artifact.

## Why I did not ship literal_bits=5

Although the change technically meets the acceptance criteria (no fixture regresses by >0.1%; aggregate improvement), the gain is 1.1 KB on a 3.82 MB file. The change comes with no risk of correctness issues but also no meaningful user-visible benefit. Shipping it would consume a release version (v2.49.0) without justifying the version bump. The honest call is **don't ship a 1.1 KB change as v2.49.0**.

If a future sprint accumulates ratio improvements that collectively justify a release, this is one to fold in. As a standalone change, it's noise.

## Next-sprint directions (Sprint 20+)

The profile points clearly at where the gap lives: the ML/OF/LL ANS bitstream (92.2% of the output). Three architectural directions, each multi-sprint:

### Direction A: Predefined ANS tables (zstd FSE-equivalent)

zstd-3 uses **predefined default tables** for ML/OF/LL when the block's frequency distribution is close enough to the predefined one. vv always builds tables from scratch per block.

- For 1 MB blocks (like dickens), the per-block table-building cost is amortized over 127K sequences, so the table HEADER overhead isn't the issue (it's already only 222 bytes/block).
- The issue is that vv's tables are **fit to the block** while zstd's predefined tables are **fit to a large training corpus**. For some symbol distributions, the predefined tables encode common values with fewer bits even if they're slightly less accurate on the specific block.
- Implementation: maintain a "default table" for each of ML/OF/LL, race it against the per-block table, choose the smaller output. Wire-format change required (add a "use_default_table" flag bit).
- Estimated 2–3 sprints.

### Direction B: Hash3 matcher (min_match=3 actually exploited)

The wire format already supports `min_match=3` via the SEQ_V2 ('T') tag. The encoder doesn't yet use it — Sprint 121 notes "hash3 matcher will be added in a future sprint to realize the improvement." This would add many short matches that the current parser misses.

- 3-byte matches are common in English text (e.g., "the ", " of ", "and "). Adding them might increase sequence count but reduce literal count significantly.
- Risk: more, shorter matches inflate the ML/OF/LL streams. Net win is not guaranteed.
- Implementation: add a 3-byte hash table to the matcher, race hash3 + hash4 matches, pick best. Toggle behind extreme mode.
- Estimated 1–2 sprints.

### Direction C: Rep-match window extension

vv tracks 3 recent offsets (rep[0], rep[1], rep[2]) and encodes matches against those at near-zero cost (2 bits). zstd-3 uses the same 3-rep system. **But** vv's parser only checks rep matches at one position; zstd checks rep matches at every position including pos+1 (during lazy probe).

- For dickens, ~30-50% of matches typically have rep-able offsets if the parser would look for them aggressively.
- Implementation: add rep-match scan to the lazy probe (at pos+1, also check if any of rep[0..2] would have produced a long match).
- Estimated 1 sprint.

## Recommendation

**Direction C first** (smallest sprint, biggest expected payoff if rep coverage is currently under-exploited). Then B (hash3 matcher) if C closes part of the gap. A (predefined tables) is the biggest architectural change and should be the last resort.

If Sprint 20 takes Direction C and closes ≥1pp on dickens, Sprint 21 takes Direction B for the rest. If C is also a negative result, Direction A becomes the priority.

## Lesson

The four documented losses (fx_source, bash, dickens, xml) are NOT individually closable by single-line tuning changes. The current encoder is well-optimized within its architectural envelope. Closing the remaining gaps requires either:

1. New matching strategies (hash3, deeper rep search) — sprint-sized
2. New entropy strategies (predefined tables) — multi-sprint

Future sprint planning should account for this: budget multi-sprint efforts for ratio work, not single sprints expecting big wins.
