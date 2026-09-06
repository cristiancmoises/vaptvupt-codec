/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CBMC harness: the decoder's variable-length integer reader must never read
 * past the end of the input buffer, for any buffer contents and any starting
 * position. read_ext_len advances a pointer over a run of 0xFF continuation
 * bytes terminated by a < 255 byte (LZ4-style byte sum). An
 * over-read here would be a classic heap-buffer-overflow on attacker-
 * controlled compressed input.
 *
 * The function below is an exact copy of the static read_ext_len in
 * src/vv_decoder.c; this harness pins its memory-safety contract:
 *   1. every dereference is strictly inside [base, end)   (--pointer-check)
 *   2. on return, base <= *pp <= end                       (asserted)
 * verified over a nondeterministic buffer of every length up to N with fully
 * nondeterministic contents and a nondeterministic start offset.
 */
#include <stdint.h>
#include <stddef.h>

/* ---- exact copy of src/vv_decoder.c:read_ext_len ---- */
static size_t read_ext_len(const uint8_t **pp, const uint8_t *end) {
    size_t val = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        val += b;
        if (b < 255) {
            *pp = p;
            return val;
        }
    }
    *pp = p;
    /* Even a zero extension requires its terminating byte. All token
     * payloads are bounded by the 24-bit compressed-size field, so their
     * byte sums fit below SIZE_MAX on both 32- and 64-bit hosts. */
    return SIZE_MAX;
}
/* ----------------------------------------------------- */

#ifndef EXT_N
#define EXT_N 16
#endif

extern uint8_t nondet_u8(void);
extern size_t  nondet_size(void);

void harness(void) {
    size_t n = nondet_size();
    __CPROVER_assume(n <= EXT_N);

    uint8_t buf[EXT_N];
    for (size_t i = 0; i < EXT_N; i++) buf[i] = nondet_u8();

    size_t start = nondet_size();
    __CPROVER_assume(start <= n);          /* caller always passes ip in [base, end] */

    const uint8_t *base = buf;
    const uint8_t *end  = buf + n;
    const uint8_t *p    = buf + start;

    size_t value = read_ext_len(&p, end);  /* dereferences checked by --pointer-check */

    /* pointer-advance invariant the callers rely on for their own bounds math */
    __CPROVER_assert(p >= base, "read_ext_len: pointer does not move backwards");
    __CPROVER_assert(p <= end,  "read_ext_len: pointer never passes end");
    __CPROVER_assert(value == SIZE_MAX || (p > buf + start && p[-1] < 255),
                     "read_ext_len: success requires a terminating byte");
    __CPROVER_assert(value != SIZE_MAX || p == end,
                     "read_ext_len: missing terminator fails at input end");
}

int main(void) { harness(); return 0; }
