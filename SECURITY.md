# VaptVupt Security Posture

**Document version**: 1.7 (v2.60.3)
**Codebase audited**: v2.60.3
**License**: GPL-3.0-or-later
**Intended deployment**: Embedded codec library inside VaptVupt secure backup tool
**Companion crypto library**: libpqvaptvupt v0.5.1 (post-quantum sealed-box)

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

## 1a. What VaptVupt Does NOT Protect Against (explicit scope boundaries)

Per the project's discipline ("State explicitly what the system does NOT protect against"), the following are **out of scope** for the codec layer:

| Out of scope | Whose responsibility |
|---|---|
| **Confidentiality of compressed data** | Caller (VaptVupt wraps with AES-256-GCM + ML-KEM-768 via libpqvaptvupt) |
| **Authenticity of compressed data** | Caller (VaptVupt provides Encrypt-then-MAC; codec's xxh64 is integrity-only, not authentication) |
| **Side-channel resistance** (timing, cache, power) | Caller (codec is throughput-tuned; constant-time properties apply only to libpqvaptvupt's crypto path) |
| **Compression-oracle attacks** (CRIME, BREAST, BREACH) | Caller (do not mix attacker-controlled and secret plaintext in the same compression stream — same caveat as zlib, zstd, brotli) |
| **Denial of service from `dst_cap` exhaustion** | Caller (the codec enforces `dst_cap` but the caller chooses the value; passing `SIZE_MAX` defeats DoS protection) |
| **Resource exhaustion from extreme-mode *encoding* of attacker-controlled input** | Caller (since v2.52.0, extreme mode uses up to a 16 MB window → ~128 MB matcher and ~1 MB/s optimal parse, so a large input consumes proportional time/memory: measured 169 MB / 126 s on a 51 MB input. The decoder is unaffected. Deployments exposing extreme *encoding* to untrusted input sizes must impose their own size/timeout limits — same caveat as zstd `--ultra --long`. Decode of untrusted input remains bounded by `dst_cap`.) |
| **Multi-process race conditions on shared input/output buffers** | Caller (codec assumes single-writer-during-call semantics) |
| **Disk persistence of working buffers** | Caller (Sprint 118's secure-zero scrubs heap, not swap; mlockall is caller's job) |
| **Resistance to compiler downgrades** | Caller (the security properties below assume `-O2` or `-O3` with a modern gcc/clang; `-O0` builds are functional but not audit-targeted) |

**Specifically**: a `.vv` file alone provides **no** confidentiality and **no** authentication. It is a compressed blob with an integrity checksum that detects accidental corruption, not deliberate tampering. For deliberate-tampering resistance, the caller MUST wrap the codec output in an authenticated encryption scheme (AEAD). This is exactly how VaptVupt uses it (via libpqvaptvupt's `pqvv_seal` / `pqvv_open`).

The codec's job is: **safely decompress untrusted input**. Everything outside that boundary belongs to the layer above.

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

14 distinct security/correctness defects have been found and fixed using 12 distinct audit tools / techniques:

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
| 12 | Manual decode-path audit + ASan repro | v2.60.2 | 1 (phase-1 match-length OOB write — see advisory below) |

**Total: 14 defects fixed, 12 distinct tools/techniques applied.**

### Advisory — v2.60.2: heap-buffer-overflow (OOB write) in AVX2 decode warmup

**Severity:** high (out-of-bounds heap write on attacker-controlled input).
**Affected:** the AVX2 token decode path (`decode_block_tokens_impl`), phase-1
"warmup" loop, in releases that shipped it prior to v2.60.2. Non-AVX2 builds
(general/tail path only) are unaffected.

**Cause:** phase 2 and the general/tail decode paths check
`op + mlen <= op_end` before each match copy; the phase-1 warmup loop validated
the match *offset* but omitted the match *length* output bound. A crafted
`.zupt`/`.vv` stream with a corrupt match-length extension (token `mc == 15`
plus continuation bytes) in the first ~64 KiB of output could drive any of the
four match-copy variants — confirmed under AddressSanitizer in `match_overlap`
(offset < 8) — to write past the output buffer. The `op < op_safe` loop guard
only reserves a fixed 72-byte margin and does not bound an extended match
length.

**Fix:** add the same `(size_t)(op_end - op) < mlen → VV_ERR_OVERFLOW` check the
other paths already carry, to the phase-1 warmup loop. On a valid stream
`op + mlen` never exceeds `op_end`, so the branch is never taken and decode
output is **byte-identical** (ratio gate ± 0; differential fuzzer 5200/5200
unchanged); it only rejects corrupt input.

**Detection:** manual audit of the decode bounds-check structure, reproduced
with a focused ASan harness. The fuzzers that found the analogous phase-2 gap
(Sprint 109) had not exercised this specific phase-1 path.

**Regression:** `tests/test_phase1_overflow.c` (TEST21, in `make test`) crafts
the overrun for all four offset classes plus the 3-byte-offset path and asserts
a clean rejection; verified under ASan+UBSan (9/9).

10 of the first 11 tools surfaced ≥1 defect on first application; tools #7, #9, #10, #11 produced no findings. The v2.60.2 defect shows that targeted manual audit of invariant *symmetry* across fast/slow paths still finds what coverage-guided fuzzing can miss.

### Companion audit — v2.60.3: SEQ entropy decoder safe-zone (no defect)

After v2.60.2, the same bounds-check-symmetry methodology was applied to the
*other* decoder — `vva_decode_sequences_impl`, the entropy/sequence path for
`'S'`/`'T'` blocks, which has its own safe-zone fast path that elides both the
offset and the match-length output-bound checks once
`op >= dst_base + SAFEZONE_MAX_OFFSET (1<<24)` and `op <= op_end - 65535`.

**Result: no defect.** Unlike the raw-token path (whose match length grows via
an *unbounded* `0xFF`-continuation varint — the v2.60.2 root cause), every
SEQ-path length is hard-bounded by a *fixed* ANS code table with a fixed
extra-bit count:

| quantity | bound | source |
|---|---|---|
| litlen | ≤ 65535 | `ll_base[35]=61440` + max 4095 (12 extra bits) |
| matchlen | ≤ 65535 | `ml_base[35]=32768` + max 32767 (15 extra bits); `ml_base_v2[35]` → 65534 |
| offset | ≤ 2²⁴−1 | code ≤ 26 → `[2²³,2²⁴)`, plus the **unconditional** `offset > SAFEZONE_MAX_OFFSET` reject (line 2507) |

Because `op_safe_end = op_end - 65535` and `offset_check_floor = dst_base + 2²⁴`
match these maxima exactly, the checks skipped in the safe zone are genuine
tautologies for *all* input, including adversarial — there is no analogue of the
v2.60.2 overflow. The structural difference is the absence of an unbounded
length-extension mechanism.

**Coverage added:** the safe-zone fast path engages only past 16 MB of frame
output (`dst_base` is per-frame, `op` cumulative across blocks). Every prior
adversarial test used ≤ 2 MB buffers, so the bounds-elision branch had **zero
coverage**; an instrumented run confirmed it executes 617,812 times on a 24 MB
frame and decodes correctly. `tests/test_safezone_adversarial.c` now includes a
20 MB single-frame roundtrip (`test_safezone_fastpath_engaged`) that exercises
the fast path, ASan+UBSan-clean (58/58). Stale comments in that file (the floor
is 1<<24 = 16 MB since Sprint 46, not the 1 MB they stated) were corrected.

---

## 4. Permanent Audit Infrastructure

The following tools and harnesses are permanently committed to the source tree for ongoing regression coverage:

### Fuzz harnesses (`tests/fuzz/`)
- **`fuzz_decompress.c`** — stateless decoder, libFuzzer + ASan + UBSan
- **`fuzz_dstream.c`** — streaming decoder with randomized chunk boundaries
- **`fuzz_roundtrip.c`** — encoder + decoder roundtrip property
- **`fuzz_differential.c`** — stateless vs streaming decoder must agree

### Allocation fault injection
- **`tests/oom_inject.c`** — LD_PRELOAD allocator interposer that fails the
  Nth malloc/calloc/realloc.
- **`tests/oom_sweep.sh`** — sweeps every allocation site in `vv_compress`
  (including the BCJ copy) and `vv_decompress`, asserting no crash; runs in
  `make test`. Under ASan/UBSan it also proves no leak or use-after-free on
  any allocation-failure path. The full sweep (144 allocation points) is
  clean: no crash, no leak, no use-after-free on any single failure.

### Formal verification (`verification/`, `make verify`)
The BCJ branch filters in `src/vv_bcj.c` and selected decoder helpers run on
the decode path (processing attacker-controlled bytes). They are
machine-checked with CBMC over fully nondeterministic inputs up to a bounded
size:
- **`vv_bcj_x86`**, **`vv_bcj_arm64`** — proven memory-safe (no OOB / invalid
  pointer), free of signed overflow and invalid conversions, and **lossless**
  (`inverse(forward(x)) == x`).
- **`vv_bcj_detect`** — proven memory-safe on arbitrary and truncated input,
  including the PE-header offset that is read from the input itself.
- **`read_ext_len`** — the decoder's variable-length integer reader, proven
  never to read at or past the input end, and to advance its pointer within
  `[base, end]`, for any compressed-input contents and any start offset. An
  over-read here would be a heap-buffer-overflow on untrusted input. The
  harness copies the function verbatim from `src/vv_decoder.c` and the verify
  driver fails on drift, so the proof binds to the shipped code.
- **block-header pack/unpack** (`vv_bh_pack` / `vv_bh_type` / `vv_bh_last` /
  `vv_bh_size`) — proven a lossless round trip over the full valid field
  domain, with every accessor in range for any 32-bit header, including
  corrupt input.

Proofs use `--unwinding-assertions` (the unwind bounds are themselves
verified). The filters use intentional modular unsigned arithmetic — defined
behaviour in C — so `--unsigned-overflow-check` is not enabled; every other
standard CBMC safety check is. This complements the runtime fuzzing in
`tests/test_bcj.c` and the differential fuzzer with an exhaustive guarantee
over all inputs up to the bound. See `verification/README.md`.

A second, independent tier uses Frama-C's Eva plugin (abstract interpretation
with RTE), which reasons about value ranges rather than enumerating inputs and
runs with the `frama-c-base` package. Eva confirms `read_ext_len` and the
block-header accessors raise **0 alarms** (no invalid pointer access, no
out-of-bounds, no undefined behaviour) for any input; the BCJ filters raise
only 3 residual pointer-comparison / pointer-difference obligations that hold
within a single object and are discharged by the CBMC `--pointer-check`
proofs. `verification/acsl_read_ext_len.c` additionally carries an ACSL
contract and loop invariant for an unbounded deductive proof of `read_ext_len`
with the full Frama-C WP plugin.

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

**This is defense in depth**, not a primary security boundary. The original input buffer (caller-owned) is unaffected; if the caller doesn't zero it themselves, the codec's hygiene doesn't help. But for VaptVupt's pipeline (compress → encrypt → write), the codec's working buffers are now scrubbed before the encryption step gets the data.

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

## 8a. SPEED PROGRAM Security Review (Sprints 25-32, v2.50.0-v2.50.6)

Between v2.48.0 (last audited in document version 1.3) and v2.50.8 (audited in this document version 1.4), 7 SPEED PROGRAM sprints made performance-motivated changes to the codec. Each is reviewed below for security implications.

| Sprint | Change | Security review |
|---|---|---|
| 26 (v2.50.0) | Default CFLAGS: `-O2` → `-O3 -flto` | **Net-positive**. `-flto` enables cross-TU dead-code elimination and call-graph optimization. Sanitizer compatibility verified (`fuzz_decompress` + ASan + UBSan re-run, 0 findings). PGO build mode (`make pgo`) preserves identical wire format and identical bounds checks. |
| 27 (v2.50.1) | ANS hot-loop OOB-fold: 3 per-iteration validators (`ll_code`, `of_code`, `ml_code`) merged into 1 bitwise-OR'd branch | **Equivalent**. The fold preserves the original invariant `ll_code < VVA_LL_CODES && of_code < VVA_OF_CODES && ml_code < VVA_ML_CODES`. The bitwise-OR collapses three short-circuit branches into one; if any code is OOB, the OR is non-zero and the function returns `VVA_ERR_CORRUPT` at the same boundary as before. Same defense as Sprint 109's original OOB fix. |
| 28 (v2.50.2) | Added `make pgo` target (instrument → train → rebuild) | **Equivalent**. PGO is an optimization-only build mode; output byte-identical to non-PGO. The instrumentation run uses Silesia training inputs (not attacker input). Documented as opt-in. |
| 29 (v2.50.3) | `matcher_insert_fast` — new static helper skipping a boundary check and `hash_safe` dispatch | **Caller proves preconditions**. The fast variant is only invoked from `compress_block`'s bulk-insert loops where the caller's `j <= end - 5` guard already proves the boundary. Inspection: every call site is preceded by the proving guard. Encoder-only change (no decoder impact, untrusted input is not in scope for the encoder per Section 1). |
| 30 (v2.50.4) | Unconditional prefetch hints in `chain_match_ex` | **Benign**. `__builtin_prefetch` is a hint; the only side effect is touching cachelines. No security implication. |
| 31 (v2.50.5) | **NEGATIVE**: runtime AVX2 dispatch via `__attribute__((target("avx2")))` was attempted, reverted after -7% benchmark regression | **No code shipped**. The branch was reverted; v2.50.5 is byte-identical to v2.50.4. The lesson — that fine-grained `__attribute__((target))` dispatch can leak side-channels through dispatch overhead variation — informs the right architectural path for future SIMD work: API-boundary dispatch with cached CPUID (the pattern libpqvaptvupt's AES-NI code uses successfully). |
| 32 (v2.50.6) | SPEED PROGRAM close — honest full-Silesia measurement | **Documentation only**. Zero code change. |

**Aggregate verdict**: the SPEED PROGRAM made the codec faster on binary/scientific decode (+85% on sao, +105% on x-ray) without altering its security properties. All audit infrastructure from Section 4 continues to apply to v2.50.8. Permanent regression reproducers from Section 4 were re-run on v2.50.8 and all still complete in <60ms.

### Re-validation evidence

The audit campaign tools were re-run against v2.50.8 between Sprint 32 and Sprint 38:

| Tool | v2.48.0 result | v2.50.8 result |
|---|---|---|
| cppcheck | 0 findings | 0 findings |
| scan-build (clang static) | 0 bugs | 0 bugs |
| GCC strict warnings | 0 hits | 0 hits |
| `clang -fsanitize=integer` | 0 errors | 0 errors |
| `fuzz_decompress` (libFuzzer + ASan + UBSan) | clean | clean (>200K cumulative iterations) |
| `fuzz_dstream` | clean | clean |
| `fuzz_roundtrip` | clean | clean |
| `fuzz_differential` | clean | clean |
| 12 DoS reproducers (Section 4) | <60ms each | <60ms each |

No security regressions from SPEED PROGRAM changes.

---

## 8b. Continuous Audit Infrastructure (Sprint 38)

v2.50.8 adds `.github/workflows/ci.yml` to gate every push and pull request. This raises Sprint 31's lesson — that local discipline alone is insufficient — to remote enforcement.

Four CI jobs run on every commit:

1. **`linux`**: distro × CC matrix (gcc + clang × Ubuntu 24.04 / Arch / Fedora) running the full test suite under `-Werror`. Catches warning-class regressions.
2. **`sanitizers`**: clang with `-fsanitize=address,undefined` running `make test` under `halt_on_error=1`. Catches use-after-free, buffer overflows, alignment violations, signed overflow.
3. **`wire-format`**: pins 12 compressed sizes (4 Silesia fixtures × 3 modes). Catches encoder changes that drift the wire format without an explicit version bump.
4. **`fuzz-smoke`**: 30s × 3 libFuzzer harnesses on every push. Catches obvious regressions in coverage from prior sprints' >145K fuzz iterations.

A nightly long-run fuzz workflow is shipped as of Sprint 40 (`.github/workflows/nightly-fuzz.yml`, see Section 9 item 4) — the per-commit CI is intentionally fast (under 10 minutes total) to provide PR feedback without becoming a merge bottleneck, while the nightly campaign provides depth.

The full 13-tool audit campaign from Section 3 is NOT run on every commit (some tools are too slow); CI catches the common-case regressions and a manual re-run of the full audit is required before any release tag.

---

## 8c. Companion Crypto Library: libpqvaptvupt

In VaptVupt deployment, VaptVupt is wrapped by libpqvaptvupt's `pqvv_seal` / `pqvv_open` to provide the confidentiality and authentication that the codec itself does not provide (Section 1a). The crypto library's security posture is documented in its own repository, but the relevant integration boundary is:

```
Plaintext
  ↓ vv_compress (VaptVupt — this library)
.vv compressed blob
  ↓ pqvv_seal (libpqvaptvupt — companion library)
sealed ciphertext: MAGIC || KEM_ct || X25519_pk || nonce || HMAC || AES-256-CTR(.vv)
  ↓ transport / storage
sealed ciphertext
  ↓ pqvv_open (libpqvaptvupt — MAC verify, then AES decrypt)
.vv compressed blob
  ↓ vv_decompress (VaptVupt — this library)
Plaintext
```

Properties of the full pipeline (NOT this codec alone):
- **Confidentiality**: ML-KEM-768 + X25519 hybrid (breaking either alone does not break confidentiality)
- **Authenticity**: HMAC-SHA-256 in Encrypt-then-MAC configuration; verified before any decryption
- **PQ-safe**: ML-KEM-768 provides post-quantum confidentiality; HMAC-SHA-256 is symmetric and PQ-safe by construction
- **Per-message overhead**: 1184 bytes (constant)

The codec's threat model (Section 1) starts AFTER `pqvv_open` has produced a verified `.vv` blob. The codec must safely decompress that verified blob.

For deployments NOT using libpqvaptvupt (i.e., consuming `.vv` files directly from untrusted sources without an authentication layer), the codec is still safe per Section 1, but the caller must understand that the integrity checksum (xxh64) is NOT cryptographic authentication — a sophisticated attacker can craft a `.vv` blob with a valid xxh64 that the codec will safely decompress to attacker-chosen plaintext. This is correct codec behavior; preventing the attack is the caller's job (use AEAD).

---

## 8d. RATIO PROGRAM Security Review (Sprints 42-50, v2.51.0-v2.52.0)

The RATIO PROGRAM made the first codec changes since v2.50.x: a whole-block
optimal parser (v2.51.0), literal-price calibration (v2.51.1), and
large-window extreme mode (v2.52.0). Two of these touch security-relevant
boundaries and are audited here.

### Change 1: Whole-block optimal parser (v2.51.0, encoder-only)

The extreme-mode encoder gained a dynamic-programming optimal parser
(`compress_block_optimal`). This is **encoder-side only** — it changes
which matches are selected, not the wire format or the decoder. Per
Section 1, the encoder consumes trusted input, so parser bugs are
correctness issues, with the roundtrip-violation exception.

- **DoS vector considered and bounded**: the DP is O(N × chain_depth ×
  extend) and degrades toward quadratic on adversarial self-similar data.
  Mitigated by the long-match short-circuit (any match ≥ 512 is taken as a
  single edge, skipping interior DP). `test_safezone_adversarial` (55/55)
  and `test_dos_hang` (12/12 in < 5 s) both pass with the parser active.
- **Roundtrip property**: validated byte-perfect on all 12 Silesia
  fixtures plus 25K+ libFuzzer roundtrip and 25K+ differential cases at
  ship time (v2.51.0), re-validated each subsequent release.

### Change 2: Large-window extreme mode + decoder offset-cap (v2.52.0)

This is the one change that touched a **decoder DoS-guard boundary** and
therefore receives the closest scrutiny.

The encoder's extreme-mode window was raised from ~1 MB to up to 2^24 =
16 MB (scaled by input size). For this to round-trip, the ANS sequence
decoder's `SAFEZONE_MAX_OFFSET` was raised from 2^20 (1 MB) to 2^24
(16 MB) — it had previously rejected any offset > 1 MB as corrupt.

**Audit findings (all verified by test/measurement, not assertion):**

1. **The DoS guard remains valid.** `SAFEZONE_MAX_OFFSET = 2^24` is the
   exact maximum offset representable in the 3-byte wire field, so an
   offset > 2^24 is genuinely unrepresentable and is still rejected
   (`offset > SAFEZONE_MAX_OFFSET ⇒ VVA_ERR_CORRUPT`). The cap did not
   weaken the guard; it aligned the guard with the true wire ceiling.
   `test_safezone_adversarial` 55/55 and `test_dos_hang` 12/12 pass with
   the 2^24 cap.

2. **Decode memory is UNCHANGED and caller-bounded.** The decoder
   allocates working buffers from the block's `lit_count` (bounded by the
   caller's `dst_cap`), NOT from the frame header's `window_log`. A
   malicious frame advertising `window_log = 24` does **not** force a
   16 MB decode-side allocation — `window_log` only selects the offset
   field width (2 vs 3 bytes). Measured decode peak RSS on a 51 MB
   fixture (mozilla): **68 MB**, sub-second. The large-window change is
   entirely encode-side for resources; the untrusted-input decode surface
   gained no new memory or time cost.

3. **Encode-time resource profile (trusted-input surface).** The 16 MB
   window allocates a ~128 MB matcher (`chain[16M] + hash4_chain[16M]` =
   2 × 4 × 16 M) and the optimal parse runs at ~1 MB/s, so extreme
   encoding a 51 MB input measured **169 MB peak RSS, 126 s wall**. Per
   Section 1 the encoder operates on trusted input, so this is a
   documented performance characteristic, not a vulnerability. **Caveat
   added to Section 1a**: any deployment that exposes extreme-mode
   *encoding* to attacker-controlled input sizes must impose its own
   input-size and timeout limits — a multi-GB input would consume
   proportional time and the fixed 128 MB matcher. This is the same
   class of caveat as zstd `--ultra --long` and is the caller's
   responsibility.

4. **Decoder-forward-compatibility break (correctness, not a
   vulnerability).** v2.52.0 decoders read all older files; pre-v2.52.0
   decoders cleanly REJECT (error -2) v2.52.0 extreme files that use
   offsets > 1 MB — they do not crash or misbehave. Clean rejection of an
   unsupported stream is the correct and safe behavior per Section 1.

5. **Stale-comment defect fixed (Sprint 51).** The inline-token decoder's
   `max_valid_off` comment still referenced the old 2^20 (wlog=20) bound;
   the code was correct (`0xFFFFFF` for 3-byte offsets) but the comment
   was updated to match the 2^24 reality. Documentation-only; the
   compiled binary is byte-identical (md5 93a73ccd…).

### Change 3: Literal-price calibration (v2.51.1, encoder-only)

A single constant (`opt_lit_price` 6→8). Encoder-only, no security
surface. Noted for completeness.

### Net security posture after the RATIO PROGRAM

The untrusted-input decode surface — the entire scope of Section 1 — is
**unchanged in memory and time cost** by the RATIO PROGRAM and retains all
DoS guards (12/12 reproducers, 55/55 adversarial, 40K+ fuzz per release).
The only new resource consideration is encode-time (trusted input), now
documented in Section 1a. No new decoder attack surface was introduced;
the one decoder change (offset cap) aligned a guard with the wire ceiling
without weakening it.

---

## 9. What's NOT Tested (residual risk as of v2.52.0)

In the interest of accuracy, the following remain not yet covered:

1. **Cross-decoder differential vs zstd**. Different formats, so this isn't possible directly. **Status: deferred (architectural)**.

2. **Multi-threaded decoder fuzz**. `vv_decompress_mt` has not been libFuzzer'd, only TSan-tested. **Status: open**. Multi-threaded fuzz is a planned roadmap item.

3. **Encoder fuzz on deeply pathological inputs** (e.g., adversarial Huffman trees that produce maximum-depth codes). Roundtrip fuzzer covers random plaintext but not crafted adversarial inputs. **Status: open**. Out of scope per Section 1 (encoder consumes trusted input), but useful for defense-in-depth.

4. **Long-running (>1 hour) fuzz campaigns**. Current cumulative fuzz: ~200K+ runs total across all harnesses. Sprint 38's CI runs 30s × 3 harnesses on every commit. **Sprint 40 adds `.github/workflows/nightly-fuzz.yml`**: a nightly long-run campaign of 3 harnesses × 2 sanitizers (address, undefined) at 120 min/harness default, with persistent coverage-minimized corpus caching across runs, plus a 100K-iteration differential fuzz and a DoS-reproducer regression check. **Status: addressed** (nightly workflow shipped; the >1h cumulative-per-night campaign is now automated).

5. **Formal verification of `matcher_insert_fast`'s precondition** (Sprint 29). The fast variant assumes the caller's `j <= end - 5` guard proves the boundary. This is verified by inspection (every call site is preceded by the guard) but not by static analysis. **Status: open**. Frama-C/ACSL annotation would close this; multi-sprint.

6. **Side-channel resistance**. The codec is throughput-tuned, not constant-time. This is correct per Section 1a (out of scope) — the codec operates on already-decrypted data inside VaptVupt's pipeline, so secret-dependent timing in the codec leaks no information about VaptVupt's keys. But callers using VaptVupt to compress secrets directly should be aware. **Status: out of scope by design**.

7. **Reproducibility of `make pgo` across compilers**. PGO output is byte-identical to the default build for the same compiler and training set; behavior across compilers (gcc vs clang vs MSVC) has not been audited. **Status: open**, low-priority (PGO is opt-in).

8. **Resistance to compiler-induced timing variation** under PGO. PGO can re-order branches in ways that vary by training data. Not exploitable in the threat model (Section 1a out-of-scope), but documented for completeness. **Status: out of scope by design**.

9. **Large-window extreme-mode encoder fuzz (Sprint 51).** The v2.52.0 large-window optimal-parse path (16 MB window, cross-block matching) was validated by byte-perfect roundtrip on all 12 Silesia fixtures and 40K+ roundtrip/differential fuzz cases, but the libFuzzer roundtrip harness uses `-max_len=65536` (single-block inputs), so the multi-block cross-block-match path is exercised by the Silesia corpus and the 1.05 MB Sprint 46 reproducer rather than by libFuzzer. **Status: partially addressed** — multi-block roundtrip is corpus-validated; a multi-block (>1 MB) libFuzzer harness would close the gap and is a roadmap item. Encoder fuzz is defense-in-depth (encoder consumes trusted input per Section 1).

Items 2-4 and 9 are the highest-priority for future audit work. Item 5 is the highest-value formal-verification target if Jasmin/Frama-C effort becomes available.

These represent residual risk to be addressed in future audit sprints if VaptVupt deployment surfaces relevant concerns.

---

## 10. Reporting Vulnerabilities

This is currently a pre-VaptVupt-integration codebase. For all security disclosures:

**Email**: `sac@securityops.co`

**Subject prefix**: `[VaptVupt SEC]` (codec) or `[libpqvaptvupt SEC]` (companion crypto library)

**Include**:
- Affected version (e.g., `v2.50.8`)
- Reproducer file (the `.vv` blob or input that triggers the issue)
- Observed behavior (crash, hang, OOB read, etc.)
- Build configuration (`make` flags, sanitizers, compiler version)
- Suggested severity

**Response SLA**: best-effort, typically within 7 days. Acknowledgement of receipt within 48 hours.

**PGP**: a project-specific PGP key for encrypted disclosure will be published before VaptVupt v2.2.3 ships. Until then, sensitive details can be exchanged out-of-band after initial contact.

**Coordinated disclosure**: preferred. Public CVE assignment will follow standard 90-day embargo unless the reporter and maintainer agree otherwise.

When VaptVupt is integrated into VaptVupt and reaches public release, vulnerability disclosure should also follow VaptVupt's policy at securityops.co (TBD URL).

---

## 11. License Note

VaptVupt is GPL-3.0-or-later. Audit infrastructure (libFuzzer harnesses, regression reproducers, fault-injection scripts) is included in the source tree under the same license, allowing downstream users to run the same audit campaign on modifications.
