#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Machine-checked verification of the BCJ branch filters with CBMC
# (https://www.cprover.org/cbmc/). Proves, by bounded model checking over
# fully nondeterministic inputs:
#
#   * memory safety  - no out-of-bounds or invalid pointer access
#                      (--bounds-check --pointer-check)
#   * no signed overflow / no invalid conversion
#                      (--signed-overflow-check --conversion-check)
#   * losslessness    - inverse(forward(x)) == x  (the bijection the codec
#                      depends on), asserted in the x86 and ARM64 harnesses
#
# The filters are fixed-iteration over the buffer, so an unwind bound of
# (max size + slack) makes the proof complete for every input up to that size.
# Unsigned wraparound is intentional (modular arithmetic) and is defined
# behaviour in C, so --unsigned-overflow-check is deliberately not enabled.
#
# Requires: cbmc.  Debian/Ubuntu: apt-get install cbmc.  Run from repo root:
#   sh verification/verify.sh
set -e
CBMC="${CBMC:-cbmc}"
COMMON="-I include --bounds-check --pointer-check --conversion-check --signed-overflow-check --unwinding-assertions --function main"

if ! command -v "$CBMC" >/dev/null 2>&1; then
    echo "cbmc not found; install it (apt-get install cbmc) to run the proofs." >&2
    exit 2
fi

echo "== CBMC: x86 BCJ filter (memory safety + bijection, sizes 0..12) =="
"$CBMC" verification/cbmc_bcj_x86.c    src/vv_bcj.c $COMMON --unwind 14

echo "== CBMC: AArch64 BCJ filter (memory safety + bijection, sizes 0..16) =="
"$CBMC" verification/cbmc_bcj_arm64.c  src/vv_bcj.c $COMMON --unwind 17

echo "== CBMC: vv_bcj_detect (memory safety on arbitrary/truncated input, sizes 0..72) =="
"$CBMC" verification/cbmc_bcj_detect.c src/vv_bcj.c $COMMON --unwind 73

echo "All BCJ verification proofs passed."
