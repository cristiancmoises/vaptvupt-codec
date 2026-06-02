# Formal verification

The BCJ branch filters (`src/vv_bcj.c`) run on the decode path: the inverse
transform processes attacker-controlled decompressed bytes. They are small,
pure, and bounded, which makes them tractable to verify rather than merely
fuzz. This directory contains CBMC harnesses that machine-check their safety
and correctness.

## What is proven

For fully nondeterministic input buffers of every length up to a bound,
[CBMC](https://www.cprover.org/cbmc/) proves:

| Harness | Function(s) | Properties |
|---|---|---|
| `cbmc_bcj_x86.c`    | `vv_bcj_x86`    | memory safety; no signed overflow; no invalid conversion; `inverse(forward(x)) == x` (lossless) — sizes 0..12 |
| `cbmc_bcj_arm64.c`  | `vv_bcj_arm64`  | memory safety; no signed overflow; no invalid conversion; `inverse(forward(x)) == x` (lossless) — sizes 0..16 |
| `cbmc_bcj_detect.c` | `vv_bcj_detect` | memory safety on arbitrary/truncated input, including the PE-header offset read from the input itself — sizes 0..72 |

"Memory safety" = CBMC's `--bounds-check` and `--pointer-check`: no
out-of-bounds or invalid pointer access. Proofs use `--unwinding-assertions`,
so the unwind bounds are themselves verified to be sufficient (the result is
sound up to the stated sizes, not merely a bounded search).

The filters use modular unsigned arithmetic by design (the relative→absolute
conversion wraps and is masked), which is defined behaviour in C, so
`--unsigned-overflow-check` is deliberately not enabled; every other standard
CBMC safety check is.

The bijection proof is the property the codec depends on: a filter must never
corrupt data, regardless of input. It complements the runtime fuzzing in
`tests/test_bcj.c` (tens of thousands of random/adversarial round trips) with
an exhaustive guarantee over all inputs up to the bound.

## Running

Requires CBMC (Debian/Ubuntu: `apt-get install cbmc`). From the repo root:

```sh
sh verification/verify.sh
# or
make verify
```

Each harness prints `VERIFICATION SUCCESSFUL`.
