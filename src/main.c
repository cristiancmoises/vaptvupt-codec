/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
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
        "  vaptvupt -c [-m mode] [-T N] [-o out.vv] input   Compress\n"
        "  vaptvupt -d [-o output] input.vv                 Decompress\n"
        "  vaptvupt -t input.vv                             Test integrity\n"
        "  vaptvupt -b input                                Benchmark\n"
        "\n"
        "Modes: fast, balanced (default), extreme\n"
        "\n"
        "Options:\n"
        "  -m mode   Compression mode (fast/balanced/extreme)\n"
        "  -T N      Encode with N threads (1=single, 0=auto-detect CPUs).\n"
        "            Requires the binary to be built with -DVV_ENABLE_THREADS\n"
        "            and -lpthread; otherwise runs sequentially with multi-frame\n"
        "            output. Multi-threaded output is a valid .vv stream readable\n"
        "            by any vv_decompress call.\n"
        "  -o file   Output file (default: input.vv / input.orig)\n"
        "  --fast    With -d: skip XXH64 verification during decompress.\n"
        "            With -c: skip XXH64 footer generation during compress.\n"
        "            Safe when another layer (e.g. AES-GCM) provides\n"
        "            integrity. On decode: massive gains on random/binary\n"
        "            (up to 3× total throughput). On encode: modest ~5-7%%\n"
        "            speedup. The resulting frame has no XXH64 footer and\n"
        "            decodes identically with or without --fast.\n"
        "  --format-v2  Emit 'T' tag blocks (min_match=3, hash3 path).\n"
        "            Only decodable by vaptvupt v2.33.0+ decoders. Helps\n"
        "            binary/executable inputs (measured ~2-3.5%% smaller);\n"
        "            slightly worse on text-structured data. Opt-in.\n"
        "  -w, --window N  Window log (10..24 = 1 KiB..16 MiB; 0 = auto).\n"
        "            Overrides the per-mode adaptive default. Larger windows\n"
        "            help large, long-range-redundant inputs (measured\n"
        "            +4-6%% on nci/webster/mozilla at -w 24) but can hurt\n"
        "            inputs with little long-range structure. The frame\n"
        "            records the window; any decoder handles it (no format\n"
        "            change).\n"
        "  --bcj, --filter x86  Apply the reversible x86 BCJ branch filter\n"
        "            before compression. Improves x86/x86-64 machine-code\n"
        "            ratio (measured ~+3-7%%; e.g. libc.so 2.179x -> 2.251x,\n"
        "            beating gzip-9). The decoder inverts it automatically\n"
        "            via a header flag. Opt-in; requires a v2.53.4+ decoder.\n"
        "  --bcj-arm64, --filter arm64  Apply the reversible AArch64 (ARM64)\n"
        "            BCJ filter (BL + ADRP) before compression. Improves\n"
        "            AArch64 machine-code ratio (measured ~+2-5%%). Opt-in;\n"
        "            requires a v2.54.0+ decoder.\n"
        "  --auto-filter, --filter auto  Detect the input's executable header\n"
        "            (ELF/PE/Mach-O) and apply the matching BCJ filter\n"
        "            automatically, or none if not recognised. Opt-in.\n"
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
    int nthreads = 1;   /* 1 = single-threaded default */
    int fast_decode = 0;  /* --fast: skip XXH64 verification */
    int use_format_v2 = 0;  /* --format-v2: emit 'T' tag blocks (min_match=3) */
    int window_log = 0;     /* -w/--window N: explicit window log (0 = auto) */
    int filter_x86 = 0;     /* --filter=x86 / --bcj: x86 BCJ branch filter */
    int filter_arm64 = 0;   /* --filter=arm64 / --bcj-arm64: AArch64 filter */
    int filter_auto = 0;    /* --auto-filter: pick a filter from the header */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) do_compress = 1;
        else if (strcmp(argv[i], "-d") == 0) do_decompress = 1;
        else if (strcmp(argv[i], "-t") == 0) do_test = 1;
        else if (strcmp(argv[i], "-b") == 0) do_bench = 1;
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) mode_str = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) output_path = argv[++i];
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "--fast") == 0) fast_decode = 1;
        else if (strcmp(argv[i], "--format-v2") == 0) use_format_v2 = 1;
        else if (strcmp(argv[i], "--bcj") == 0) filter_x86 = 1;
        else if (strcmp(argv[i], "--bcj-arm64") == 0) filter_arm64 = 1;
        else if (strcmp(argv[i], "--auto-filter") == 0) filter_auto = 1;
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            const char *fname = argv[++i];
            if (strcmp(fname, "x86") == 0) filter_x86 = 1;
            else if (strcmp(fname, "arm64") == 0) filter_arm64 = 1;
            else if (strcmp(fname, "auto") == 0) filter_auto = 1;
            else { fprintf(stderr, "Unknown --filter %s (supported: x86, arm64, auto)\n", fname); return 1; }
        }
        else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--window") == 0)
                 && i + 1 < argc) {
            window_log = atoi(argv[++i]);
            /* Valid window logs are 10..24 (1 KiB .. 16 MiB). The 16 MiB
             * cap is the 3-byte (24-bit) offset wire-format limit. 0 keeps
             * the per-mode adaptive default. Reject anything else rather
             * than silently clamping, so the user knows their value was
             * out of range. */
            if (window_log != 0 && (window_log < 10 || window_log > 24)) {
                fprintf(stderr, "Invalid -w/--window %d: must be 10..24 "
                        "(1 KiB .. 16 MiB), or 0 for auto\n", window_log);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            nthreads = atoi(argv[++i]);
            if (nthreads < 0) nthreads = 0;
        }
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
        /* --fast on compress: skip XXH64 footer generation.
         * Modest speedup (~5–7%) on text/json; bigger on trivial
         * inputs where the codec work is near-free. Callers using
         * AES-GCM or similar AEAD already have stronger integrity. */
        if (fast_decode) opts.checksum = 0;
        /* --format-v2: encode with min_match=3, emitting 'T' tag
         * blocks. Only decodable by v2.33.0+ decoders. Infrastructure
         * is in place; real ratio gains require hash3 matcher
         * (Sprint 45). */
        opts.format_v2 = use_format_v2;

        /* -w/--window N: explicit window log, overriding the per-mode
         * adaptive default. The decoder reads window_log from the frame
         * header and handles any value up to 24 (3-byte offsets), so this
         * is forward/backward compatible — no format change. Larger
         * windows help large, long-range-redundant inputs (measured
         * +4–6% on nci/webster/mozilla at wlog=24) but can hurt inputs
         * with little long-range structure, so it is opt-in, not default. */
        if (window_log != 0) opts.window_log = (uint8_t)window_log;
        if (filter_x86 && filter_arm64) {
            fprintf(stderr, "Cannot combine --filter x86 and --filter arm64 (a file is one architecture)\n");
            return 1;
        }
        opts.filter_x86 = filter_x86;
        opts.filter_arm64 = filter_arm64;
        opts.filter_auto = filter_auto;

        /* MT path uses slightly larger bound because concatenated frames
         * have per-frame overhead. Add 64 KB per potential chunk. */
        size_t dst_cap = vv_compress_bound(input_len);
        if (nthreads != 1) {
            size_t n_chunks = (input_len + 4 * 1024 * 1024 - 1) / (4 * 1024 * 1024);
            if (n_chunks < 1) n_chunks = 1;
            dst_cap += n_chunks * 65536;
        }
        uint8_t *dst = (uint8_t *)malloc(dst_cap);
        if (!dst) { fprintf(stderr, "Out of memory\n"); free(input_data); return 1; }

        double t0 = now_sec();
        int64_t comp_size;
        if (nthreads != 1) {
            /* Use MT API (nthreads=0 means auto-detect, >1 means explicit) */
            comp_size = vv_compress_mt(input_data, input_len, dst, dst_cap, &opts,
                                       (unsigned int)nthreads, 0);
        } else {
            comp_size = vv_compress(input_data, input_len, dst, dst_cap, &opts);
        }
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
        /* For multi-frame streams (produced by vv_compress_mt), we need
         * to sum the content_size of every frame. Walk the stream once
         * to tally, then allocate. */
        size_t total_content = 0;
        size_t scan_pos = 0;
        int scan_ok = 1;
        while (scan_pos + sizeof(vv_frame_header_t) <= input_len) {
            vv_frame_header_t scan_fh;
            memcpy(&scan_fh, input_data + scan_pos, sizeof(scan_fh));
            if (scan_fh.magic != VV_MAGIC) { scan_ok = 0; break; }
            total_content += (size_t)scan_fh.content_size;
            /* Advance to next frame by reading block headers */
            size_t fp = scan_pos + sizeof(vv_frame_header_t);
            int has_cks = scan_fh.flags & 1;
            for (;;) {
                if (fp + 4 > input_len) { scan_ok = 0; break; }
                uint32_t bh_packed;
                memcpy(&bh_packed, input_data + fp, 4);
                vv_block_type_t btype = vv_bh_type(bh_packed);
                int is_last = vv_bh_last(bh_packed);
                uint32_t dsz = vv_bh_size(bh_packed);
                fp += 4;
                if (btype == VV_BLOCK_RAW) {
                    fp += dsz;
                } else if (btype == VV_BLOCK_RLE) {
                    fp += 1;
                } else if (btype == VV_BLOCK_COMPRESSED || btype == VV_BLOCK_ENTROPY) {
                    if (fp + 3 > input_len) { scan_ok = 0; break; }
                    uint32_t csz = (uint32_t)input_data[fp] | ((uint32_t)input_data[fp+1] << 8) | ((uint32_t)input_data[fp+2] << 16);
                    fp += 3 + csz;
                } else {
                    scan_ok = 0; break;
                }
                if (fp > input_len) { scan_ok = 0; break; }
                if (is_last) break;
            }
            if (!scan_ok) break;
            if (has_cks) fp += sizeof(vv_frame_footer_t);
            scan_pos = fp;
        }

        /* Read content size from header (first frame) as fallback */
        if (input_len < sizeof(vv_frame_header_t)) {
            fprintf(stderr, "Input too small\n"); free(input_data); return 1;
        }
        vv_frame_header_t fh;
        memcpy(&fh, input_data, sizeof(fh));

        size_t dst_cap = scan_ok ? total_content : (size_t)fh.content_size;
        if (dst_cap == 0) dst_cap = input_len * 8;  /* Guess */
        /* Defense against malicious/corrupted content_size:
         * - Never trust a value smaller than a reasonable guess, so a
         *   tiny content_size (e.g. 3 when the real data is 35 bytes)
         *   doesn't cause OVERFLOW during decode.
         * - Cap the upper bound so an absurdly-huge content_size
         *   doesn't cause an OOM allocation (DoS).
         * Lower bound = 8× input_size (typical decompression ratio).
         * Upper bound = max(lower_bound, 256 MB). */
        size_t floor_cap = input_len * 8;
        if (floor_cap < (256u << 20)) floor_cap = (256u << 20);
        if (dst_cap < (size_t)(input_len * 8)) dst_cap = input_len * 8;
        if (dst_cap > floor_cap) dst_cap = floor_cap;
        /* Add slack for SIMD over-copy */
        uint8_t *dst = (uint8_t *)calloc(1, dst_cap + 64);
        if (!dst) { fprintf(stderr, "Out of memory\n"); free(input_data); return 1; }

        double t0 = now_sec();
        uint32_t dec_flags = fast_decode ? VV_DECOMPRESS_SKIP_CHECKSUM
                                          : VV_DECOMPRESS_DEFAULT;
        int64_t decomp_size = vv_decompress_flags(input_data, input_len,
                                                   dst, dst_cap, dec_flags);
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
