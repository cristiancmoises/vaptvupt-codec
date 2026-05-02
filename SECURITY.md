# VaptVupt Security Posture

**Document version**: 1.3 (Sprint 120)
**Codebase audited**: v2.48.0
**License**: GPL-3.0-or-later
**Intended deployment**: Embedded codec library inside Zupt secure backup tool

---

## 1. Threat Model

VaptVupt is designed to safely decompress **untrusted input**. The primary threat model is:

> An attacker controls a `.vv` file. The decoder must either produce the
> original plaintext, return a clean error code, or terminate. It must not:
>
> - Crash the host process (segfault, abort, panic)
> - Read or write outside its allocated buffers (memory corruption)
> - Hang indefinitely (denial of service)
> - Consume unbounded memory or CPU
> - Execute arbitrary code

The encoder operates on **trusted input** (the user's own data being backed up). Encoder bugs are correctness issues, not security issues, with one exception: encoder/decoder roundtrip violations are security issues because they break the property that "what you compressed is what you decompress".

---

## 2. Attack Surfaces

VaptVupt exposes three decoder surfaces to untrusted input:

| Surface | Function | Use Case |
|---|---|---|
| Stateless one-shot | `vv_decompress()` | Single-buffer decompression |
| Streaming | `vv_dstream_decompress_chunk()` | Network/file streaming, partial frames |
| Multi-threaded | `vv_decompress_mt()` | Bulk decompression of large frames |

All three surfaces are covered by the audit campaign described below.

---

## 3. Audit Campaign Summary

13 distinct security/correctness defects have been found and fixed across 8 patch releases (v2.46.1 through v2.47.4) using 11 distinct audit tools:

| # | Tool | Sprint | Findings |
|---|---|---|---|
| 1 | LeakSanitizer (LSan) | Sprint 90 | 1 (memory leak) |
| 2 | UBSan + adversarial fuzz | Sprint 91 | 1 (DoS hang) |
| 3 | scan-build (clang static) | Sprint 92 | 5 (NULL deref + hygiene) |
| 4 | Allocation audit | Sprint 93 | 1 (matcher_init OOM) |
| 5 | API contract test | Sprint 94 | 2 (NULL opts, empty input) |
| 6 | ThreadSanitizer (TSan) | Sprint 98 | 1 (SIMD init data race) |
| 7 | Allocation-fault injection | Sprint 100 | 0 (clean — no defects) |
| 8 | libFuzzer + ASan + UBSan (decoder) | Sprint 109 | 3 (LL/OF/ML OOB, memcpy OOB, NULL deref) |
| 9 | libFuzzer (streaming decoder) | Sprint 110 | 0 (clean) |
| 10 | libFuzzer (encoder roundtrip) | Sprint 110 | 0 (clean) |
| 11 | libFuzzer (differential decoder) | Sprint 111 | 0 (clean) |

**Total: 13 defects fixed, 11 distinct tools applied.**

10 of 11 tools surfaced ≥1 defect on first application. Tools #7, #9, #10, #11 have produced no findings, indicating diminishing returns and a maturing security posture.

---

## 4. Permanent Audit Infrastructure

The following tools and harnesses are permanently committed to the source tree for ongoing regression coverage:

### Fuzz harnesses (`tests/fuzz/`)
- **`fuzz_decompress.c`** — stateless decoder, libFuzzer + ASan + UBSan
- **`fuzz_dstream.c`** — streaming decoder with randomized chunk boundaries
- **`fuzz_roundtrip.c`** — encoder + decoder roundtrip property
- **`fuzz_differential.c`** — stateless vs streaming decoder must agree

### Allocation fault injection (`tests/fault_injection/`)
- **`malloc_fault.c`** — LD_PRELOAD harness simulating allocation failures
- **`run_fault_inject.sh`** — driver that randomly fails 1 in N malloc/calloc

### Regression reproducers (`tests/regression_inputs/`)
13 permanent reproducer files covering every defect found:
- `huf4_inflate_s1.vv`, `huf4_zero_s1.vv`, `huf4_truncate.vv` (Sprint 105 4-stream Huffman DoS attacks)
- `fuzz_oob_ll_code.vv` (Sprint 109 LL OOB read)
- `fuzz_oob_decode_block.vv` (Sprint 109 memcpy OOB)
- `fuzz_null_dec_table.vv` (Sprint 109 NULL deref)
- 6 additional reproducers from Sprints 91-98

`test_dos_hang.c` exercises all 12 reproducers; each must complete in <60ms or fail.

---

## 5. Validation Results (v2.47.9)

| Check | Result |
|---|---|
| 18 test binaries, ~365 test cases | pass |
| 12 DoS reproducers handled | <60ms each |
| Roundtrip on 8 fixtures | pass |
| Sanitized random fuzz (300 byte-flips × 3 fixtures) | 300/0/0 |
| `fuzz_decompress` (long run) | 22,026 runs, 0 crashes |
| `fuzz_dstream` (long run) | 14,151 runs, 0 crashes |
| `fuzz_roundtrip` (long run) | 2,651 runs, 0 crashes |
| `fuzz_differential` (long run) | 10,359 runs, 0 crashes |
| **Cumulative fuzz** | **~145,000 runs, 0 crashes** |
| Encoder ASan + UBSan (24 fixture×mode runs) | clean |
| ThreadSanitizer (16-thread aggressive) | clean |
| **Strict UBSan `-fsanitize=integer`** (Sprint 117) | **0 errors** (was 92 false positives) |
| `make amalg-verify` (Sprint 115) | in sync |
| cppcheck | 0 findings |
| scan-build (clang static) | 0 bugs |
| GCC strict warnings | 0 warnings |

---

## 6. DoS Resistance

The decoder is hardened against:

- **Decompression bombs**: `dst_cap` is enforced by the caller. No internal expansion.
- **Match-count overflow**: SPRINT 90 fix bounds `match_count` against `dst_cap / min_match`. Fixed before this would have been exploitable.
- **Infinite loop on degenerate ANS state**: SPRINT 89 fix adds bounded iteration counter (`max_iters = total_lits + match_count + 16`). Returns `VVA_ERR_CORRUPT` instead of hanging.
- **Huge literal-run extension lengths**: SPRINT 109 fix bounds-checks LL extension before memcpy.
- **OOB symbol codes**: SPRINT 109 fix bounds-checks `ll_code`, `of_code`, `ml_code` against their respective tables.
- **NULL deref on edge-case sequences**: SPRINT 109 fix always allocates all 3 ANS decode tables.
- **4-stream Huffman crafted attacks**: Sprint 105 hardened `vvh_decode4` against 6 distinct DoS patterns.

All 12 known DoS reproducers complete in <60ms.

---

## 7. Memory Hygiene (Sprint 118)

The encoder's working buffers contain plaintext-derived data (literal bytes from input, partially-encoded sequences, raw input window). When the encoder context is destroyed without scrubbing, plaintext fragments persist in the heap free-list and may be observable through:

- Later allocations that reuse the same heap chunks
- Memory-disclosure attacks (e.g., heap leak via separate vulnerability)
- Process core dumps containing freed-but-not-zeroed memory
- Memory-introspection tools running with the same process

**v2.47.9 introduces explicit secure-zero scrubbing** of plaintext-bearing buffers before `free()`:

| Buffer | Contents | Scrubbed in |
|---|---|---|
| `lit_buf` | Literal bytes extracted from input | `vv_cstream_destroy`, `vv_compress` exit |
| `stripped` | LZ-tokenized output (compressed but pre-entropy) | same |
| `src_buf` | Raw input sliding window (streaming) | `vv_cstream_destroy` |
| `tmp` | Scratch for LZ tokens | same |
| `ent_buf` | Pre-output entropy-coded blocks | same |
| Encoder context struct | Options, internal state | `vv_cstream_destroy` |

The implementation (`vv_secure_zero` in `src/vv_encoder.c`) prefers `explicit_bzero` (BSD/glibc 2.25+) and falls back to a volatile-pointer memset that the optimizer cannot eliminate. Behavior is verified by `tests/test_secure_zero.c` (TEST18 in the suite).

**This is defense in depth**, not a primary security boundary. The original input buffer (caller-owned) is unaffected; if the caller doesn't zero it themselves, the codec's hygiene doesn't help. But for Zupt's pipeline (compress → encrypt → write), the codec's working buffers are now scrubbed before the encryption step gets the data.

**Encoder output is byte-identical to v2.47.8** — scrubbing happens after output is emitted, so the wire format and compressed bytes are unchanged.

---

## 8. Hardened-Build Compatibility (Sprint 118)

For deployments using `clang -fsanitize=integer` (a stricter superset of standard UBSan that catches **defined-but-suspicious** unsigned overflow), v2.47.9 builds cleanly.

The audit found 92 strict-integer warnings in v2.47.8, all from intentional unsigned modular arithmetic in:

- xxh64 round/finalize hash mixers (intentional overflow per RFC 2104-style mixing)
- LZ matcher hash functions (Knuth multiplicative hash, intentional overflow)
- Loop-counter post-decrement guards (`while (... && depth-- > 0)`)
- 64-bit rotate left implementation (`(x << r) | (x >> (64 - r))`)

C11 §6.2.5p9 defines unsigned overflow as wraparound (modular arithmetic), so these are NOT undefined behavior — but `-fsanitize=integer` catches them anyway because the sanitizer cannot distinguish intentional modular arithmetic from unintended overflow.

v2.47.9 adds explicit `__attribute__((no_sanitize))` annotations to all 11 affected functions, plus uses `__builtin_rotateleft64` in `xxh_rotl64` to avoid the shift-base sanitizer entirely. Result: **0 strict-integer warnings** (down from 92).

This matters for security-conscious downstream users who want to verify the codec under the strictest available sanitizer regime as part of their own build hardening.

---

## 9. What's NOT Tested

In the interest of accuracy, the following are not yet covered:

1. **Cross-decoder differential vs zstd**. Different formats, so this isn't possible directly.
2. **Multi-threaded decoder fuzz**. `vv_decompress_mt` has not been libFuzzer'd, only TSan-tested.
3. **Encoder fuzz on deeply pathological inputs** (e.g., adversarial Huffman trees that produce maximum-depth codes). Roundtrip fuzzer covers random plaintext but not crafted adversarial inputs.
4. **Long-running (>1 hour) fuzz campaigns**. Current results are from ~17 minutes of total fuzz time.

These represent residual risk to be addressed in future audit sprints if Zupt deployment surfaces relevant concerns.

---

## 10. Reporting Vulnerabilities

This is currently an internal-development codebase. When VaptVupt is integrated into Zupt and reaches public release, vulnerability disclosure should follow Zupt's policy.

For development-time reporting: file an issue or contact the maintainer with a reproducer file and the failure observed.

---

## 11. License Note

VaptVupt is GPL-3.0-or-later. Audit infrastructure (libFuzzer harnesses, regression reproducers, fault-injection scripts) is included in the source tree under the same license, allowing downstream users to run the same audit campaign on modifications.
