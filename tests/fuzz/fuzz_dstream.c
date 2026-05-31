/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libFuzzer harness for vv_dstream_decompress_chunk (streaming decoder).
 *
 * Sprint 110: The streaming decoder has its own state machine that is
 * NOT exercised by the Sprint 109 vv_decompress fuzzer. This harness
 * splits each fuzz input across randomized chunk boundaries and feeds
 * them through vv_dstream_decompress_chunk one at a time. Any bug
 * triggered by partial-frame handling, state-machine transitions, or
 * chunk-boundary edge cases will surface here.
 *
 * Build:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -Iinclude tests/fuzz/fuzz_dstream.c \
 *     src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c \
 *     src/vaptvupt_api.c src/vv_simd.c src/vv_decoder.c \
 *     -mavx2 -o /tmp/fuzz_dstream
 */
#include "vaptvupt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OUT (4u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16) return 0;

    vv_dstream_t *ctx = vv_dstream_create();
    if (!ctx) return 0;

    uint8_t *out = (uint8_t *)malloc(MAX_OUT);
    if (!out) { vv_dstream_destroy(ctx); return 0; }

    /* Split input into chunks. Use the input bytes themselves to derive
     * chunk sizes — gives the fuzzer control over boundary placement so
     * it can probe state-transition edge cases.
     *
     * vv_dstream API semantics:
     *   - dst pointer is STABLE across calls (decoder writes at offset
     *     it tracks internally based on previously-written bytes)
     *   - `written` returned is CUMULATIVE total, not per-chunk delta
     *   - `consumed` is per-call: bytes read from src this call
     *   - Returns 0 (need more), 1 (frame done), or negative on error
     */
    size_t pos = 0;
    int safety_iters = 0;
    while (pos < size && safety_iters++ < 4096) {
        /* Derive chunk size from input bytes (1 to ~1024). Cap at remaining. */
        size_t chunk_size;
        if (pos + 1 < size) {
            chunk_size = ((size_t)data[pos] | ((size_t)data[pos + 1] << 8)) & 0x3FF;
        } else {
            chunk_size = (size_t)data[pos] & 0x3F;
        }
        if (chunk_size > size - pos) chunk_size = size - pos;
        if (chunk_size == 0) chunk_size = 1;  /* Force progress */

        size_t consumed = 0, written = 0;
        int rc = vv_dstream_decompress_chunk(ctx, data + pos, chunk_size,
                                              out, MAX_OUT,
                                              &consumed, &written);
        if (rc < 0) break;
        if (rc == 1) break;  /* Frame complete */
        if (consumed == 0) break;  /* No progress on input -> stop */
        pos += consumed;
    }

    free(out);
    vv_dstream_destroy(ctx);
    return 0;
}
