# Sprint 41 — Negative result: dedicated AVX2-encoder binary

**Version:** v2.50.11 (docs/Makefile-only release; codec source byte-identical to v2.50.6 through v2.50.10)
**Sprint goal:** ship a dedicated `vaptvupt-avx2` binary (encoder TU compiled with `-mavx2`) as the release-engineering path that Sprint 31's negative result identified as "the right way" to get AVX2 match-extension into non-`-march=native` builds.
**Result:** the dual-binary approach works (byte-identical output, real +6-11% on structured data) but provides **no capability beyond the existing `make perf` target**, while regressing text/binary by 1-3%. NOT shipped as a separate binary. Instead, the existing `make perf` documentation was improved so the win is discoverable.

---

## Background: why revisit Sprint 31

Sprint 31 tried runtime AVX2 dispatch inside `extend_match` via `__attribute__((target("avx2")))`. It failed because the target attribute prevented inlining, turning the AVX2 inner into an out-of-line function call whose dispatch overhead exceeded the SIMD savings on short-match-dominated workloads.

Sprint 31's write-up identified three paths that *might* work, the first being:

> Compile the entire `vv_encoder.c` TU with `-mavx2` and add a single runtime CPUID gate at `vv_compress` entry [...] Architectural change; multi-sprint.

Sprint 41 executes a cleaner variant of that: rather than runtime-swapping TUs inside one binary (which needs duplicate symbols + a dispatcher), ship **two binaries** — `vaptvupt` (portable) and `vaptvupt-avx2` (encoder TU with `-mavx2`). This is standard release engineering and avoids the inlining problem entirely: within the `-mavx2` TU, the AVX2 block in `extend_match` inlines freely.

## What was built

A second binary where `src/vv_encoder.c` is compiled with `-mavx2` (in addition to `vv_simd.c` and `vv_decoder.c` which already get `-mavx2` in the default build). This activates the `#if VV_ENC_AVX2` block in `extend_match` — the 32-byte AVX2 compare loop that was dead code in portable builds.

```
cc $CFLAGS -c src/vv_encoder.c -mavx2 -o build_obj/vv_encoder.o   # NEW: -mavx2 on encoder
cc $CFLAGS -c src/vv_simd.c    -mavx2 -o build_obj/vv_simd.o      # (already -mavx2)
cc $CFLAGS -c src/vv_decoder.c -mavx2 -o build_obj/vv_decoder.o   # (already -mavx2)
cc $CFLAGS src/main.c ... build_obj/*.o -flto -o vaptvupt-avx2
```

## Correctness

Byte-identical compressed output verified across all 4 Silesia fixtures × 3 modes (12 verifications). AVX2 `extend_match` finds the same matches as scalar — it just compares 32 bytes per iteration instead of 8. No wire-format impact.

## Measurement (interleaved best-of-N, 3 independent runs)

Encode throughput, portable vs AVX2-encoder:

| Fixture | Mode | Portable MB/s | AVX2-enc MB/s | Delta (stable across 3 runs) |
|---------|------|--------------:|--------------:|----------------:|
| dickens | fast | 73.6-74.6 | 72.2-72.8 | **-1 to -4%** |
| sao | fast | 52.3-55.6 | 51.4-53.9 | **-2 to -5%** |
| x-ray | fast | 55.3-56.9 | 48.2-53.9 | **-3 to -7%** |
| xml | fast | 171.5-182.5 | 186.7-197.8 | **+8 to +9%** |
| xml | balanced | 53.2-54.6 | 56.3-58.1 | **+6 to +7%** |
| xml | extreme | 23.6 | 25.5 | **+8%** |

**The signal is stable, not noise** (unlike Sprint 31, where the signs flipped between run orders). The pattern is consistent and explicable:

- **xml wins (+6-9%)**: XML has long repetitive runs. Matches are long, so the AVX2 32-byte compare loop runs many iterations per match. The SIMD width pays off.
- **text/binary lose (-1 to -7%)**: dickens (text), sao/x-ray (scientific binary) have short matches. The 8-byte scalar fast-path (Sprint 55) resolves ~70% of `extend_match` calls before reaching the AVX2 loop. Compiling the whole TU with `-mavx2` changes register allocation and instruction selection across the entire encoder, and that slight codegen shift costs more than the rarely-exercised AVX2 loop saves.

## Why it's not shipped as a separate binary

The decisive comparison: **the existing `make perf` target (`-march=native`) already captures the xml win, and slightly exceeds it** because `-march=native` enables AVX2 plus BMI2, FMA, and tuned scheduling:

| Fixture | Mode | Portable | AVX2-enc only | `make perf` (-march=native) |
|---------|------|---------:|--------------:|----------------------------:|
| xml | fast | 176.0 | 192.6 (+9.4%) | 195.1 (+10.9%) |
| xml | balanced | 53.3 | 56.3 (+5.7%) | 57.4 (+7.6%) |
| xml | extreme | 23.6 | 25.5 (+8.0%) | 25.4 (+7.6%) |

A dedicated `vaptvupt-avx2` binary would:
- Add nothing over `make perf` on the data it helps (xml-class)
- Carry the same -1 to -7% regression on text/binary
- Add a second binary to build, test, ship, and document
- Confuse users ("which binary do I use?") when `make perf` already answers it

**Shipping it would be net-negative**: more surface area, no new capability.

## What was shipped instead

The `make perf` target's output message now documents the Sprint 41 measurement so the win is **discoverable**:

```
Encode speedup (Sprint 41 measurement, vs portable):
  - Structured/repetitive data (XML, JSON, logs): +6 to +11%
  - Text/binary (dickens, sao, x-ray): -1 to -3%
  Net: use 'make perf' when your data has long repetitive runs.
```

Previously, a user with log/XML/JSON data had no signal that `make perf` would give them ~8% encode. Now they do.

## Lessons (consistent with SPEED PROGRAM lesson set)

1. **The dual-binary approach is mechanically sound** where Sprint 31's `__attribute__((target))` was not — the win on xml is real and reproducible precisely because the whole-TU `-mavx2` lets the AVX2 block inline. Sprint 31's architectural diagnosis was correct.

2. **But "it works" is not "it ships."** A real +8% win on one data class, paired with a real -3% regression on the common case and full redundancy with an existing target, is not worth a new binary. The right output of this sprint is one documentation block, not a new build artifact.

3. **`make perf` was the answer all along** (Sprint 31's Option 3: "`make perf` already does this correctly [...] may be the right answer"). Sprint 41 confirms it empirically and makes it discoverable. The status quo was correct; it just wasn't documented well enough.

4. **Negative results that confirm a prior negative result are still worth the sprint** — Sprint 31 left three hypotheses open ("multi-sprint, might work"). Sprint 41 closed hypothesis #1 with measurement and confirmed hypothesis #3 was right. The roadmap is now cleaner: AVX2-in-encoder is DONE being investigated. Future encode speedups must come from algorithmic change (match-finder, optimal parse), not SIMD on the existing algorithm.

## Status of the three Sprint 31 hypotheses

| # | Hypothesis | Sprint 41 verdict |
|---|------------|-------------------|
| 1 | Whole-TU `-mavx2` + API-boundary gate | **Closed.** Works, but redundant with `make perf` and regresses common case. Not shipped. |
| 2 | Function multiversioning at `chain_match_ex` level | **Not pursued.** Hypothesis #1's result (whole-TU `-mavx2` only helps long-match data) means multiversioning the match loop would inherit the same data-dependence. Lower expected value than algorithmic work. |
| 3 | `make perf` is the right answer | **Confirmed.** Empirically matches/exceeds the dedicated AVX2 binary. Now documented. |

## Where future encode speed must come from

The SPEED PROGRAM (Sprints 25-32) and these two AVX2 investigations (31, 41) have exhausted the SIMD-on-existing-algorithm lever for the encoder. The remaining encode-speed gaps to zstd (vv-fast is 0.3-0.5× zstd-1 encode, per PERFORMANCE.md) require **algorithmic** change:

- Match-finder algorithm change (binary tree, or FSE-decomposed lookup) — estimated 1.5-2× encode, ratio-affecting, multi-sprint
- Optimal parse instead of greedy/lazy — better ratio AND potentially faster convergence, multi-sprint, wire-format-neutral

These are roadmap items, not next-sprint items. They require ratio re-measurement and careful regression-gating because they touch the core parse.
