# Sprint 20 — Negative result: rep-only lazy-2 probe

## Outcome

**Negative result.** A rep-only lazy-2 probe at pos+2 was implemented and tested. It produced **corrupt output that failed roundtrip decode** on the xml fixture (compressed 5,345,280 bytes → 1,633,665 bytes claimed, but `vaptvupt -d` returned error -2 on the result). The variant was reverted; codec is byte-identical to v2.48.3 / v2.48.2.

## What was implemented

Direction C from Sprint 19's negative-result doc: extend the lazy parser to also check rep matches at pos+2, in addition to the existing pos+1 probe. The hypothesis was that rep-only (no hash-chain walk) lazy-2 would avoid the shift-cascade problem Sprint 121 documented for full lazy-2.

Code change in `src/vv_encoder.c` (around line 735, in the lazy-probe block):

1. After the existing pos+1 chain+rep probe, added an O(1) rep-only scan at pos+2 (only when neither current match nor pos+1 candidate is a rep, to avoid cascades).
2. Added a parallel cost-aware decision: if `(2*literal_bits + 2) * mlen < moff_bits * r2len`, shift pos by 2 and take the rep match.

## Measurement (variant vs v2.48.3 baseline)

| Fixture | Baseline | Variant | Δ bytes | Δ % | Roundtrip |
|---|---:|---:|---:|---:|:-:|
| dickens | 3,818,656 | 3,819,857 | +1,201 | +0.031% | OK |
| **xml** | **643,067** | **1,633,665** | **+990,598** | **+154.043%** | **❌ DECODE FAILED** |
| sao | 5,410,425 | 5,414,484 | +4,059 | +0.075% | OK |
| x-ray | 5,881,236 | 5,881,217 | −19 | −0.000% | OK |

The xml result is not just a ratio regression — it's a correctness failure. `vaptvupt -d` returned error -2 (VV_ERR_CORRUPT) when attempting to decompress.

## Root cause analysis

The implementation bug is a hash-table consistency violation:

When the variant's pos+2 shift triggers, it does `pos += 2; mlen = r2len; ...` and proceeds to emit the rep match. But unlike the pos+1 shift path (which already did `matcher_insert(m, src, pos, end)` at the start of the lazy-probe block to insert the original pos), the pos+2 shift skips position pos+1 entirely without inserting it into the hash table.

Consequence: subsequent block positions may receive incorrect (offset, length) tuples from the hash chain because the chain is missing entries. On xml — which has many short, structurally similar patterns at small offsets — this cascades into invalid matches that reference positions where the data doesn't actually match. The encoder still writes valid-looking byte sequences, but the offsets don't decode back to the original input.

The xml decoder fails because the corrupted match offsets violate the wire-format constraint (`offset <= 1 << window_log`) or produce out-of-range copies that the decoder rejects with VV_ERR_CORRUPT.

## Why this isn't fixable by adding hash insertion

The obvious fix — insert pos+1 into the hash before doing `pos += 2` — would close the correctness bug. But this turns the rep-only lazy-2 into "do the original work of inserting pos+1, AND check rep at pos+2." The implementation work is no longer O(1). And:

1. Even with correct hash insertion, the cost model error compounds in the same way Sprint 121 saw. A shift by 2 followed by another shift on the next iteration becomes a 3-position cascade — the same dynamic that broke Sprint 121's lazy-2, just deferred by one position.

2. The dickens result (the actual target) was +0.031% — a regression on the fixture this sprint was supposed to improve. Even if the xml correctness bug were fixed, dickens shows no benefit from the change. The hypothesis ("rep at pos+2 catches common English-prose repeats") was wrong.

3. Repairing the hash insertion would require ~10 more tool calls to design, implement, and validate without breaking anything else. The sprint budget is approaching exhaustion, and the underlying hypothesis is unsupported by the dickens measurement.

## What this tells us about the dickens gap

The +4.07% dickens loss vs zstd-3 is NOT closable by lazy-parser extensions in the current architecture. Both Sprint 121 (full lazy-2) and Sprint 20 (rep-only lazy-2) confirmed this: any additional shift logic either regresses dickens directly or fails to materialize the expected gain.

The remaining unexplored direction is **Direction B (hash3 matcher)** — adding a 3-byte hash table to the matcher so the encoder can find min_match=3 matches that the current 4-byte hash misses. The wire format already supports this via the 'T' (SEQ_V2) tag. This is a more invasive change than lazy-parser tuning, but it's mechanically separate (lives in `find_match` / hash-table code, not the parser cost model) and is the next direction the Sprint 19 negative-result doc identified.

## Next steps (Sprint 21)

**Direction B: hash3 matcher**. Estimated 1–2 sprints. The change:

1. Add a 3-byte hash table parallel to the existing 4-byte/5-byte hashes
2. In extreme mode, race hash3 matches alongside hash4/hash5 in `chain_match`
3. Emit the resulting min_match=3 matches via the existing 'T' (SEQ_V2) tag path
4. Measure on all 4 fixtures; ship if dickens improves ≥0.5% without regressions

If Sprint 21 also delivers <0.2% on dickens (or causes regressions), then **Direction A (predefined ANS tables)** is the only remaining option, and that's a 2–3 sprint architectural change.

## Lesson

Lazy-parser tuning is exhausted. The dickens gap lives in the entropy-coding section (per Sprint 19's profile: 92.2% of the output), not in match selection. Match-selection changes that touch the parser cost model are blocked by:

- Cost-model approximation errors that compound with each shift
- Hash-table consistency requirements that defeat O(1) extensions
- Sprint 121's documented finding that lazy-2 of any kind regresses dickens

Future ratio work on dickens should focus on:

- The match-finder itself (hash3, longer chains) — affects the SET of candidate matches, not the parser logic
- Entropy-table efficiency (predefined ANS tables, better frequency adaptation) — affects how matches are encoded once chosen

Both are bigger changes than lazy-parser tuning. The codec is at a local optimum for its current match-finder + parser architecture.
