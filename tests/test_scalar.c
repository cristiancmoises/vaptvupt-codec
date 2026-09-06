/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vaptvupt.h"
#include <stdio.h>
#include <string.h>

#if !VV_DISABLE_SIMD || VV_HAS_AVX2 || VV_HAS_SSE2 || VV_HAS_NEON
#error "This test must use the scalar-only platform configuration"
#endif

int main(void) {
    static const size_t lengths[] = {0, 1, 3, 7, 8, 15, 16, 17, 31, 32,
                                     33, 63, 64, 65, 255, 256, 257, 4096};
    uint8_t actual[4352], expected[4352], source[4352];
    size_t checks = 0;
    for (size_t i = 0; i < sizeof(source); i++)
        source[i] = (uint8_t)(i * 37u + i / 13u);
    for (size_t skew = 0; skew < 16; skew++) {
        for (size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); n++) {
            size_t length = lengths[n], start = 160 + skew;
            memcpy(actual, source, sizeof(actual));
            memcpy(expected, source, sizeof(expected));
            memcpy(expected + start, source + 1, length);
            vv_copy_fast(actual + start, source + 1, length);
            if (memcmp(actual, expected, sizeof(actual))) return 1;
            checks++;
            for (uint32_t offset = 1; offset <= 128; offset++) {
                memcpy(actual, source, sizeof(actual));
                memcpy(expected, source, sizeof(expected));
                /* LZ overlap feeds bytes written earlier in the same match. */
                for (size_t i = 0; i < length; i++)
                    expected[start + i] = expected[start + i - offset];
                vv_copy_match(actual + start, offset, length);
                if (memcmp(actual, expected, sizeof(actual))) {
                    fprintf(stderr, "scalar match failed: offset=%u length=%zu\n",
                            offset, length);
                    return 1;
                }
                checks++;
            }
        }
    }
    printf("scalar copy PASS (%zu checks, including output canaries)\n", checks);
    return 0;
}
