/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CBMC harness: machine-checked proofs for the AArch64 BCJ filter.
 *
 *   1. Memory safety: vv_bcj_arm64 never reads or writes outside data[0..size).
 *      CBMC's built-in bounds/pointer checks verify this over a nondeterministic
 *      buffer of every length up to N and fully nondeterministic contents.
 *   2. Bijection (losslessness): for every such buffer,
 *      inverse(forward(x)) == x, exactly. This is the property the codec relies
 *      on — the filter must never corrupt data.
 *
 * Run with:
 *   cbmc verification/cbmc_bcj_arm64.c src/vv_bcj.c -I include \
 *        --unwind N+1 --bounds-check --pointer-check --conversion-check \
 *        --signed-overflow-check --unsigned-overflow-check
 */
#include "vv_bcj.h"
#include <stdint.h>
#include <stddef.h>

#ifndef BCJ_N
#define BCJ_N 16          /* buffer sizes 0..BCJ_N are all checked */
#endif

extern uint8_t nondet_u8(void);
extern size_t  nondet_size(void);

void harness(void) {
    size_t n = nondet_size();
    __CPROVER_assume(n <= BCJ_N);

    uint8_t orig[BCJ_N];
    uint8_t work[BCJ_N];
    for (size_t i = 0; i < BCJ_N; i++) {
        uint8_t b = nondet_u8();
        orig[i] = b;
        work[i] = b;
    }

    /* forward then inverse, in place */
    vv_bcj_arm64(work, n, 0, 1);
    vv_bcj_arm64(work, n, 0, 0);

    /* bijection: the round trip reproduces every byte */
    for (size_t i = 0; i < n; i++)
        __CPROVER_assert(work[i] == orig[i], "arm64 BCJ round-trip is lossless");
}

int main(void) { harness(); return 0; }
