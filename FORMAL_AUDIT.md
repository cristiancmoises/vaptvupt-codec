# VaptVupt Formal Audit Document

**Document version**: 1.5
**Codebase audit scope**: v2.65.11 release delta (regression/dynamic validation)
**Formal evidence baseline**: inherited historical results, with original
versions and bounds recorded below; no full formal-tool rerun for v2.65.11
**License**: GPL-3.0-or-later
**Intended deployment**: Embedded codec library inside the VaptVupt secure backup tool, plus general-purpose use as a zstd/lz4 alternative

This document is the formal audit reference for VaptVupt. It specifies what has been verified, by which mechanism, against what threat model, with what limits. It is intended to satisfy the due-diligence requirements of:

- Downstream library consumers integrating VaptVupt as a compression dependency
- Security-conscious deployments verifying the codec under hardened-build sanitizer regimes
- Internal review processes at downstream teams
- Independent third-party security auditors performing pre-integration review

It is intentionally specific about what is **not** verified, to support honest risk assessment.

## v2.65.11 delta-validation scope

Sparse small-input matcher initialization is checked against full-matcher
output and with MemorySanitizer, including exact-sized buffers. Fast-mode
format selection has roundtrip and reset regressions. Caller-owned entropy
workspace checks cover alignment, capacity, reuse, malformed data and
allocation behavior. These are dynamic tests, not formal proofs.

The scalar gate builds the core with `VV_DISABLE_SIMD=1`, freestanding compiler
semantics and general-purpose registers only, then links userspace tests.
Its stack-usage files are diagnostics: passing this gate does not certify a
kernel stack budget, a kernel build, all architectures, or allocation-free
framed decoding. Linux integration also requires compatible licensing and
human review of the contribution. No kernel submission is certified here.

The existing formal harnesses are unchanged in v2.65.11. The v2.65.10 reader
exception below still applies; CBMC/Frama-C results are not inferred from
compiler or sanitizer success.

> **Historical currency note (v2.65.10):** the token length reader changed in that
> release and its formal harness copies have been updated. Historical
> `read_ext_len` proof results therefore do not certify the current helper
> until the tools are rerun. Current evidence for this helper and the other
> changed codec paths consists of regression and dynamic validation.

## v2.65.10 delta-validation scope (historical)

Targeted regressions cover Huffman encoder capacity exhaustion and undefined
shift prevention; truncated single/four-stream Huffman codewords; ANS
normalized-frequency validation before all literal table builds; mandatory
token extension terminators; advertised-window offset bounds; and cumulative
streaming output after completion. Valid controls accompany the malformed
streams, with one-shot, fragmented streaming and reference-decoder coverage.

Encoder regression tests exercise repeated v1/v2 stream resets against fresh
contexts, including the long-match corruption trigger. Paired timing runs
verify the compressed size/hash while assessing optional hash4 allocation;
these performance measurements are not formal proofs. CLI regressions cover
invalid options and failed buffered output. The build propagates required
Python failures and instruments the SIMD fuzz object.

The updated CBMC/Eva/ACSL helper copies require new tool runs. Historical BCJ
and block-header results remain evidence for those unchanged functions under
their stated bounds, not a proof of the whole current codec. No new formal
claim is made for the streaming orchestration, entropy table builders or
encoder allocation paths.

> **Historical currency note (v2.65.9):** historical sprint references below record the
> original evidence, bounds, and tool availability. This release did **not**
> perform or claim a fresh full CBMC/Frama-C or audit-campaign rerun. Its delta
> is covered by regression and dynamic validation: direct-vs-historical tANS
> decode-table equivalence, established decode/roundtrip suites, streaming BCJ
> whole/split roundtrips for x86 and AArch64 with checksum on/off, and API
> contract cases for invalid modes and conflicting filters. The 21-case SEQ
> suite adds direct and end-to-end coverage for oversize nonterminal literal
> fallback; reference parity and OOM-baseline checks are also dynamic. Formal
> results are
> inherited only for the unchanged functions and properties named below.
> Re-run the commands on the target toolchain before treating historical
> results as current certification.

## v2.65.9 delta-validation scope (historical)

The direct sequence-table builder removes the 4 KiB spread scratch and is
checked entry-for-entry against the prior builder over 256 deterministic valid
normalizations, then exercised through ANS, SEQ, roundtrip, safe-zone, and
exact-buffer decoding. The streaming completion change is exercised across
both BCJ architectures, whole and split frames, and both checksum settings;
the inverse occurs once, only after checksum validation or the final
checksumless block. API-contract regressions require `VV_ERR_PARAM` for enum
values outside the three public modes and for simultaneous x86/ARM64 filters;
decoder regressions reject input headers that set both architecture bits in
one-shot, streaming, and frame-info paths.

`test_seq_v2` 21/21 also covers the global-`match_count` invariant: a literal
run over 65,535 bytes before a later match is not representable as a zero-match
midstream LL entry, so the encoder rejects that SEQ candidate and falls back
losslessly. The OOM sweep verifies its randomized baseline roundtrip before
injection. Both reference decoders now consume trailing LL-only entries with
the C-equivalent iteration bound, reject dual-BCJ headers, and apply exact
x86/AArch64 inverses after checksum validation; current-output fixtures cover
checksum on and off. C remains canonical for legacy H/A/I/C; Python retains
limited A-tag support and JavaScript omits the legacy tags.

These are regression/dynamic claims. The streaming state-machine orchestration
and the new direct table builder are not covered by a new bounded or deductive
proof in this revision. The existing CBMC/Eva baseline still applies to its
then-unchanged BCJ filter functions, detector, `read_ext_len`, and block-header
helpers under the bounds stated in Section 2. The v2.65.10 exception for the
changed token length reader is described above.

---

## 1. Threat Model

The codec is part of a layered system. Threats are categorized by which layer is responsible for mitigating them.

### 1.1 In-scope threats — codec must mitigate

| Threat | Mitigation |
|---|---|
| **T1: Decompression bomb** | Caller-provided `dst_cap` is enforced. No internal expansion. Decoder rejects any sequence that would exceed `dst_cap` with `VV_ERR_OVERFLOW` (`VVA_ERR_OVERFLOW` in the ANS layer). |
| **T2: Crafted match-count overflow** | Sprint 90 fix bounds `match_count` against `dst_cap / min_match` before allocation. |
| **T3: Infinite loop on degenerate ANS state** | Sprint 89 fix adds bounded iteration counter (`max_iters = total_lits + match_count + 16`). Returns `VVA_ERR_CORRUPT` instead of hanging. |
| **T4: Huge literal-run extension lengths** | Sprint 109 bounds-checks LL extension before memcpy. |
| **T5: Out-of-bounds symbol codes** | Sprint 109 bounds-checks `ll_code`, `of_code`, `ml_code` against their respective tables (`VVA_LL_CODES=36`, `VVA_OF_CODES=27`, `VVA_ML_CODES=36`). |
| **T6: NULL deref on edge-case sequences** | Sprint 109 always allocates all 3 ANS decode tables, regardless of empty-symbol-count edge cases. |
| **T7: Crafted 4-stream Huffman attacks** | Sprint 105 hardens `vvh_decode4` against 6 distinct DoS patterns: invalid stream-length headers, mismatched stream lengths, OOB final-bit positions, malformed lit_fmt=4 selection, and two integer-overflow paths in stream-header parsing. |
| **T8: Allocation failure mid-decode** | The historical Sprint 100 campaign recorded 250+ randomized allocation-fault trials with no crashes / leaks / UB; the current deterministic OOM harness separately fails each allocation reached by its baseline fixture. |
| **T9: Plaintext recovery via heap residue** | `vv_secure_zero` is called on tracked plaintext-bearing encoder arenas (literals, LZ-tokenized output, raw input window, scratch) and, since v2.65.9, the private one-shot BCJ input copy before `free()` — defense in depth, not a confidentiality boundary. |
| **T10: Hardened-build false positives** | Sprint 117 `VV_NO_SANITIZE_INTEGER` annotations on 12 functions performing intentional unsigned modular arithmetic; `__builtin_rotateleft64` for the rotate. Result: 0 strict-integer warnings. |
| **T11: Encoder mid-stream abort with state leak** | Streaming encoder's `vv_cstream_destroy` explicitly scrubs its tracked context and sub-buffers. `tests/test_secure_zero.c` exercises destroy/roundtrip completion under sanitizers; it is not a direct post-free content proof. |
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
| Supply-chain attacks against the release-publishing pipeline | Out of scope for the codec. Verify amalgamation hashes against the published GitHub repo. |

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

The runtime regression suite is integrated into the normal build and CI cycle.
The static-analysis and formal mechanisms remain reproducible from the source
tree but are manual/tool-dependent unless a particular CI job invokes them;
their historical results must not be read as evidence that every tool ran on
every commit.

### 2.1 Static analysis

| Tool | Invocation | Result on v2.48.1 |
|---|---|---|
| cppcheck (warning + performance + portability) | `cppcheck --enable=warning,performance,portability -I include src/` | 0 findings |
| clang scan-build | `scan-build --status-bugs make` | 0 bugs |
| GCC strict warnings | `make CFLAGS="-Wpedantic -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wunreachable-code -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wundef -Wuninitialized"` | 0 hits |

The table records the original audit-campaign results beginning at v2.46.0.
It is historical evidence, not a claim that all three tools were rerun for
v2.65.9.

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

The harnesses are built with `-fsanitize=address,undefined`. The current smoke
target seeds temporary corpora under `build_obj/`; durable defect reproducers
are checked in under `tests/regression_inputs/`.

### 2.4 DoS reproducer suite

Twelve saved adversarial payloads, each a known historical attack:

- 6 from v2.46.x (lit_fmt=3 stream-header, ANS state divergence, OOB symbol codes, NULL deref on empty-table edge cases, match-count overflow, infinite-loop ANS)
- 3 from v2.47.x lit_fmt=4 (4-stream offset overflow, stream-length-mismatch, malformed selection)
- 3 from Sprint 109 (LL extension overflow, OOB ll_code, deg-degenerate Huffman)

`make test_dos_hang` exercises all twelve. **All complete in <60ms each** on the reference hardware (commodity x86_64). Without the Sprint 89-109 fixes, these payloads would hang or crash.

### 2.5 Allocation-fault injection

Two complementary dynamic harnesses are recorded:

- The historical Sprint 100 `tests/fault_injection/malloc_fault.c` interposer
  uses `VV_FAULT_RATE` and `VV_FAULT_SEED` to fail allocations randomly. The
  audit campaign recorded 250+ trials across encode, decode, extreme,
  streaming, multi-threaded, and sanitized paths with no crash, leak, or UB.
- The current `tests/oom_sweep.sh` builds `tests/oom_inject.c`, counts the
  allocations reached by its baseline `vv_compress --bcj` and `vv_decompress`
  fixtures, and then fails each reached allocation index in turn. In v2.65.9
  it first requires that randomized baseline to roundtrip byte-exactly, keeping
  codec correctness failures distinct from injector failures.

These are strong dynamic results for the exercised fixtures and allocation
points, not an exhaustive proof of every potentially reachable site.

### 2.6 API contract checks

`tests/test_api_contract.c` (35 checks in v2.65.9) exercises the documented
public API contract:

- NULL-pointer handling on every public function
- `dst_cap = 0` boundary
- `src_len = 0` boundary
- `vv_compress_bound` correctness (must always return ≥ actual output for any valid input)
- `vv_dstream_create` failure paths
- Caller-owned vs codec-owned memory boundaries
- Streaming reset after error
- Rejection of invalid mode enum representations and simultaneous x86/ARM64
  encoder filters
- Rejection of invalid modes and unsupported BCJ options by streaming-encoder
  create/reset entry points
- Rejection of `window_log` below 10 or above 24 before BCJ allocation/work
- Rejection of dual-BCJ input flags by one-shot decode, streaming decode, and
  frame-info parsing

All 35 are release-gating checks. These are **contracts**, not implementation
tests — if the implementation changes, the contract MUST still hold.

### 2.7 Format conformance

Two reference decoders independent of the production C decoder:

- `reference/vv_decoder.py` — byte-exact pure Python, easy to inspect
- `reference/vv_decoder.js` — byte-exact JavaScript, validates the wire format spec

Both reference decoders reproduce current/default encoder output, including
S/T sequence entropy, HUFFMAN4 literals, and x86/AArch64 BCJ frames. Release
fixtures cover BCJ with checksum on and off. C is canonical for legacy H/A/I/C;
Python retains limited A-tag support and JavaScript omits the legacy tags. The
`fuzz_differential` harness compares C output against the Python reference; any
divergence in their shared scope is a failed run.

The wire format itself is documented in `FORMAT.md` with byte-level diagrams
of every block type, header, and ANS table format. The base path has been
**frozen since v1.0.0**. The `'T'`/`format_v2` path was introduced behind an
explicit flag and has had a frozen representation since v2.33.0; since v2.61.0
the encoder also selects it adaptively for binary balanced/extreme input.

### 2.8 Memory hygiene tests

`tests/test_secure_zero.c` reports 4 checks in v2.65.9:

1. Streaming destroy completes cleanly under sanitizers (no double-free, no use-after-free)
2. The streaming output roundtrips byte-exactly
3. 100 allocation/destroy cycles complete cleanly
4. The one-shot BCJ private-copy cleanup path completes and roundtrips

These checks exercise the cleanup paths and observable codec behavior under
sanitizers. They do not inspect freed storage and are not proof of post-free
memory contents. The scrub claim itself is scoped to the explicit
`vv_secure_zero` calls on tracked buffers, including the v2.65.9 BCJ copy.

### 2.9 Amalgamation drift detection

`make amalg-verify` (Sprint 115) checks that `build/vaptvupt.c` and `build/vaptvupt.h` are byte-identical to a re-amalgamation from `src/`. This catches:

- Stale amalgamation in the published artifact
- Any divergence between distributed single-file and the modular source

The release gate runs `make amalg-verify`; a tag must not be published unless
that command confirms the generated and modular sources are in sync.

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

**Per-defect regression coverage**: each defect has either a test in `tests/`
or a checked-in reproducer (including `tests/regression_inputs/`). Most tests
directly detect the historical failure mode. The hygiene test is narrower: it
exercises cleanup paths and roundtrips under sanitizers but does not inspect
post-`free()` memory, so the presence of each explicit `vv_secure_zero` call
also requires source review/static inspection.

The audit campaign also produced **0 findings** in three follow-up campaigns (Sprints 110, 117, 118). This is empirical evidence that the bug-class search has reached genuine diminishing returns at v2.48.x.

---

## 4. Out-of-Scope Defects

Two categories of defect are intentionally not pursued by the audit:

1. **Encoder-side timing or cache side channels.** Mitigation would require constant-time compression, which is incompatible with the codec's design goals. Callers concerned about side channels should encrypt the codec's output before time-of-check (VaptVupt does — encryption is the next stage in the pipeline).

2. **Format compatibility with future zstd / lz4 versions.** VaptVupt's wire format is independent and will not converge to either competitor's format. Frame-level format detection at the application layer (look for magic bytes) is the responsibility of any application that needs to support multiple codecs.

---

## 5. Verification Reproduction

To rerun the documented mechanisms on a fresh checkout, on Ubuntu 22.04+ or
equivalent, install each named tool and run the applicable commands. Results in
Section 2 remain historical until such a rerun is recorded:

```bash
# Static analysis
make clean
cppcheck --enable=warning,performance,portability -I include src/  # → 0 findings
scan-build --status-bugs make                                       # → 0 bugs
make clean
make CFLAGS="-Wall -Wextra -Werror -Wpedantic -Wshadow -Wcast-qual -O2 -std=c11 -D_POSIX_C_SOURCE=199309L -Iinclude"

# Sanitizer matrix (one build at a time)
printf 'formal-audit smoke input\n' > /tmp/vv-audit-input
for s in address undefined integer leak; do
    make clean
    make CC=clang CFLAGS="-Wall -Wextra -Werror -O1 -g -std=c11 -D_POSIX_C_SOURCE=199309L -Iinclude -fsanitize=$s" LDFLAGS="-fsanitize=$s"
    ./vaptvupt -c -m extreme /tmp/vv-audit-input -o /tmp/c.zupt
    ./vaptvupt -d /tmp/c.zupt -o /tmp/dec
    cmp /tmp/vv-audit-input /tmp/dec
done

# Test suite
make clean
make
make test

# DoS reproducers
make test_dos_hang
./test_dos_hang                                                     # → 12 payloads, <60ms each

# Fuzz (clang/libFuzzer; target carries its configured smoke budget)
make fuzz-libfuzzer

# Allocation fault injection
VV_BIN=./vaptvupt CC=cc sh tests/oom_sweep.sh

# Bounded/deductive verification (tool-dependent)
make verify
```

The sanitizer loop, fuzz targets, and formal tools have host-dependent runtimes.
A missing tool or skipped target must be reported, not counted as a pass.

---

## 6. Reporting Vulnerabilities

Report security issues via the contact in `SECURITY.md`. The codec is
GPL-3.0-or-later; downstream projects building on it inherit the GPL
obligations. The project does not advertise a paid bug-bounty program.

A vulnerability is defined as: any input that causes the *decoder* (one-shot or streaming) to:

- Crash (segfault, abort, panic)
- Loop indefinitely or for >1 second on inputs <100 KB
- Read or write outside `dst_cap` or the input buffer
- Leak memory
- Fail under any of the runtime sanitizers in §2.2 with valid inputs

Encoder bugs are accepted but considered lower severity — the encoder runs on caller-chosen data.

---

## 7. Status

This document is committed to the repo and is the source of truth for "what has
been audited" claims. Updates accompany every patch release that changes the
audit posture. Revision 1.3 covers the v2.65.9 delta under the currency and
scope limits above; historical result tables retain the versions on which the
underlying evidence was originally collected.
