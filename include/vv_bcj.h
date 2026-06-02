/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * VaptVupt — BCJ branch filters (see src/vv_bcj.c).
 *
 * Reversible, architecture-specific branch converters that improve the
 * compression of machine code by turning relative call targets into an
 * absolute form. Each is an exact bijection on arbitrary input, so a file
 * filtered with the wrong architecture (or no machine code at all) still
 * round-trips byte-for-byte.
 */
#ifndef VV_BCJ_H
#define VV_BCJ_H

#include <stddef.h>
#include <stdint.h>

/*
 * x86 / x86-64 BCJ. Converts near CALL (0xE8) and JMP (0xE9) relative
 * displacements to/from absolute. encoding != 0 = forward (compress-side),
 * 0 = inverse (decode-side). `ip` is the stream offset of byte 0 (use 0 for
 * whole-buffer transforms). Returns the prefix length that may have been
 * modified. vv_bcj_x86(b,n,0,0) undoes vv_bcj_x86(b,n,0,1).
 */
size_t vv_bcj_x86(uint8_t *data, size_t size, uint32_t ip, int encoding);

/*
 * AArch64 (ARM64) BL + ADRP filter. Converts BL (call) 26-bit relative word
 * offsets and ADRP (PC-relative page address) 21-bit page offsets to/from an
 * absolute form, each modulo its immediate width. Same calling convention as
 * vv_bcj_x86. Only BL (opcode 100101) and ADRP (1xx10000) are touched;
 * opcode and register bits are preserved, so the transform is an exact
 * bijection on arbitrary input. vv_bcj_arm64(b,n,0,0) undoes
 * vv_bcj_arm64(b,n,0,1).
 */
size_t vv_bcj_arm64(uint8_t *data, size_t size, uint32_t ip, int encoding);

#endif /* VV_BCJ_H */
