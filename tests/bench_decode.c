/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In-process decode-speed benchmark. Eliminates process-startup
 * overhead so we measure the actual decoder hot path.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint8_t *read_file(const char *path, size_t *out_len) {
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_len = (size_t)st.st_size;
    return buf;
}

static double bench_decode(const uint8_t *cmp, size_t cmp_len,
                           uint8_t *out, size_t out_cap, int iters) {
    double t0 = now_sec();
    for (int i = 0; i < iters; i++) {
        int64_t r = vv_decompress(cmp, cmp_len, out, out_cap);
        if (r < 0) { fprintf(stderr, "decode failed: %lld\n", (long long)r); exit(1); }
    }
    return now_sec() - t0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file [file...]\n", argv[0]);
        return 1;
    }

    printf("Decode-speed benchmark (in-process, MB/s)\n");
    printf("  %-15s %10s %10s %8s %8s\n", "fixture", "raw", "cmp", "iters", "MB/s");

    for (int a = 1; a < argc; a++) {
        size_t raw_len = 0;
        uint8_t *raw = read_file(argv[a], &raw_len);
        if (!raw) { fprintf(stderr, "read failed: %s\n", argv[a]); continue; }

        /* Compress in-memory, balanced mode */
        size_t cmp_cap = raw_len * 2 + 4096;
        uint8_t *cmp = (uint8_t *)malloc(cmp_cap);
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        int64_t cmp_len = vv_compress(raw, raw_len, cmp, cmp_cap, &opts);
        if (cmp_len < 0) { fprintf(stderr, "compress failed\n"); free(raw); free(cmp); continue; }

        uint8_t *out = (uint8_t *)malloc(raw_len);
        if (!out) { free(raw); free(cmp); continue; }

        /* Warm cache */
        vv_decompress(cmp, (size_t)cmp_len, out, raw_len);

        /* Choose iterations to give stable reading: aim for ~0.5s minimum */
        int iters = 5;
        double t_warm = bench_decode(cmp, (size_t)cmp_len, out, raw_len, iters);
        if (t_warm < 0.3) {
            iters = (int)(iters * 0.5 / t_warm) + 1;
        }

        /* Real measurement */
        double t = bench_decode(cmp, (size_t)cmp_len, out, raw_len, iters);
        double mb_s = (double)raw_len * iters / 1048576.0 / t;

        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        printf("  %-15s %10zu %10lld %8d %8.1f\n", name, raw_len, (long long)cmp_len, iters, mb_s);

        free(raw); free(cmp); free(out);
    }
    return 0;
}
