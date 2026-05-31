/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — x86 BCJ (Branch/Call/Jump) filter.
 *
 * Purpose: improve compression of x86/x86-64 machine code. Near CALL (0xE8)
 * and JMP (0xE9) instructions carry a 32-bit little-endian *relative*
 * displacement. The same call target reached from different instruction
 * positions yields a *different* relative displacement, so to the
 * compressor these look like noise. Converting the displacement to an
 * absolute form (add the instruction's stream position) makes repeated
 * references to the same target encode identically, which the LZ+ANS stage
 * then compresses well. The inverse runs on decode, before the bytes are
 * handed back to the caller.
 *
 * This is a clean-room reimplementation of the well-known x86 branch-
 * converter algorithm (the same transform used by 7-Zip/xz and described
 * in the LZMA SDK). The algorithm is exactly reversible on ARBITRARY input
 * — it is a bijection, so applying the filter to non-x86 data and then
 * inverting it reproduces the input byte-for-byte. That property is
 * fuzz-verified in tests; do not "optimize" the masking logic without
 * re-checking inverse(forward(x)) == x on random and adversarial inputs.
 *
 * The buffer is transformed in place. `encoding` is non-zero for the
 * forward (compress-side) transform, zero for the inverse (decode-side).
 * The last up-to-4 bytes are never touched (no room for a full operand),
 * which is consistent between forward and inverse.
 */

#include "vv_bcj.h"
#include <stddef.h>
#include <stdint.h>

/* A near-branch displacement's most-significant byte is treated as a sign
 * extension: only 0x00 or 0xFF are considered "convertible". */
static inline int bcj_test_msb(uint8_t b) { return b == 0x00 || b == 0xFF; }

/*
 * Transform data[0..size) in place. `ip` is the stream position of byte 0
 * (always 0 for whole-buffer use). Returns the number of bytes processed
 * (the prefix that may have been modified); the caller does not need it for
 * whole-buffer use. Mirrors the reference state machine exactly so that the
 * forward and inverse are perfect inverses.
 */
size_t vv_bcj_x86(uint8_t *data, size_t size, uint32_t ip, int encoding) {
    if (size < 5)
        return 0;

    size_t pos = 0;
    uint32_t mask = 0;          /* rolling mask of recent E8/E9 sightings */
    size_t limit = size - 4;    /* last position with a full 4-byte operand */
    ip += 5;                    /* displacement is relative to end of insn */

    for (;;) {
        /* Scan forward to the next byte that looks like E8/E9 ( & 0xFE == E8 ). */
        uint8_t *p = data + pos;
        uint8_t *end = data + limit;
        for (; p < end; p++)
            if ((*p & 0xFE) == 0xE8)
                break;

        {
            size_t d = (size_t)(p - data) - pos;   /* bytes skipped */
            pos = (size_t)(p - data);
            if (p >= end) {
                return pos;                         /* done */
            }
            if (d > 2) {
                mask = 0;
            } else {
                mask >>= (unsigned)d;
                if (mask != 0 &&
                    (mask > 4 || mask == 3 ||
                     bcj_test_msb(p[(size_t)(mask >> 1) + 1]))) {
                    mask = (mask >> 1) | 4;
                    pos++;
                    continue;
                }
            }
        }

        if (bcj_test_msb(p[4])) {
            uint32_t v = ((uint32_t)p[4] << 24) | ((uint32_t)p[3] << 16) |
                         ((uint32_t)p[2] << 8)  | ((uint32_t)p[1]);
            uint32_t cur = ip + (uint32_t)pos;
            pos += 5;
            if (encoding) v += cur; else v -= cur;
            if (mask != 0) {
                unsigned sh = (mask & 6) << 2;
                if (bcj_test_msb((uint8_t)(v >> sh))) {
                    v ^= (((uint32_t)0x100 << sh) - 1);
                    if (encoding) v += cur; else v -= cur;
                }
                mask = 0;
            }
            p[1] = (uint8_t)v;
            p[2] = (uint8_t)(v >> 8);
            p[3] = (uint8_t)(v >> 16);
            p[4] = (uint8_t)(0 - ((v >> 24) & 1));
        } else {
            mask = (mask >> 1) | 4;
            pos++;
        }
    }
}
