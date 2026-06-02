/* SPDX-License-Identifier: GPL-3.0-or-later
 * Frama-C/Eva + RTE: the BCJ filters and the header detector raise no runtime
 * error (no invalid pointer access, no out-of-bounds, no UB) over
 * nondeterministic buffer contents and lengths. This is an abstract-
 * interpretation safety result complementing the bounded CBMC bijection
 * proofs (cbmc_bcj_*.c). Uses the real src/vv_bcj.c. */
#include <stdint.h>
#include <stddef.h>
#include "vv_bcj.h"
#include "__fc_builtin.h"

#define CAP 64
static uint8_t g_buf[CAP];

int main(void) {
    Frama_C_make_unknown((char*)g_buf, CAP);
    size_t n = Frama_C_interval(0, CAP);
    int enc = Frama_C_interval(0, 1);

    vv_bcj_x86(g_buf, n, 0, enc);
    Frama_C_make_unknown((char*)g_buf, CAP);
    vv_bcj_arm64(g_buf, n, 0, enc);
    Frama_C_make_unknown((char*)g_buf, CAP);
    vv_filter_kind_t k = vv_bcj_detect(g_buf, n);
    (void)k;
    return 0;
}
