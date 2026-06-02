# Formal verification

The BCJ branch filters (`src/vv_bcj.c`) and selected decoder helpers run on
the decode path: they process attacker-controlled bytes (the inverse filter
transforms decompressed output; the decoder parses untrusted compressed
input). They are small, pure or bounded, which makes them tractable to verify
rather than merely fuzz. This directory contains CBMC harnesses that
machine-check their safety and correctness.

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

## Running

Requires CBMC (Debian/Ubuntu: `apt-get install cbmc`). From the repo root:

```sh
sh verification/verify.sh
# or
make verify
```

Each harness prints `VERIFICATION SUCCESSFUL`.
