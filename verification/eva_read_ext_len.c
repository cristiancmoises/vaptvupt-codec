/* SPDX-License-Identifier: GPL-3.0-or-later
 * Frama-C/Eva memory-safety analysis of the decoder varint reader.
 * Eva (abstract interpretation) + RTE proves the absence of runtime errors
 * (invalid pointer dereference, out-of-bounds read) without manual loop
 * invariants. Body is an exact copy of src/vv_decoder.c:read_ext_len. */
#include <stdint.h>
#include <stddef.h>
#include "__fc_builtin.h"

static size_t read_ext_len(const uint8_t **pp, const uint8_t *end) {
    size_t val = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        val += b;
        if (b < 255) break;
    }
    *pp = p;
    return val;
}

#define CAP 64
static uint8_t g_buf[CAP];

int main(void) {
    /* nondeterministic contents */
    Frama_C_make_unknown((char*)g_buf, CAP);
    /* nondeterministic valid length 0..CAP and start 0..n */
    size_t n = Frama_C_interval(0, CAP);
    size_t start = Frama_C_interval(0, n);
    const uint8_t *p = g_buf + start;
    const uint8_t *end = g_buf + n;
    read_ext_len(&p, end);
    return 0;
}
