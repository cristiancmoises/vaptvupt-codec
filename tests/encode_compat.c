/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * encode_compat: small C wrapper around vv_compress() that sets
 * `vv_options_t::compat_v246_5_decoder = 1` to suppress lit_fmt=4
 * output. Used by `reference/test_lit_fmt_3.py` to verify the
 * Python reference decoder against C-encoded lit_fmt=3 frames.
 *
 * Build:
 *   cc -Iinclude tests/encode_compat.c src/*.c -mavx2 -O2 \
 *     -o tests/encode_compat
 *
 * Usage:
 *   tests/encode_compat <input> <output.vv>
 *
 * Sprint 114 (v2.47.6): added when lit_fmt=3 support was ported
 * to the Python reference decoder. See AUDIT.md item 6.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input> <output.vv>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return 1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 1; }

    uint8_t *src = (uint8_t *)malloc((size_t)n);
    if (!src) { fclose(f); return 1; }
    size_t got = fread(src, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(src); fprintf(stderr, "short read\n"); return 1; }

    size_t cap = (size_t)n * 2 + 4096;
    uint8_t *dst = (uint8_t *)malloc(cap);
    if (!dst) { free(src); return 1; }

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;
    opts.compat_v246_5_decoder = 1;  /* force lit_fmt=3 instead of 4 */

    int64_t out_sz = vv_compress(src, (size_t)n, dst, cap, &opts);
    if (out_sz < 0) {
        fprintf(stderr, "compress failed: %lld\n", (long long)out_sz);
        free(src); free(dst);
        return 1;
    }

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror(argv[2]); free(src); free(dst); return 1; }
    fwrite(dst, 1, (size_t)out_sz, o);
    fclose(o);

    printf("encoded %ld -> %lld bytes (lit_fmt=3 forced)\n",
           n, (long long)out_sz);

    free(src); free(dst);
    return 0;
}
