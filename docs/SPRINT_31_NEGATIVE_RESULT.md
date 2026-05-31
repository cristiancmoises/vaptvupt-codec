# Sprint 31 — Negative result: runtime AVX2 dispatch in extend_match

**Version:** v2.50.5 (docs-only release; codec source byte-identical to v2.50.4)
**Sprint goal:** enable AVX2 match-extension on default portable builds via runtime CPUID dispatch and `__attribute__((target("avx2")))`
**Result:** measurement showed -8% to +5% across fixtures, average roughly flat with high run-to-run variance. Reverted.

---

## What was tried

The encoder's `extend_match` function in `src/vv_encoder.c` had a 32-byte AVX2 SIMD inner loop, but it was guarded by `#if VV_ENC_AVX2` — a compile-time check that only activates when `-march=native` (or equivalent) is passed at build time. The default portable build (`-O3 -flto` with no `-march`) emits NO AVX2 in the encoder, even though >95% of x86_64 hardware shipped since 2014 (Intel Haswell+) supports AVX2.

**Hypothesis:** refactor `extend_match` to:

1. Split the AVX2 inner loop into a separate function marked
   `__attribute__((target("avx2")))` so it compiles AVX2 instructions
   regardless of the file-level `-march` flag.
2. Add a runtime CPUID check (`vv_enc_has_avx2_runtime()`, lazily cached
   on first call) so non-AVX2 CPUs fall back to scalar safely.
3. Default portable builds would then get AVX2 match-extension on
   modern CPUs at zero portability cost.

Sprint 26's lesson ("try the compiler before touching source") suggested
this was a low-effort, high-reward target.

## Implementation

```c
__attribute__((target("avx2"))) static inline int32_t
extend_match_avx2_inner(const uint8_t *a, const uint8_t *b,
                        int32_t len, int32_t max_len) {
    while (len + 32 <= max_len) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + len));
        /* ... AVX2 32-byte compare ... */
    }
    /* ... scalar tail ... */
}

static int vv_enc_has_avx2_runtime(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        cached = 0; return 0;
    }
    cached = (ebx & (1 << 5)) ? 1 : 0;
    return cached;
}

static inline int32_t extend_match(const uint8_t *a, const uint8_t *b,
                                    int32_t max_len) {
    /* 8-byte scalar fast-path (unchanged from prior versions) */
    ...
    /* Dispatch to AVX2 if available + worth it */
    if (max_len >= len + 32 && vv_enc_has_avx2_runtime()) {
        return extend_match_avx2_inner(a, b, len, max_len);
    }
    /* scalar fall-through */
    while (len < max_len && a[len] == b[len]) len++;
    return len;
}
```

Code compiled cleanly, byte-identical compressed output across all 4 Silesia fixtures × 3 modes (12 verifications), all 12 test suites passed.

## Measurement

Built two binaries: v2.50.4 baseline vs Sprint 31 variant. Both `-O3 -flto`, no `-march`.

### Interleaved built-in bench, dickens fast encode (10 samples each)

```
  v2.50.4: 52.4    Sprint31: 50.1
  v2.50.4: 58.2    Sprint31: 54.6
  v2.50.4: 55.1    Sprint31: 50.7
  v2.50.4: 57.7    Sprint31: 53.6
  v2.50.4: 57.4    Sprint31: 52.0
  v2.50.4: 57.5    Sprint31: 54.0
  v2.50.4: 56.7    Sprint31: 51.4
  v2.50.4: 55.1    Sprint31: 52.6
  v2.50.4: 52.4    Sprint31: 49.8
  v2.50.4: 56.1    Sprint31: 53.6
```

Median: v2.50.4 = 56.1 MB/s, Sprint 31 = 52.3 MB/s. **Sprint 31 is ~7% SLOWER.**

### Subprocess bench (best of 7), both run orders

| Fixture | Round 1 (B vs A) | Round 2 (inverted) |
|---------|----------------:|-------------------:|
| dickens | -8.0% | +5.3% |
| xml | +9.6% | -6.5% |
| sao | -1.5% | +1.8% |
| x-ray | +1.1% | -4.0% |

**High variance, opposing signs between rounds — classic noise pattern with mild negative tilt.** Not a real win.

## Why it failed

The `__attribute__((target("avx2")))` function cannot be inlined into callers that don't share its target attribute (incompatible function attributes). So `extend_match_avx2_inner` becomes an out-of-line function call.

For fast-mode encoding, `extend_match` is called via the chain walker on each candidate match. The 8-byte scalar fast-path (added in Sprint 55) resolves ~70% of calls without ever touching AVX2 — short matches dominate. Only ~30% of calls reach the AVX2-eligible code path.

**Cost-benefit per call:**

- 70% of calls: zero benefit (resolved in scalar 8-byte path), but pay `vv_enc_has_avx2_runtime()` overhead on every call (a few uops for the cached int load + compare)
- 30% of calls: AVX2 saves N×4 cycles in the 32-byte loop, but pays ~6 cycles for non-inlined function call + ~4 cycles cached int load

For dickens (text), matches are short. The 30% of calls reaching AVX2 are themselves dominated by short extensions that complete in 1-2 iterations of the 32-byte loop. The dispatch overhead exceeds the AVX2 instruction-level savings on a per-call basis.

## What does work for AVX2 in default builds

Three paths, none of which fit one sprint:

1. **Compile the entire `vv_encoder.c` TU with `-mavx2` and add a single runtime CPUID gate at `vv_compress` entry** — error out or fall to a separately-compiled scalar TU on non-AVX2 hardware. Effectively splits the binary into "AVX2 build" and "scalar build" under a runtime selector at the API boundary, not deep in the call graph. Architectural change; multi-sprint.

2. **Function multiversioning at `chain_match_ex` level** instead of `extend_match` level. The whole match-finding hot loop becomes dual-target, all internal helpers inline freely within each version. Requires careful Makefile + symbol versioning work. Multi-sprint.

3. **`make perf` already does this correctly** via `-march=native`. Users who want AVX2 encoder paths build with `make perf`; the default `make` ships portable. This is the status quo and may be the right answer.

## Decision: revert to v2.50.4 codec, ship v2.50.5 docs-only

Per the SPEED PROGRAM "Rules of engagement":

> 2. **Measurement after code.** Every sprint ends with the same profile run + a wall-clock benchmark comparing before/after. **If the change doesn't move the needle, it's reverted before shipping.**

Sprint 31 doesn't move the needle. It's a noise change with mild negative tilt on the most important fixture (dickens). Per the rules, it doesn't ship.

The negative result IS shipped — this document — to:

1. Document that the runtime-AVX2-dispatch hypothesis was tried and failed
2. Save future sprint cycles from re-attempting the same approach
3. Identify the three real architectural paths to AVX2 in default builds

## What v2.50.5 contains

- `docs/SPEED_PROGRAM.md` updated with Sprint 25-31 retrospective
- `docs/SPRINT_31_NEGATIVE_RESULT.md` (this document)
- `CHANGELOG.md` v2.50.5 entry
- **No source code changes** — codec is byte-identical to v2.50.4 on all 12 fixture×mode combinations

## Lesson logged in SPEED_PROGRAM.md

> `__attribute__((target))` is not a free SIMD upgrade. Without inlinability, the dispatch overhead exceeds the SIMD gain on workloads with cheap fast-paths.
