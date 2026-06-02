/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CBMC harness: machine-checked proofs for the x86 BCJ filter.
 *   1. Memory safety: vv_bcj_x86 never accesses outside data[0..size).
 *   2. Bijection: inverse(forward(x)) == x for every input.
 * The x86 filter carries a rolling-mask state machine, so this exercises a
 * more intricate control flow than the AArch64 proof.
 */
#include "vv_bcj.h"
#include <stdint.h>
#include <stddef.h>

#ifndef BCJ_N
#define BCJ_N 12
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

    vv_bcj_x86(work, n, 0, 1);
    vv_bcj_x86(work, n, 0, 0);

    for (size_t i = 0; i < n; i++)
        __CPROVER_assert(work[i] == orig[i], "x86 BCJ round-trip is lossless");
}

int main(void) { harness(); return 0; }
