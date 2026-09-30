/* SPDX-License-Identifier: Apache-2.0
 * Small independent inputs must not allocate a file-sized matcher window.
 * Link with --wrap=malloc/calloc/realloc to exercise the real public encoder.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
static volatile int tracking;
static volatile size_t largest, rejected;
static const size_t allocation_limit = 1u << 20;

static int reject(size_t size) {
    if (!tracking) return 0;
    if (size > largest) largest = size;
    if (size <= allocation_limit) return 0;
    rejected++;
    return 1;
}
void *__wrap_malloc(size_t size) {
    return reject(size) ? NULL : __real_malloc(size);
}
void *__wrap_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return reject(n * size) ? NULL : __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t size) {
    return reject(size) ? NULL : __real_realloc(p, size);
}

int main(void) {
    static const size_t sizes[] = {0, 1, 3, 4, 5, 31, 64, 255, 1023, 1024,
                                  1025, 2047, 2048, 2049, 4095, 4096};
    unsigned failures = 0, cases = 0;
    void *(*volatile probe)(size_t) = malloc;
    tracking = 1;
    void *p = probe(allocation_limit + 1);
    tracking = 0;
    if (p || rejected != 1) { free(p); return 2; }
    for (unsigned mode = 0; mode < 3; mode++) {
        for (unsigned wlog = 10; wlog <= 24; wlog += 7) {
            for (unsigned kind = 0; kind < 3; kind++) {
                for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                    size_t n = sizes[s], cap = vv_compress_bound(n);
                    uint8_t *src = malloc(n ? n : 1), *dst = malloc(cap + 16);
                    uint8_t *decoded = malloc(n + 16);
                    if (!src || !dst || !decoded) return 2;
                    uint32_t rng = 1234567;
                    for (size_t i = 0; i < n; i++) {
                        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                        src[i] = kind == 0 ? (uint8_t)rng : kind == 1 ?
                            (uint8_t)(i % 43) :
                            (uint8_t)(i % 11 == 0 ? rng : i % 43);
                    }
                    memset(dst + cap, 0xa5, 16);
                    memset(decoded + n, 0xa5, 16);
                    vv_options_t opts;
                    vv_default_options(&opts);
                    opts.mode = (vv_mode_t)mode;
                    opts.window_log = (uint8_t)wlog;
                    largest = rejected = 0;
                    tracking = 1;
                    int64_t z = vv_compress(n ? src : NULL, n, dst, cap, &opts);
                    tracking = 0;
                    int ok = z > 0 && rejected == 0 && largest <= allocation_limit;
                    if (z > 0) {
                        int64_t produced = vv_decompress(dst, (size_t)z, decoded, n);
                        ok &= produced == (int64_t)n;
                        if (produced == (int64_t)n)
                            ok &= memcmp(src, decoded, n) == 0;
                    }
                    for (size_t i = 0; i < 16; i++)
                        ok &= dst[cap + i] == 0xa5 && decoded[n + i] == 0xa5;
                    cases++;
                    if (!ok) {
                        failures++;
                        fprintf(stderr, "FAIL mode=%u window=%u size=%zu fixture=%u"
                                " result=%lld largest=%zu rejected=%zu\n",
                                mode, wlog, n, kind, (long long)z, largest, rejected);
                    }
                    free(src); free(dst); free(decoded);
                }
            }
        }
    }
    printf("small encoder allocations: %u cases, %u failures\n", cases, failures);
    return failures != 0;
}
