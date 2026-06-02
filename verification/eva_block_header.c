/* SPDX-License-Identifier: GPL-3.0-or-later
 * Frama-C/Eva + RTE: the block-header pack/unpack accessors raise no runtime
 * error (no shift/overflow UB) for any field values, and pack is free of
 * signed/overflow issues. Definitions copied from include/vaptvupt.h. */
#include <stdint.h>
#include "__fc_builtin.h"

typedef enum { VV_BLOCK_RAW=0, VV_BLOCK_RLE=1, VV_BLOCK_COMPRESSED=2, VV_BLOCK_RESV=3 } vv_block_type_t;
static inline vv_block_type_t vv_bh_type(uint32_t h) { return (vv_block_type_t)(h & 3); }
static inline int      vv_bh_last(uint32_t h) { return (h >> 2) & 1; }
static inline uint32_t vv_bh_size(uint32_t h) { return (h >> 3) & 0x1FFFFF; }
static inline uint32_t vv_bh_pack(vv_block_type_t t, int last, uint32_t sz) {
    return (uint32_t)t | ((uint32_t)last << 2) | (sz << 3);
}
int main(void) {
    uint32_t h = Frama_C_interval(0, 0xFFFFFFFF);
    volatile uint32_t s = (uint32_t)vv_bh_type(h);
    s = (uint32_t)vv_bh_last(h);
    s = vv_bh_size(h);
    uint32_t t = Frama_C_interval(0,3), l = Frama_C_interval(0,1), sz = Frama_C_interval(0, (1u<<21)-1);
    s = vv_bh_pack((vv_block_type_t)t, (int)l, sz);
    (void)s;
    return 0;
}
