/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CBMC harness: the 32-bit block-header pack/unpack is a faithful, lossless
 * round trip over the full valid field domain (type in 0..3, last in 0..1,
 * size in 0..2^21-1). The decoder unpacks attacker-controlled headers, so the
 * accessors must extract exactly the fields the encoder packed and never read
 * a field outside its declared range.
 *
 * Uses the public inline definitions from vaptvupt.h directly, so the proof
 * binds to the shipped accessors.
 */
#include "vaptvupt.h"
#include <stdint.h>

extern uint32_t nondet_u32(void);

void harness(void) {
    uint32_t t  = nondet_u32(); __CPROVER_assume(t < 4);
    uint32_t l  = nondet_u32(); __CPROVER_assume(l < 2);
    uint32_t sz = nondet_u32(); __CPROVER_assume(sz < (1u << 21));

    uint32_t packed = vv_bh_pack((vv_block_type_t)t, (int)l, sz);

    /* round trip: every field comes back exactly */
    __CPROVER_assert((uint32_t)vv_bh_type(packed) == t, "block header: type round-trips");
    __CPROVER_assert((uint32_t)vv_bh_last(packed) == l, "block header: last round-trips");
    __CPROVER_assert(vv_bh_size(packed) == sz,          "block header: size round-trips");

    /* accessor ranges hold for ANY 32-bit header, including corrupt input */
    uint32_t any = nondet_u32();
    __CPROVER_assert((uint32_t)vv_bh_type(any) < 4,        "block header: type always in range");
    __CPROVER_assert((uint32_t)vv_bh_last(any) < 2,        "block header: last always in range");
    __CPROVER_assert(vv_bh_size(any) < (1u << 21),         "block header: size always in range");
}

int main(void) { harness(); return 0; }
