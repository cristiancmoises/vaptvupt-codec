/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * VaptVupt — x86 BCJ filter (see src/vv_bcj.c).
 */
#ifndef VV_BCJ_H
#define VV_BCJ_H

#include <stddef.h>
#include <stdint.h>

/*
 * Transform data[0..size) in place with the reversible x86 branch
 * converter. encoding != 0 = forward (compress-side, relative->absolute);
 * encoding == 0 = inverse (decode-side). `ip` is the stream offset of
 * byte 0 (use 0 for whole-buffer transforms). Returns the prefix length
 * that may have been modified. Exact bijection on arbitrary input:
 * vv_bcj_x86(b,n,0,0) undoes vv_bcj_x86(b,n,0,1).
 */
size_t vv_bcj_x86(uint8_t *data, size_t size, uint32_t ip, int encoding);

#endif /* VV_BCJ_H */
