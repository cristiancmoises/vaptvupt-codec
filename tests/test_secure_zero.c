/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_secure_zero — exercise every encoder cleanup path that invokes the
 * secure wipe helper before freeing plaintext-bearing scratch.
 *
 * Sprint 117 (v2.47.9): added when memory hygiene was introduced.
 *
 * Reading freed storage would itself be undefined behavior, so this is not a
 * post-free memory-content proof. It verifies sanitizer-clean cleanup and
 * byte-exact roundtrips for streaming destruction and the one-shot BCJ
 * private-copy path; source review/static analysis establishes that those
 * paths call vv_secure_zero before free.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char MARKER[] = "SECRET-PLAINTEXT-MARKER-DO-NOT-LEAK-1234567890ABCDEF";

static void fill_marker_input(uint8_t *input, size_t input_len) {
    size_t marker_len = strlen(MARKER);
    for (size_t i = 0; i + marker_len < input_len; i += 256) {
        memcpy(input + i, MARKER, marker_len);
        for (size_t j = i + marker_len; j < i + 256 && j < input_len; j++)
            input[j] = (uint8_t)((i ^ j) & 0xFF);
    }
}

static int compress_with_marker(uint8_t *out, size_t out_cap, size_t *out_len) {
    vv_options_t opts;
    vv_default_options(&opts);
    opts.checksum = 1;
    opts.mode = VV_MODE_BALANCED;

    vv_cstream_t *ctx = vv_cstream_create(&opts);
    if (!ctx) return 1;

    /* Build an input that places MARKER in many positions. Use 64 KB
     * so the encoder allocates real working buffers. */
    uint8_t input[65536];
    fill_marker_input(input, sizeof(input));

    size_t written = 0;
    int rc = vv_cstream_compress_chunk(ctx, input, sizeof(input),
                                        out, out_cap, &written, 1);
    if (rc < 0) {
        vv_cstream_destroy(ctx);
        return 2;
    }
    *out_len = written;
    vv_cstream_destroy(ctx);  /* triggers vv_secure_zero on working buffers */
    return 0;
}

int main(void) {
    int failures = 0;

    /* Test 1: streaming destroy completes cleanly under sanitizers.
     * If vv_secure_zero somehow corrupts post-free state, ASan will
     * fire here. */
    {
        uint8_t out[131072];
        size_t out_len = 0;
        int rc = compress_with_marker(out, sizeof(out), &out_len);
        if (rc != 0) {
            printf("FAIL: streaming compress + destroy returned %d\n", rc);
            failures++;
        } else {
            printf("PASS: streaming compress + destroy completes cleanly (%zu bytes)\n",
                   out_len);
        }

        /* Roundtrip the output to confirm the encode succeeded fully */
        uint8_t decoded[65536], expected[65536];
        fill_marker_input(expected, sizeof(expected));
        int64_t dlen = vv_decompress(out, out_len, decoded, sizeof(decoded));
        if (dlen != (int64_t)sizeof(decoded) ||
            memcmp(decoded, expected, sizeof(expected)) != 0) {
            printf("FAIL: streaming roundtrip differs (length %lld)\n",
                   (long long)dlen);
            failures++;
        } else {
            printf("PASS: encoded output round-trips correctly\n");
        }
    }

    /* Test 2: 100 alloc/destroy cycles with the same pattern. Stresses
     * the heap allocator's chunk reuse. If destroy fails to scrub, we
     * may see allocator misbehavior over many cycles. */
    {
        for (int i = 0; i < 100; i++) {
            uint8_t out[131072];
            size_t out_len = 0;
            int rc = compress_with_marker(out, sizeof(out), &out_len);
            if (rc != 0) {
                printf("FAIL: cycle %d returned %d\n", i, rc);
                failures++;
                break;
            }
        }
        if (failures == 0) {
            printf("PASS: 100 alloc/destroy cycles complete cleanly\n");
        }
    }

    /* Test 3: exercise the one-shot BCJ private-copy cleanup path. This is a
     * sanitizer-visible completion/roundtrip test, not a direct inspection
     * of freed memory contents. */
    {
        uint8_t input[16384];
        for (size_t i = 0; i < sizeof(input); i++) input[i] = (uint8_t)(i * 7 + 3);

        uint8_t out[32768];
        vv_options_t opts;
        vv_default_options(&opts);
        opts.filter_x86 = 1;
        int64_t out_len = vv_compress(input, sizeof(input), out, sizeof(out), &opts);
        uint8_t decoded[sizeof(input)];
        int64_t decoded_len = out_len > 0
                            ? vv_decompress(out, (size_t)out_len,
                                            decoded, sizeof(decoded))
                            : VV_ERR_CORRUPT;
        if (out_len < 0 || decoded_len != (int64_t)sizeof(input) ||
            memcmp(decoded, input, sizeof(input)) != 0) {
            printf("FAIL: one-shot compress returned %lld\n", (long long)out_len);
            failures++;
        } else {
            printf("PASS: one-shot BCJ cleanup path round-trips (%lld bytes)\n",
                   (long long)out_len);
        }
    }

    if (failures == 0) {
        printf("\nResults: 4/4 passed\n");
        return 0;
    } else {
        printf("\nResults: %d failed\n", failures);
        return 1;
    }
}
