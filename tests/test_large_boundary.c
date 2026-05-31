/*
 * VaptVupt — Sprint 64 regression test
 *
 * Exercises the Sprint 63-64 fix for the LL-coding 65536-byte literal-run
 * overflow bug. Before v2.44.0, a block containing a trailing literal
 * run of exactly 65536 bytes would silently lose 4096 bytes on decode
 * (ll_base[35]=61440 + ll_extra[35]=12 bits = max representable litlen
 * 65535; encoder feeding 65536 truncated to 61440 base + 0 extra).
 *
 * The minimal reproducer is:
 *     data = b'A' * 1048839 + random_bytes(65536)
 *
 * Produces 2 blocks:
 *   Block 0: 1MB of 'A' (compresses to ~93 bytes)
 *   Block 1: 263 'A' bytes (rep-match to block 0) + 65536 literal bytes
 *            = 65799 total output bytes
 *
 * Before the fix: decode produces 61703 bytes of block 1, returns
 *                 VV_ERR_CORRUPT.
 * After the fix:  round-trip is bit-exact.
 *
 * Additional coverage:
 *   - 65535-byte trailing literal (at the ceiling, no split needed)
 *   - 65537-byte trailing literal (1 byte over, split required)
 *   - 100000-byte trailing literal (multi-split: 65535 + 34465)
 *   - 200000-byte trailing literal (3-way split)
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int test_boundary(size_t prefix_len, size_t tail_len, unsigned seed, const char *label) {
    size_t total = prefix_len + tail_len;
    uint8_t *in = (uint8_t *)malloc(total);
    if (!in) { fprintf(stderr, "  %s: malloc fail\n", label); return 1; }

    /* Prefix: runs of 'A' */
    memset(in, 'A', prefix_len);

    /* Tail: deterministic pseudo-random */
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < tail_len; i++) {
        s = s * 1103515245u + 12345u;
        in[prefix_len + i] = (uint8_t)(s >> 16);
    }

    size_t cap = vv_compress_bound(total) + 4096;
    uint8_t *cmp = (uint8_t *)malloc(cap);
    if (!cmp) { free(in); return 1; }

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;

    int64_t cmp_len = vv_compress(in, total, cmp, cap, &opts);
    if (cmp_len <= 0) {
        fprintf(stderr, "  %s: compress failed (%ld)\n", label, (long)cmp_len);
        free(in); free(cmp);
        return 1;
    }

    uint8_t *out = (uint8_t *)malloc(total + 64);
    if (!out) { free(in); free(cmp); return 1; }

    int64_t out_len = vv_decompress(cmp, cmp_len, out, total + 64);
    if (out_len != (int64_t)total) {
        fprintf(stderr, "  %s: decompress len mismatch (got %ld, expected %zu)\n",
                label, (long)out_len, total);
        free(in); free(cmp); free(out);
        return 1;
    }
    if (memcmp(in, out, total) != 0) {
        fprintf(stderr, "  %s: decompressed content differs\n", label);
        free(in); free(cmp); free(out);
        return 1;
    }

    printf("  PASS  %s  (prefix=%zu tail=%zu cmp=%ld)\n",
           label, prefix_len, tail_len, (long)cmp_len);

    free(in); free(cmp); free(out);
    return 0;
}

int main(void) {
    int failures = 0;

    printf("test_large_boundary: LL_MAX=65535 literal-run overflow regression\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    /* The original reproducer */
    failures += test_boundary(1048839, 65536, 42, "reproducer_1048839_A_+_65536_rand");

    /* At the ceiling — no split needed */
    failures += test_boundary(1048576, 65535, 42, "at_LL_MAX_ceiling (tail=65535)");

    /* One byte over — minimal split */
    failures += test_boundary(1048576, 65537, 42, "one_over_LL_MAX (tail=65537)");

    /* Double-split case */
    failures += test_boundary(1048576, 100000, 42, "tail_100K (1-split)");

    /* Triple-split case */
    failures += test_boundary(1048576, 200000, 42, "tail_200K (3-way split)");

    /* Longer prefix, doesn't change semantics */
    failures += test_boundary(5 * 1024 * 1024, 65536, 999, "5MB_prefix_+_65536_rand");

    /* Exactly 2× LL_MAX boundary */
    failures += test_boundary(1048576, 131070, 42, "tail_2xLLMAX");

    printf("═══════════════════════════════════════════════════════════════\n");
    if (failures == 0) {
        printf("Results: ALL 7 cases passed\n");
        return 0;
    }
    printf("Results: %d/7 FAILED\n", failures);
    return 1;
}
