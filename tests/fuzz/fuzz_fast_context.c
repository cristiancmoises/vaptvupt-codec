/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise the canonical FAST parser through both storage ownership paths. */
#include "vaptvupt.h"
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 65538) return 0;
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_ULTRA_FAST;
    opts.checksum = data[0] & 1;
    opts.window_log = (uint8_t)(10 + data[0] % 15);
    opts.accel = data[1];
    opts.no_rep = data[0] & 2;
    opts.format_v2 = data[0] & 4;
    data += 2;
    size -= 2;

    size_t bound = vv_compress_bound(size);
    void *storage = malloc(vv_fast_context_size(65536));
    uint8_t *encoded = malloc(bound);
    uint8_t *reference = malloc(bound);
    uint8_t *decoded = malloc(size + 1);
    if (!storage || !encoded || !reference || !decoded) abort();
    vv_fast_context_t *ctx;
    if (vv_fast_context_init(storage, vv_fast_context_size(65536), 65536,
                             &opts, &ctx) != VV_OK) abort();

    /* Shortening and restoring the input must not expose prior dictionary
     * entries. Decode into a misaligned buffer ending at the allocation end. */
    for (unsigned round = 0; round < 3; round++) {
        size_t n = round == 1 ? size / 2 : size;
        int64_t r = vv_compress(data, n, reference, bound, &opts);
        int64_t c = vv_fast_context_compress(ctx, data, n, encoded, bound);
        if (r < 0 || c != r || memcmp(reference, encoded, (size_t)c)) abort();
        int64_t d = vv_decompress(encoded, (size_t)c, decoded + 1, n);
        if (d != (int64_t)n || memcmp(data, decoded + 1, n)) abort();
        if (vv_fast_context_compress(ctx, data, n, encoded,
                                     vv_compress_bound(n) - 1) != VV_ERR_OVERFLOW)
            abort();

        /* An exact allocation makes footer capacity errors visible to ASan.
         * Existing one-shot APIs can reject small buffers before encoding. */
        size_t short_cap = (size_t)c - 1;
        uint8_t *short_output = malloc(short_cap);
        if (!short_output) abort();
        if (vv_compress(data, n, short_output, short_cap, &opts) != VV_ERR_OVERFLOW)
            abort();
        free(short_output);
    }
    free(decoded);
    free(reference);
    free(encoded);
    free(storage);
    return 0;
}
