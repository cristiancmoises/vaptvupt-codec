/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In-process encoder allocation benchmark. The deterministic fixtures,
 * fingerprint seed and >=80 ms doubling calibration preserve the protocol
 * used for the v2.65.9/v2.65.10 paired measurements in COMPARISON.md.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int parse_number(const char *s, size_t limit, size_t *out) {
    size_t value = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned digit = (unsigned)(*s - '0');
        if (value > limit / 10 ||
            (value == limit / 10 && digit > limit % 10)) return 0;
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

int main(int argc, char **argv) {
    size_t n, mode, kind;
    if (argc != 4 || !parse_number(argv[1], VV_MAX_BLOCK_SIZE, &n) ||
        n == 0 || !parse_number(argv[2], 2, &mode) ||
        !parse_number(argv[3], 2, &kind)) {
        fprintf(stderr, "usage: %s bytes mode fixture\n"
                "  bytes: 1..%u; mode: 0=fast, 1=balanced, 2=extreme\n"
                "  fixture: 0=varying words, 1=random, 2=43-byte period\n"
                "output: bytes mode fixture compressed_bytes hash iterations ns/call\n",
                argv[0], (unsigned)VV_MAX_BLOCK_SIZE);
        return 2;
    }
    uint8_t *src = malloc(n + 1), *out = malloc(vv_compress_bound(n));
    uint8_t *decoded = malloc(n + 1);
    int result = 0;
    if (!src || !out || !decoded) {
        fprintf(stderr, "benchmark allocation failed\n");
        result = 2;
        goto done;
    }
    uint32_t x = 1234567;
    /* The measured harness used 12-byte rows: "compression " had no NUL,
     * so its strlen included the following "matcher " row. Preserve those
     * exact fixture bytes explicitly while giving every row a terminator. */
    static const char words[][24] = {
        "hello ", "world ", "brown ", "lazy ", "compression matcher ",
        "matcher ", "record ", "fox ", "test ", "stream "
    };
    for (size_t i = 0; i < n;) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        if (kind == 0) {
            const char *w = words[x % 10];
            size_t z = strlen(w);
            if (z > n - i) z = n - i;
            memcpy(src + i, w, z); i += z;
        } else if (kind == 1) {
            src[i++] = (uint8_t)x;
        } else {
            src[i] = (uint8_t)(i % 43); i++;
        }
    }
    vv_options_t opts;
    vv_default_options(&opts); opts.mode = (vv_mode_t)mode;
    int64_t z = vv_compress(src, n, out, vv_compress_bound(n), &opts);
    if (z < 0 || vv_decompress(out, (size_t)z, decoded, n) != (int64_t)n ||
        memcmp(src, decoded, n)) {
        fprintf(stderr, "benchmark roundtrip failed\n");
        result = 3;
        goto done;
    }
    /* Preserve the original rolling hash's nonstandard initial seed. */
    uint64_t fingerprint = 1469598103934665603ULL;
    for (int64_t i = 0; i < z; i++)
        fingerprint = (fingerprint ^ out[i]) * 1099511628211ULL;
    size_t iters = 1;
    double elapsed;
    do {
        double start = seconds();
        for (size_t i = 0; i < iters; i++) {
            if (vv_compress(src, n, out, vv_compress_bound(n), &opts) != z) {
                fprintf(stderr, "compressed size changed during benchmark\n");
                result = 4;
                goto done;
            }
        }
        elapsed = seconds() - start;
        if (elapsed < 0.08) {
            if (iters > SIZE_MAX / 2) {
                fprintf(stderr, "benchmark calibration overflow\n");
                result = 2;
                goto done;
            }
            iters *= 2;
        }
    } while (elapsed < 0.08);
    printf("%zu %zu %zu %lld %016llx %zu %.3f\n", n, mode, kind,
           (long long)z, (unsigned long long)fingerprint, iters,
           elapsed * 1e9 / iters);
done:
    free(src); free(out); free(decoded);
    return result;
}
