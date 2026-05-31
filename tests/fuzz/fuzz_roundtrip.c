/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libFuzzer harness for vv_compress + vv_decompress roundtrip.
 *
 * Sprint 110: Sprint 109 only fuzzed the decoder. This harness fuzzes
 * the encoder by:
 *   1. Treating fuzz input as plaintext to compress
 *   2. Compressing it
 *   3. Decompressing the result
 *   4. Asserting byte-equality with the original
 *
 * Catches:
 *   - Encoder OOB writes (ASan)
 *   - Encoder UB (UBSan)
 *   - Encoder/decoder roundtrip violations (assert)
 *   - Allocation issues in the encoder
 *
 * Build:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -Iinclude tests/fuzz/fuzz_roundtrip.c \
 *     src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c \
 *     src/vaptvupt_api.c src/vv_simd.c src/vv_decoder.c \
 *     -mavx2 -o /tmp/fuzz_roundtrip
 */
#include "vaptvupt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define MAX_OUT (4u * 1024u * 1024u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* Skip pathological sizes: empty input is a valid edge case but
     * tested elsewhere; very large is just slow. */
    if (size > 1024 * 1024) return 0;

    /* Encode bound: per FORMAT.md the worst-case expansion is ~1% +
     * fixed overhead. Use 2× + 4KB to be safely above. */
    size_t enc_cap = size * 2 + 4096;
    uint8_t *enc = (uint8_t *)malloc(enc_cap);
    uint8_t *dec = (uint8_t *)malloc(MAX_OUT);
    if (!enc || !dec) { free(enc); free(dec); return 0; }

    /* Cycle through the 3 modes based on the first byte (or balanced
     * if input is empty). The fuzzer can steer mode selection through
     * its mutations. */
    vv_options_t opts;
    vv_default_options(&opts);
    if (size > 0) {
        switch (data[0] % 3) {
            case 0: opts.mode = VV_MODE_ULTRA_FAST; break;
            case 1: opts.mode = VV_MODE_BALANCED;   break;
            case 2: opts.mode = VV_MODE_EXTREME;    break;
        }
    }

    int64_t enc_size = vv_compress(data, size, enc, enc_cap, &opts);
    if (enc_size < 0) {
        /* Compression refused — legitimate (e.g., bound too tight).
         * Not a bug. */
        free(enc); free(dec);
        return 0;
    }

    int64_t dec_size = vv_decompress(enc, (size_t)enc_size, dec, MAX_OUT);
    if (dec_size < 0) {
        /* Decoder refused our own valid frame. THIS IS A BUG. */
        __builtin_trap();
    }
    if ((size_t)dec_size != size) {
        /* Round-trip size mismatch. THIS IS A BUG. */
        __builtin_trap();
    }
    if (size > 0 && memcmp(data, dec, size) != 0) {
        /* Round-trip content mismatch. THIS IS A BUG. */
        __builtin_trap();
    }

    free(enc);
    free(dec);
    return 0;
}
