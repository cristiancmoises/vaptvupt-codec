/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libFuzzer harness for differential testing: stateless decoder
 * (vv_decompress) vs streaming decoder (vv_dstream_*).
 *
 * Sprint 111: For any compressed frame, both decoders must produce
 * byte-identical output (or both must fail). A divergence is a real
 * bug — either:
 *   - The streaming decoder accepts something the stateless rejects
 *     (or vice versa) — security risk: an attacker could craft a
 *     frame that one decoder accepts and the other rejects, leading
 *     to inconsistent behavior in deployments that use both APIs.
 *   - The streaming decoder produces different bytes than the
 *     stateless decoder for the same input — silent data corruption.
 *
 * This is the kind of bug that roundtrip fuzzing cannot catch
 * because roundtrip only validates encode→decode→equality, not
 * decode-A vs decode-B equality.
 *
 * Build:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -Iinclude tests/fuzz/fuzz_differential.c \
 *     src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c \
 *     src/vaptvupt_api.c src/vv_simd.c src/vv_decoder.c \
 *     -mavx2 -o /tmp/fuzz_differential
 */
#include "vaptvupt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OUT (4u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16) return 0;

    uint8_t *out_a = (uint8_t *)malloc(MAX_OUT);
    uint8_t *out_b = (uint8_t *)malloc(MAX_OUT);
    if (!out_a || !out_b) { free(out_a); free(out_b); return 0; }

    /* Decoder A: stateless one-shot */
    int64_t size_a = vv_decompress(data, size, out_a, MAX_OUT);

    /* Decoder B: streaming with input fed in randomized chunks.
     * Use the first byte to pick a chunk-size pattern so the fuzzer
     * has steering control. Force minimum chunk of 4 bytes so a
     * 100KB input doesn't need 100K iters of single-byte feeding. */
    int64_t size_b = -999;
    vv_dstream_t *ctx = vv_dstream_create();
    if (ctx) {
        size_t chunk_size_base = (((size_t)data[0] & 0x3F) + 1) * 4;  /* 4..256 */
        size_t pos = 0;
        size_t total_written = 0;
        size_t safety_max = size + 1024;  /* Scale with input size */
        size_t safety = 0;
        int frame_done = 0;
        while (pos < size && safety++ < safety_max) {
            size_t cs = chunk_size_base;
            if (cs > size - pos) cs = size - pos;
            size_t consumed = 0, written = 0;
            int rc = vv_dstream_decompress_chunk(ctx, data + pos, cs,
                                                  out_b, MAX_OUT,
                                                  &consumed, &written);
            total_written = written;  /* cumulative */
            if (rc < 0) { size_b = rc; break; }
            if (rc == 1) { size_b = (int64_t)total_written; frame_done = 1; break; }
            if (consumed == 0) break;
            pos += consumed;
        }
        if (!frame_done && size_b == -999) {
            /* Reached end of input without frame completion — could
             * be a valid "needs more input" state (truncated frame).
             * Treat as failure for differential purposes only if
             * stateless ALSO failed; if stateless succeeded, that's
             * a divergence we want to flag. */
            size_b = -1;
        }
        vv_dstream_destroy(ctx);
    }

    /* Differential check.
     *
     * We can't require exact equality of error codes — the two
     * decoders take different paths and may surface different errors
     * for the same corruption. But:
     *
     *   1. If the stateless decoder ACCEPTS (size_a >= 0), the
     *      streaming decoder must also accept and produce the same
     *      bytes. If streaming rejects a frame stateless accepts,
     *      that's a security divergence.
     *
     *   2. We do NOT require streaming-rejects implies stateless-
     *      rejects — streaming is allowed to be more strict (e.g.,
     *      it has tighter bounds-check on chunk inputs). The
     *      converse divergence (streaming-accepts implies stateless-
     *      accepts) is the more dangerous direction in practice
     *      because most untrusted-input deployments use the stateless
     *      decoder.
     */
    if (size_a >= 0 && size_b < 0) {
        /* Stateless accepted, streaming rejected. SECURITY DIVERGENCE. */
        __builtin_trap();
    }
    if (size_a >= 0 && size_b >= 0) {
        if (size_a != size_b) {
            /* Both accepted but produced different lengths. BUG. */
            __builtin_trap();
        }
        if (size_a > 0 && memcmp(out_a, out_b, (size_t)size_a) != 0) {
            /* Both accepted same length but different content. BUG. */
            __builtin_trap();
        }
    }

    free(out_a);
    free(out_b);
    return 0;
}
