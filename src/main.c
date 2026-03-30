/*
 * VaptVupt CLI — Command-line interface
 *
 * Usage:
 *   vaptvupt -c [-m mode] [-o output] input      Compress
 *   vaptvupt -d [-o output] input                 Decompress
 *   vaptvupt -t input                             Test (decompress + verify)
 *   vaptvupt -b input                             Benchmark
 *
 * Modes: fast (ultra-fast), balanced (default), extreme
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr,
        "VaptVupt %s — Next-generation lossless compression\n"
        "\n"
        "Usage:\n"
        "  vaptvupt -c [-m mode] [-o out.vv] input    Compress\n"
        "  vaptvupt -d [-o output] input.vv            Decompress\n"
        "  vaptvupt -t input.vv                        Test integrity\n"
        "  vaptvupt -b input                           Benchmark\n"
        "\n"
        "Modes: fast, balanced (default), extreme\n"
        "\n"
        "Options:\n"
        "  -m mode   Compression mode (fast/balanced/extreme)\n"
        "  -o file   Output file (default: input.vv / input.orig)\n"
        "  -v        Verbose output\n"
        "  -h        Show this help\n",
        VV_VERSION_STRING);
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static vv_mode_t parse_mode(const char *s) {
    if (strcmp(s, "fast") == 0) return VV_MODE_ULTRA_FAST;
    if (strcmp(s, "extreme") == 0) return VV_MODE_EXTREME;
    return VV_MODE_BALANCED;
}

int main(int argc, char **argv) {
    int do_compress = 0, do_decompress = 0, do_test = 0, do_bench = 0;
    const char *mode_str = "balanced";
    const char *output_path = NULL;
    const char *input_path = NULL;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) do_compress = 1;
        else if (strcmp(argv[i], "-d") == 0) do_decompress = 1;
        else if (strcmp(argv[i], "-t") == 0) do_test = 1;
        else if (strcmp(argv[i], "-b") == 0) do_bench = 1;
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) mode_str = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-h") == 0) { usage(); return 0; }
        else if (argv[i][0] != '-') input_path = argv[i];
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }

    if (!input_path || !(do_compress || do_decompress || do_test || do_bench)) {
        usage();
        return 1;
    }

    size_t input_len = 0;
    uint8_t *input_data = read_file(input_path, &input_len);
    if (!input_data) return 1;

    /* ─── COMPRESS ─── */
    if (do_compress) {
        vv_options_t opts;
        vv_default_options(&opts);
        opts.mode = parse_mode(mode_str);
        opts.verbose = verbose;

        size_t dst_cap = vv_compress_bound(input_len);
        uint8_t *dst = (uint8_t *)malloc(dst_cap);
        if (!dst) { fprintf(stderr, "Out of memory\n"); free(input_data); return 1; }

        double t0 = now_sec();
        int64_t comp_size = vv_compress(input_data, input_len, dst, dst_cap, &opts);
        double t1 = now_sec();

        if (comp_size < 0) {
            fprintf(stderr, "Compression failed: %lld\n", (long long)comp_size);
            free(dst); free(input_data); return 1;
        }

        /* Default output name */
        char out_buf[4096];
        if (!output_path) {
            snprintf(out_buf, sizeof(out_buf), "%s.vv", input_path);
            output_path = out_buf;
        }

        if (write_file(output_path, dst, (size_t)comp_size) < 0) {
            free(dst); free(input_data); return 1;
        }

        double ratio = input_len > 0 ? (double)input_len / (double)comp_size : 1.0;
        double speed = (double)input_len / (t1 - t0) / (1024.0 * 1024.0);

        fprintf(stderr, "Compressed %zu → %lld bytes (%.2f:1, %.1f MB/s, %s mode)\n",
                input_len, (long long)comp_size, ratio, speed, mode_str);

        free(dst);
    }

    /* ─── DECOMPRESS ─── */
    if (do_decompress || do_test) {
        /* Read content size from header */
        if (input_len < sizeof(vv_frame_header_t)) {
            fprintf(stderr, "Input too small\n"); free(input_data); return 1;
        }
        vv_frame_header_t fh;
        memcpy(&fh, input_data, sizeof(fh));
        size_t dst_cap = (size_t)fh.content_size;
        if (dst_cap == 0) dst_cap = input_len * 8;  /* Guess */
        /* Add slack for SIMD over-copy */
        uint8_t *dst = (uint8_t *)calloc(1, dst_cap + 64);
        if (!dst) { fprintf(stderr, "Out of memory\n"); free(input_data); return 1; }

        double t0 = now_sec();
        int64_t decomp_size = vv_decompress(input_data, input_len, dst, dst_cap);
        double t1 = now_sec();

        if (decomp_size < 0) {
            fprintf(stderr, "Decompression failed: %lld\n", (long long)decomp_size);
            free(dst); free(input_data); return 1;
        }

        double speed = (double)decomp_size / (t1 - t0) / (1024.0 * 1024.0);

        if (do_test) {
            fprintf(stderr, "Test OK: %lld bytes decompressed (%.1f MB/s)\n",
                    (long long)decomp_size, speed);
        } else {
            char out_buf[4096];
            if (!output_path) {
                /* Strip .vv extension */
                snprintf(out_buf, sizeof(out_buf), "%s.orig", input_path);
                output_path = out_buf;
            }
            if (write_file(output_path, dst, (size_t)decomp_size) < 0) {
                free(dst); free(input_data); return 1;
            }
            fprintf(stderr, "Decompressed %zu → %lld bytes (%.1f MB/s)\n",
                    input_len, (long long)decomp_size, speed);
        }
        free(dst);
    }

    /* ─── BENCHMARK ─── */
    if (do_bench) {
        const char *modes[] = {"fast", "balanced", "extreme"};
        vv_mode_t mvs[] = {VV_MODE_ULTRA_FAST, VV_MODE_BALANCED, VV_MODE_EXTREME};

        fprintf(stderr, "%-10s %10s %10s %8s %10s %10s\n",
                "Mode", "Orig", "Comp", "Ratio", "Enc MB/s", "Dec MB/s");
        fprintf(stderr, "─────────────────────────────────────────────────────────\n");

        for (int mi = 0; mi < 3; mi++) {
            vv_options_t opts;
            vv_default_options(&opts);
            opts.mode = mvs[mi];

            size_t dst_cap = vv_compress_bound(input_len);
            uint8_t *comp = (uint8_t *)malloc(dst_cap);
            if (!comp) continue;

            /* Compress */
            double t0 = now_sec();
            int64_t comp_size = vv_compress(input_data, input_len, comp, dst_cap, &opts);
            double t1 = now_sec();
            if (comp_size < 0) { free(comp); continue; }
            double enc_speed = (double)input_len / (t1 - t0) / (1024.0 * 1024.0);

            /* Decompress */
            vv_frame_header_t fh;
            memcpy(&fh, comp, sizeof(fh));
            size_t dec_cap = (size_t)fh.content_size + 64;
            uint8_t *dec = (uint8_t *)calloc(1, dec_cap + 64);
            if (!dec) { free(comp); continue; }

            t0 = now_sec();
            int64_t dec_size = vv_decompress(comp, (size_t)comp_size, dec, dec_cap);
            t1 = now_sec();
            double dec_speed = dec_size > 0 ? (double)dec_size / (t1 - t0) / (1024.0 * 1024.0) : 0;

            /* Verify */
            int ok = (dec_size == (int64_t)input_len && memcmp(input_data, dec, input_len) == 0);

            double ratio = (double)input_len / (double)comp_size;
            fprintf(stderr, "%-10s %10zu %10lld %7.2f:1 %9.1f %9.1f %s\n",
                    modes[mi], input_len, (long long)comp_size, ratio,
                    enc_speed, dec_speed, ok ? "✓" : "FAIL");

            free(comp); free(dec);
        }
    }

    free(input_data);
    return 0;
}
