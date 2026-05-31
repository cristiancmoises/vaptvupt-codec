# Sprint 19 — Baseline measurement

VaptVupt v2.48.2 vs zstd-3, measured on Linux x86_64 with `vaptvupt -c -m extreme` and `zstd -3`.

## Per-fixture ratio table

| Fixture | Raw size | vv-extreme | zstd-3 | Δ (% vs zstd-3) |
|---------|---------:|-----------:|-------:|----------------:|
| dickens | 10,192,446 | 3,818,656 | 3,669,252 | **+4.07%** (loss) |
| xml | 5,345,280 | 643,067 | 639,138 | **+0.61%** (loss) |
| sao | 7,251,944 | 5,410,425 | 5,551,158 | −2.54% ✓ |
| x-ray | 8,474,240 | 5,881,236 | 6,086,279 | −3.37% ✓ |

Aggregate (8-fixture per PERFORMANCE.md): −1.07% (vv wins).

Fixtures fx_text, fx_json, fx_source, bash from PERFORMANCE.md are external project-specific synthetics not included in the codec source tarball. Reproducing exact numbers on those is out of reach for this sprint.

## Picked target: **dickens** (+4.07%)

- Biggest gap among reproducible fixtures
- Pure English prose — entropy/parser issues are well-understood
- 10 MB enough to expose entropy-coding issues clearly
- xml's +0.61% is within noise; the dickens gap is the higher-signal target

## Block-level profile of `dickens.vv` (3,818,656 bytes total)

| Section | Bytes | % of output |
|---------|------:|------------:|
| Frame header + footer (16 + 12) | 28 | 0.0% |
| Block overhead (10 × 8B) | 80 | 0.0% |
| SEQ meta (lit_count, lit_fmt, match_count, seq_bs_len) | 170 | 0.0% |
| Literal payload (lit_fmt=4 / 4-stream Huffman) | 294,432 | 7.7% |
| ML+OF+LL ANS table headers | 2,103 | 0.1% |
| ANS initial states | 60 | 0.0% |
| **Sequence bitstream (ML/OF/LL ANS-coded)** | **3,521,783** | **92.2%** |
| **Total** | **3,818,628** | 100% |

Literals are already optimally coded (4-stream Huffman). The 149 KB gap to zstd-3 is in the sequence bitstream (ML/OF/LL ANS streams). At 3.52 MB total, closing 149 KB is a ~4.2% improvement on this section — non-trivial but possible.

## Per-sequence statistics

- 10 blocks, each 1 MB except the last (755 KB)
- Total sequences: ~1.28 million
- Average: ~127K matches per 1MB block, ~46K literals per block
- Average literal-run length: 0.36 (most sequences have litlen=0 or 1)
- Avg bits per literal in output: 5.11 bits/byte (294,432 × 8 / 460,653)

## Suspected root cause

The encoder's cost-aware lazy parser uses `literal_bits = 6` as a constant in its shift/no-shift cost model (see `src/vv_encoder.c` ~line 787). The actual average on dickens is **5.11 bits/byte** — a 17% overestimate.

A high `literal_bits` constant biases the parser toward "emit current match, don't shift" decisions: shifting "costs" 6 simulated bits (one literal) plus the new match cost, vs emitting the current match. With literals cheaper than the constant assumes, we underweight the value of shifting on text-heavy content. Result: more, shorter matches → bigger ML/LL streams.

This is the testable hypothesis for Sprint 19's fix.

## Constraints

- Sprint 121 explicitly tested `mlen<5` (more aggressive shifting) and rejected it: fx_json regressed 8.5pp. So tuning the *gate* is exhausted.
- Sprint 121 tested lazy-2: dickens regressed 0.912%, sao regressed 0.874%. So tuning the *probe depth* is exhausted.
- What hasn't been tested: tuning **the cost model constant itself**.

## Verification plan

1. Try `literal_bits = 5` (matches dickens actual): re-measure all 4 fixtures.
2. If dickens improves and no other fixture regresses > 0.1%, ship it.
3. If anything regresses meaningfully, revert and document negative result.

This is a one-line code change. The aggregate ratio target (−1.07%) must hold.
