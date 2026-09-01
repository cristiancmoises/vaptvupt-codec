# VaptVupt Formal Audit Document

**Document version**: 1.2
**Codebase audited**: v2.65.8
**License**: GPL-3.0-or-later
**Intended deployment**: Embedded codec library inside the VaptVupt secure backup tool, plus general-purpose use as a zstd/lz4 alternative

This document is the formal audit reference for VaptVupt. It specifies what has been verified, by which mechanism, against what threat model, with what limits. It is intended to satisfy the due-diligence requirements of:

- Downstream library consumers integrating VaptVupt as a compression dependency
- Security-conscious deployments verifying the codec under hardened-build sanitizer regimes
- Internal review processes at downstream teams
- Independent third-party security auditors performing pre-integration review

It is intentionally specific about what is **not** verified, to support honest risk assessment.

> **Currency note (v2.65.8):** historical sprint references below record the
> original evidence and tool availability. Sprint 138 added the truncated
> offset regression and streaming API misuse checks. Sprint 137 added the SEQ
> combined-run safe-zone regression, frame-window validation, and repaired
> sanitizer/OOM/fuzzer gate propagation. Re-run the commands on the target
> toolchain before treating historical results as current certification.

---

## 1. Threat Model

The codec is part of a layered system. Threats are categorized by which layer is responsible for mitigating them.

### 1.1 In-scope threats — codec must mitigate

| Threat | Mitigation |
|---|---|
| **T1: Decompression bomb** | Caller-provided `dst_cap` is enforced. No internal expansion. Decoder rejects any sequence that would exceed `dst_cap` with `VVA_ERR_BOUNDS`. |
| **T2: Crafted match-count overflow** | Sprint 90 fix bounds `match_count` against `dst_cap / min_match` before allocation. |
| **T3: Infinite loop on degenerate ANS state** | Sprint 89 fix adds bounded iteration counter (`max_iters = total_lits + match_count + 16`). Returns `VVA_ERR_CORRUPT` instead of hanging. |
| **T4: Huge literal-run extension lengths** | Sprint 109 bounds-checks LL extension before memcpy. |
| **T5: Out-of-bounds symbol codes** | Sprint 109 bounds-checks `ll_code`, `of_code`, `ml_code` against their respective tables (`VVA_LL_CODES=36`, `VVA_OF_CODES=27`, `VVA_ML_CODES=36`). |
| **T6: NULL deref on edge-case sequences** | Sprint 109 always allocates all 3 ANS decode tables, regardless of empty-symbol-count edge cases. |
| **T7: Crafted 4-stream Huffman attacks** | Sprint 105 hardens `vvh_decode4` against 6 distinct DoS patterns: invalid stream-length headers, mismatched stream lengths, OOB final-bit positions, malformed lit_fmt=4 selection, and two integer-overflow paths in stream-header parsing. |
| **T8: Allocation failure mid-decode** | Sprint 100 fault-injection harness validates the decoder against 250+ allocation-failure points with no crashes / leaks / UB. |
| **T9: Plaintext recovery via heap residue** | Sprint 118 `vv_secure_zero` scrubs all encoder working buffers (literals, LZ-tokenized output, raw input window, scratch) before `free()` — defense in depth. |
| **T10: Hardened-build false positives** | Sprint 117 `VV_NO_SANITIZE_INTEGER` annotations on 12 functions performing intentional unsigned modular arithmetic; `__builtin_rotateleft64` for the rotate. Result: 0 strict-integer warnings. |
| **T11: Encoder mid-stream abort with state leak** | Streaming encoder's `vv_cstream_destroy` scrubs full context including all sub-buffers. Verified by `tests/test_secure_zero.c`. |
| **T12: Concurrent encoder/decoder thread races** | ThreadSanitizer (Sprint 98 + 105) clean across multi-threaded encode + decode using lit_fmt=4. No false sharing detected; no shared mutable state across thread boundaries. |

### 1.2 Out-of-scope threats — caller / system must mitigate

| Threat | Why out of scope |
|---|---|
| Confidentiality of the original input | Caller's responsibility. The codec never claims to encrypt; an attacker with the compressed bytes can recover the original. Use AEAD (VaptVupt does). |
| Authentication / integrity of compressed output | Caller's responsibility. The codec emits an XXH64 checksum optionally but this is for accidental-corruption detection, not authentication. Use AEAD. |
| Key management for downstream encryption | Out of scope by definition — the codec doesn't see keys. |
| Side-channel attacks (timing, cache, power) on the encoder | Not mitigated. The encoder's runtime depends on the input data. If the input is sensitive, use a constant-time encryption layer downstream. |
| Side-channel attacks on the decoder | Not mitigated for the same reasons. |
| OS-level threats (memory dumps, ptrace, etc.) | The OS must protect its own primitives. `vv_secure_zero` reduces the *codec's* contribution to heap residue but doesn't and can't fix the OS. |
| Supply-chain attacks against the Anthropic publishing pipeline | Out of scope for the codec. Verify amalgamation hashes against the published GitHub repo. |

### 1.3 Trust boundaries

```
                               ┌─────────────────────────────────┐
                               │  Caller (VaptVupt or 3rd party)     │
                               │  - owns input bytes             │
                               │  - owns dst_cap                 │
                               │  - owns key material            │
                               └─────────────────────────────────┘
                                              │
                               ┌──────────────▼───────────────────┐
TRUST   compressed bytes ────► │  VaptVupt decoder                │
BOUNDARY    (untrusted)        │  - bounds-checks every read      │
↑                              │  - rejects malformed input       │
↑                              │  - never expands beyond dst_cap  │
↑                              │  - bounded loop iteration count  │
↑                              └──────────────┬───────────────────┘
                                              │
                                  ┌───────────▼──────────┐
                                  │  Decompressed bytes  │
                                  │  back to caller      │
                                  └──────────────────────┘
```

The decoder **must assume** the input bytes are adversarial. The encoder **may assume** the input bytes are non-adversarial (a friendly file from the caller's filesystem) — the encoder is only in scope for resource-exhaustion DoS, not crafted-input attacks, since by definition the encoder runs on data the caller chose.

---

## 2. Verification Mechanisms

Each verification mechanism is permanently integrated into the build and the CI cycle. Re-running them on a fresh checkout is one command per mechanism.

### 2.1 Static analysis

| Tool | Invocation | Result on v2.48.1 |
|---|---|---|
| cppcheck (warning + performance + portability) | `cppcheck --enable=warning,performance,portability -I include src/` | 0 findings |
| clang scan-build | `scan-build --status-bugs make` | 0 bugs |
| GCC strict warnings | `make CFLAGS="-Wpedantic -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wunreachable-code -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wundef -Wuninitialized"` | 0 hits |

All three tools have been run continuously since v2.46.0 and the codebase has remained at zero findings across the audit campaign.

### 2.2 Runtime sanitizers (encoder + decoder)

| Sanitizer | Invocation | Result |
|---|---|---|
| ASan | `clang -fsanitize=address` + 8 fixtures × 3 modes × encode | clean (24/24 runs) |
| UBSan (standard) | `clang -fsanitize=undefined` + same matrix | clean (24/24 runs) |
| UBSan **strict integer** (`-fsanitize=integer`) | `clang -fsanitize=integer,implicit-conversion,shift,undefined,address` + same matrix | **0 errors** (was 92 false positives before Sprint 117/118 annotations) |
| LSan | `clang -fsanitize=leak` + same matrix | clean (24/24 runs) |
| MSan | `clang -fsanitize=memory` + same matrix | clean |
| ThreadSanitizer | 16-thread aggressive encode + decode using lit_fmt=4 | clean (12/12 runs) |

The strict-integer pass deserves special note: this is a stricter superset of standard UBSan that catches **defined-but-suspicious** unsigned modular arithmetic. C11 §6.2.5p9 explicitly defines unsigned overflow as wraparound, so the codec's xxh64 hash mixers and Knuth multiplicative hash functions are technically not undefined behavior — but `-fsanitize=integer` flags them anyway. Hardened-build deployments running with this flag would otherwise see thousands of false-positive runtime errors. The Sprint 117/118 annotations make the codec clean under this regime.

### 2.3 Differential fuzzing (4 surfaces)

Permanent libFuzzer harnesses in `tests/fuzz/`:

| Harness | What it tests | Cumulative runs (v2.48.1) | Crashes |
|---|---|---|---|
| `fuzz_decompress` | One-shot decoder against arbitrary byte sequences | 47,331 | 0 |
| `fuzz_dstream` | Streaming decoder under arbitrary chunk boundaries | 24,461 | 0 |
| `fuzz_roundtrip` | encode → decode → verify equal to input | 3,319 | 0 |
| `fuzz_differential` | One-shot decoder vs streaming decoder, same input → same output | 24,178 | 0 |
| **Cumulative** | | **~99,000 documented + ~46,000 added Sprint 117–118 ASan/UBSan re-runs = ~145,000** | **0** |

All four harnesses are run with `-fsanitize=address,undefined`. Reproducer corpora are checked in at `tests/fuzz/corpus_*/` for regression coverage.

### 2.4 DoS reproducer suite

Twelve saved adversarial payloads, each a known historical attack:

- 6 from v2.46.x (lit_fmt=3 stream-header, ANS state divergence, OOB symbol codes, NULL deref on empty-table edge cases, match-count overflow, infinite-loop ANS)
- 3 from v2.47.x lit_fmt=4 (4-stream offset overflow, stream-length-mismatch, malformed selection)
- 3 from Sprint 109 (LL extension overflow, OOB ll_code, deg-degenerate Huffman)

`make test_dos_hang` exercises all twelve. **All complete in <60ms each** on the reference hardware (commodity x86_64). Without the Sprint 89-109 fixes, these payloads would hang or crash.

### 2.5 Allocation-fault injection

`tests/fault_injection/` (Sprint 100) wraps `malloc`/`calloc`/`realloc` with an interposer that fails the *N*th call. The harness sweeps *N* across 250+ values for both decode and roundtrip paths. **0 crashes / leaks / UB** observed.

This is the strongest available evidence that the codec correctly handles allocation failure at every internal allocation site. The interposer is invoked with `LD_PRELOAD=tests/fault_injection/libfault.so`.

### 2.6 API contract checks

`tests/test_api_contract.c` (17 tests) exercises the documented public API contract:

- NULL-pointer handling on every public function
- `dst_cap = 0` boundary
- `src_len = 0` boundary
- `vv_compress_bound` correctness (must always return ≥ actual output for any valid input)
- `vv_dstream_create` failure paths
- Caller-owned vs codec-owned memory boundaries
- Streaming reset after error

All 17 pass. These are **contracts**, not implementation tests — if the implementation changes, the contract MUST still hold.

### 2.7 Format conformance

Two reference decoders independent of the production C decoder:

- `reference/vv_decoder.py` — byte-exact pure Python, easy to inspect
- `reference/vv_decoder.js` — byte-exact JavaScript, validates the wire format spec

Both reference decoders are used as differential-test oracles. The `fuzz_differential` harness compares C decoder output against the reference; any divergence is a failed fuzz run.

The wire format itself is documented in `FORMAT.md` with byte-level diagrams of every block type, header, and ANS table format. The format has been **frozen since v1.0.0** for the v1 path; v2 (opt-in, format_v2 flag) has been frozen since v2.33.0.

### 2.8 Memory hygiene tests

`tests/test_secure_zero.c` (Sprint 118, 4 tests):

1. Streaming destroy completes cleanly under sanitizers (no double-free, no use-after-free)
2. 100 alloc/destroy cycles do not leak or corrupt
3. One-shot `vv_compress` scrub path validates
4. Encoder context struct is fully scrubbed (verified by sentinel pattern)

All 4 pass.

### 2.9 Amalgamation drift detection

`make amalg-verify` (Sprint 115) checks that `build/vaptvupt.c` and `build/vaptvupt.h` are byte-identical to a re-amalgamation from `src/`. This catches:

- Stale amalgamation in the published artifact
- Any divergence between distributed single-file and the modular source

Currently in sync.

---

## 3. Defects Fixed in the Audit Campaign

The audit campaign began at Sprint 86 (v2.46.0) and has produced **13 distinct defect fixes** across **11 distinct tools**. Every fix has a regression test or a permanent fuzz reproducer.

| # | Defect | Severity | Sprint | Patch |
|---|---|---|---|---|
| 1 | ANS state infinite-loop on degenerate input | DoS | 89 | `vv_ans.c` bounded iter counter |
| 2 | match_count overflow in decoder | DoS | 90 | `vv_ans.c` bounds against dst_cap |
| 3 | Empty-symbol-table NULL deref | DoS | 109 | `vv_ans.c` always-allocate |
| 4 | OOB ll_code / of_code / ml_code reads | DoS | 109 | `vv_ans.c` bounds-check before lookup |
| 5 | LL extension length overflow | DoS | 109 | `vv_ans.c` bounds-check pre-memcpy |
| 6 | 4-stream Huffman header injection | DoS | 105 | `vv_huffman.c` 6 distinct hardenings |
| 7 | Streaming-decoder chunk-boundary state divergence | Correctness | 98 | `vv_decoder.c` partial-block state |
| 8 | Allocation-failure crash on encoder destroy path | Robustness | 100 | `vv_encoder.c` NULL-check |
| 9 | Allocation-failure leak on streaming decoder context | Robustness | 100 | `vv_decoder.c` cleanup ordering |
| 10 | Header-size underflow on 1-byte input | Correctness | 113 | `vv_encoder.c` minimum block size |
| 11 | Reference Python decoder lit_fmt=3 unsupported | Format conformance | 116 | `reference/vv_decoder.py` |
| 12 | Reference JavaScript decoder lit_fmt=3 unsupported | Format conformance | 117 | `reference/vv_decoder.js` |
| 13 | Encoder buffer plaintext residue on free | Hygiene (defense-in-depth) | 118 | `vv_encoder.c` `vv_secure_zero` |

**Per-defect regression coverage**: each defect has either a test in `tests/` or a reproducer in `tests/fuzz/corpus_*/`. The test/fuzz harness must continue to exercise the historical attack — a future regression cannot reach the published codec without breaking a test.

The audit campaign also produced **0 findings** in three follow-up campaigns (Sprints 110, 117, 118). This is empirical evidence that the bug-class search has reached genuine diminishing returns at v2.48.x.

---

## 4. Out-of-Scope Defects

Two categories of defect are intentionally not pursued by the audit:

1. **Encoder-side timing or cache side channels.** Mitigation would require constant-time compression, which is incompatible with the codec's design goals. Callers concerned about side channels should encrypt the codec's output before time-of-check (VaptVupt does — encryption is the next stage in the pipeline).

2. **Format compatibility with future zstd / lz4 versions.** VaptVupt's wire format is independent and will not converge to either competitor's format. Frame-level format detection at the application layer (look for magic bytes) is the responsibility of any application that needs to support multiple codecs.

---

## 5. Verification Reproduction

To reproduce the full audit verification on a fresh checkout, on Ubuntu 22.04+ or equivalent:

```bash
# Static analysis
make clean
cppcheck --enable=warning,performance,portability -I include src/  # → 0 findings
scan-build --status-bugs make                                       # → 0 bugs
make CFLAGS="-Wpedantic -Wshadow -Wcast-qual -Werror"              # → builds clean

# Sanitizer matrix
for s in address undefined integer leak; do
    clang -fsanitize=$s -O1 -g -Iinclude src/*.c -o /tmp/vv-san
    /tmp/vv-san -c -m extreme tests/fixtures/dickens -o /tmp/c.vv
    /tmp/vv-san -d /tmp/c.vv -o /tmp/dec
done

# Test suite
make
for t in test_*; do ./$t; done                                      # → 18/18 pass

# DoS reproducers
make test_dos_hang
./test_dos_hang                                                     # → 12 payloads, <60ms each

# Fuzz (set time budget)
cd tests/fuzz
make fuzz_decompress
./fuzz_decompress -max_total_time=120 -fork=4 corpus_decompress/    # → 0 crashes

# Allocation fault injection
cd tests/fault_injection
make
LD_PRELOAD=./libfault.so ../test_roundtrip                          # sweeps fault N
```

Total runtime: ~5 minutes for the full pass.

---

## 6. Reporting Vulnerabilities

Report security issues via the contact in `SECURITY.md`. The codec is GPL-3.0-or-later; downstream projects building on it inherit the GPL obligations. Anthropic does not maintain a paid bug bounty for this codec.

A vulnerability is defined as: any input that causes the *decoder* (one-shot or streaming) to:

- Crash (segfault, abort, panic)
- Loop indefinitely or for >1 second on inputs <100 KB
- Read or write outside `dst_cap` or the input buffer
- Leak memory
- Fail under any of the runtime sanitizers in §2.2 with valid inputs

Encoder bugs are accepted but considered lower severity — the encoder runs on caller-chosen data.

---

## 7. Status

This document is committed to the repo and is the source of truth for "what has been audited" claims. Updates to the document accompany every patch release that changes the audit posture. The current revision (v1.0, Sprint 121) reflects the codec state at v2.48.1.
