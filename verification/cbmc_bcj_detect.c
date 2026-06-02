/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CBMC harness: vv_bcj_detect must never read outside data[0..size), for any
 * size and any contents — including the computed PE-header offset, which is
 * read from the (untrusted) input itself. Proven over a nondeterministic
 * buffer of every length up to N.
 */
#include "vv_bcj.h"
#include <stdint.h>
#include <stddef.h>

#ifndef DET_N
#define DET_N 72        /* covers the PE path (offset field at 0x3C..0x3F) */
#endif

extern uint8_t nondet_u8(void);
extern size_t  nondet_size(void);

void harness(void) {
    size_t n = nondet_size();
    __CPROVER_assume(n <= DET_N);

    uint8_t buf[DET_N];
    for (size_t i = 0; i < DET_N; i++) buf[i] = nondet_u8();

    /* The only requirement is memory safety (bounds/pointer checks below).
     * The return value is an optimization hint, so no functional assertion. */
    vv_filter_kind_t k = vv_bcj_detect(buf, n);
    (void)k;
}

int main(void) { harness(); return 0; }
