/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libFuzzer harness for vv_decompress.
 *
 * Sprint 109: Coverage-guided fuzzing of the decoder.
 *
 * Build:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -Iinclude tests/fuzz/fuzz_decompress.c \
 *     src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c \
 *     src/vaptvupt_api.c src/vv_simd.c src/vv_decoder.c \
 *     -mavx2 -o /tmp/fuzz_decompress
 *
 * Run:
 *   /tmp/fuzz_decompress -max_total_time=60 corpus_dir
 */
#include "vaptvupt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Cap output buffer to keep fuzzing fast. Frames claiming much larger
 * outputs will fail VV_ERR_OVERFLOW which is correct codec behavior. */
#define MAX_OUT (4u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16) return 0;  /* Below header size */

    /* Allocate a fresh output buffer for each iteration so ASan catches
     * any out-of-bounds writes. */
    uint8_t *out = (uint8_t *)malloc(MAX_OUT);
    if (!out) return 0;

    int64_t r = vv_decompress(data, size, out, MAX_OUT);
    (void)r;  /* ignore result; we only care about safety */

    free(out);
    return 0;
}
