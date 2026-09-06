# Formal verification

The BCJ branch filters (`src/vv_bcj.c`) and selected decoder helpers run on
the decode path: they process attacker-controlled bytes (the inverse filter
transforms decompressed output; the decoder parses untrusted compressed
input). They are small, pure or bounded, which makes them tractable to verify
rather than merely fuzz. This directory contains CBMC harnesses that
machine-check their safety and correctness.

## v2.65.10 currency and scope

The `read_ext_len` implementation now returns `SIZE_MAX` for an extension
that reaches the compressed-input boundary without a byte below 255. Its
CBMC, Eva and ACSL harness copies follow the new code. The historical reader
proofs below do not cover this revision until those tools are rerun; tool
unavailability is not a passing proof result.

The current release validates this behavior through positive/negative token
fixtures, one-shot and fragmented streaming decoding, sanitizer checks and
reference parity. Additional dynamic regressions cover Huffman output
capacity and input truncation, ANS frequency totals, frame-window offsets,
streaming format reset, and CLI failure reporting. These do not extend the
formal proof set. Historical BCJ and block-header evidence remains scoped to
the unchanged functions and recorded bounds.

## v2.65.9 currency and scope (historical)

The formal baseline below is inherited for the unchanged functions and stated
bounds; v2.65.9 does **not** claim a fresh full CBMC/Frama-C rerun. Its new
direct sequence-tANS table builder and streaming decoder completion
orchestration are outside these formal harnesses and are covered by regression
and dynamic validation instead.

For the direct builder, a dedicated hook compares all decode-table entries with
the prior spread-plus-build path across 256 deterministic valid normalized
tables, followed by the ANS, SEQ, roundtrip, safe-zone, and exact-buffer suites.
For streaming BCJ completion, roundtrips cover x86 and AArch64 frames supplied
whole or in 7-byte chunks, with checksum both enabled and disabled. These tests
verify that the inverse runs exactly once after checksum validation or the
final checksumless block. API-contract cases separately require `VV_ERR_PARAM`
for invalid mode enums and simultaneous architecture filters. Streaming-encoder
create/reset cases also reject invalid modes and unsupported BCJ options rather
than accepting ignored filters, while decoder regressions reject headers that
set both BCJ bits in one-shot, streaming, and frame-info paths.

Dynamic release coverage also includes `test_seq_v2` 21/21: direct rejection
of an unrepresentable oversize nonterminal literal run and a deterministic
end-to-end lossless-fallback reproducer. Both reference decoders consume
trailing LL-only entries with the C-equivalent iteration bound and reject
dual-BCJ headers. They also implement exact x86/AArch64 inverses after checksum
validation; current-output fixtures cover checksum on and off. Legacy
H/A/I/C decoding remains canonical in C; Python retains limited A-tag support
and JavaScript omits the legacy tags. The OOM sweep validates its baseline
roundtrip before failure injection. These remain regression claims, not
additions to the formal proof set below.

The one-shot BCJ private input copy is now explicitly passed to
`vv_secure_zero` before `free()`. `test_secure_zero` exercises that cleanup
path and a byte-exact BCJ roundtrip under sanitizers; it does not inspect freed
storage or prove post-`free()` contents. The scrubbing claim therefore remains
an implementation/source-inspection claim with dynamic path coverage, not a
new formal result.

The historical CBMC/Eva results describe the BCJ filter functions,
detector, `read_ext_len`, and block-header helpers named below. Re-run
`make verify` on the target toolchain before treating the inherited evidence as
current certification. In particular, the current `read_ext_len` body differs
from the historical proved body as described above.

## What is proven

For fully nondeterministic inputs up to a bound,
[CBMC](https://www.cprover.org/cbmc/) proves:

| Harness | Function(s) | Properties |
|---|---|---|
| `cbmc_bcj_x86.c`        | `vv_bcj_x86`    | memory safety; no signed overflow; no invalid conversion; `inverse(forward(x)) == x` (lossless) — sizes 0..12 |
| `cbmc_bcj_arm64.c`      | `vv_bcj_arm64`  | memory safety; no signed overflow; no invalid conversion; `inverse(forward(x)) == x` (lossless) — sizes 0..16 |
| `cbmc_bcj_detect.c`     | `vv_bcj_detect` | memory safety on arbitrary/truncated input, including the PE-header offset read from the input itself — sizes 0..72 |
| `cbmc_read_ext_len.c`   | `read_ext_len`  | the decoder's varint reader never reads at or past the input end, and advances the pointer within `[base, end]` — buffers 0..16, any start offset |
| `cbmc_block_header.c`   | `vv_bh_pack` / `vv_bh_type` / `vv_bh_last` / `vv_bh_size` | pack/unpack is a lossless round trip over the full valid field domain, and every accessor returns in range for any 32-bit header (incl. corrupt input) |

"Memory safety" = CBMC's `--bounds-check` and `--pointer-check`: no
out-of-bounds or invalid pointer access. Proofs use `--unwinding-assertions`,
so the unwind bounds are themselves verified to be sufficient (the result is
sound up to the stated sizes, not merely a bounded search).

The filters use modular unsigned arithmetic by design (the relative→absolute
conversion wraps and is masked), which is defined behaviour in C, so
`--unsigned-overflow-check` is not enabled; every other standard CBMC safety
check is.

`read_ext_len` is the decode hot path's variable-length integer reader; an
over-read there would be a heap-buffer-overflow on attacker-controlled
compressed input. Its harness copies the function verbatim from
`src/vv_decoder.c`, and `verify.sh` fails if that copy drifts from the
shipped source, so the proof always binds to the code that runs.

These proofs complement the runtime fuzzing in `tests/test_bcj.c` and the
differential fuzzer (tens of thousands of random/adversarial cases) with an
exhaustive guarantee over all inputs up to the bound.

## Two tiers: CBMC (bounded) and Frama-C/Eva (abstract interpretation)

The CBMC proofs above are *bounded model checking*: exhaustive over all inputs
up to a size, with `--unwinding-assertions` confirming the bounds suffice.
They establish the bijection (losslessness) property, which abstract
interpretation cannot.

The harnesses prefixed `eva_` add a second, independent tier using Frama-C's
Eva plugin (abstract interpretation with RTE), which reasons about value
ranges symbolically rather than enumerating concrete inputs:

| Harness | Result |
|---|---|
| `eva_read_ext_len.c`   | **0 alarms** — no invalid pointer access, no out-of-bounds, no UB, for any buffer contents and any start offset |
| `eva_block_header.c`   | **0 alarms** — the pack/unpack accessors are free of shift/overflow UB for any field values |
| `eva_bcj.c`            | the BCJ filters and detector raise **3 residual obligations** (`\pointer_comparable` on the in-bounds scan comparisons, and one pointer-difference overflow check). These are Eva conservatism on pointer arithmetic that holds within a single object; they are discharged by the CBMC `--pointer-check` proofs above. No other alarms. |

`acsl_read_ext_len.c` carries the ACSL contract and loop invariant for an
*unbounded* deductive proof of `read_ext_len` via Frama-C's WP plugin. WP is
not in the `frama-c-base` package; with the full `frama-c` (WP) installed,
`frama-c -wp -wp-rte acsl_read_ext_len.c` discharges memory safety for buffers
of any size. The Eva result above provides a memory-safety guarantee that runs
with `frama-c-base` alone.

## Running

Requires CBMC for the bounded proofs (Debian/Ubuntu: `apt-get install cbmc`)
and, optionally, `frama-c-base` + `z3` for the Eva analyses
(`apt-get install frama-c-base z3`). From the repo root:

```sh
sh verification/verify.sh
# or
make verify
```

The CBMC harnesses print `VERIFICATION SUCCESSFUL`; the Eva analyses print
their alarm counts. If `frama-c` is absent, the Eva analyses are skipped with
a notice and the CBMC proofs still run.
