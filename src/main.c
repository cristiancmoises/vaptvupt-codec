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
#include <limits.h>

static void usage(void) {
    fprintf(stderr,
        "VaptVupt %s — Next-generation lossless compression\n"
        "\n"
        "Usage:\n"
        "  vaptvupt -c [-m mode] [-T N] [-o out.zupt] input Compress\n"
        "  vaptvupt -d [-o output] input.zupt               Decompress\n"
        "  vaptvupt -t input.zupt                           Test integrity\n"
        "  vaptvupt -b input                                Benchmark\n"
        "\n"
        "Modes: fast, balanced (default), extreme\n"
        "\n"
        "Options:\n"
        "  -m mode   Compression mode (fast/balanced/extreme)\n"
        "  -T N      Encode with N threads (1=single, 0=auto-detect CPUs).\n"
        "            Requires the binary to be built with -DVV_ENABLE_THREADS\n"
        "            and -lpthread; otherwise runs sequentially with multi-frame\n"
        "            output. Multi-threaded output is a valid .zupt stream readable\n"
        "            by any vv_decompress call.\n"
        "  -o file   Output file (default: input.zupt on -c; input with the\n"
        "            .zupt/.vv suffix stripped, else input.orig, on -d)\n"
        "  -D, --depth N  Override the match-finder chain depth (1..4096).\n"
        "            Higher = better ratio, slower encode (smooth tradeoff);\n"
        "            0 (default) keeps the per-mode default (fast=4,\n"
        "            balanced=24, extreme=256). Output stays decodable by any\n"
        "            decoder; the default (0) is byte-identical to prior\n"
        "            releases.\n"
        "  -A, --accel N  Position-skip acceleration (0..64; 0 = automatic:\n"
        "            fast=2, balanced/extreme=1). >0 selects an explicit factor,\n"
        "            massively speeding up encode on incompressible or already-\n"
        "            compressed input (measured ~8-9x on random/gzip data) for\n"
        "            a small ratio cost on compressible data. Most useful with\n"
        "            -m fast; output stays decodable by any decoder.\n"
        "      --no-rep   Disable rep-match probing in the parser. In fast mode\n"
        "            (no entropy stage) this is measured ~10%% faster and\n"
        "            net-positive on ratio for text/structured data (logs,\n"
        "            CSV, JSON), with a small ratio cost on some binaries.\n"
        "            Opt-in; default keeps rep enabled (byte-identical).\n"
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
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path); fclose(f); return NULL;
    }
    /* malloc(0) may return NULL even though an empty file is valid input. */
    uint8_t *buf = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    if (fwrite(data, 1, len, f) != len) {
        perror(path); fclose(f); return -1;
    }
    /* A small write may succeed only in stdio's buffer. The final flush can
     * still fail (disk full, quota, device error); success requires close. */
    if (fclose(f) != 0) { perror(path); return -1; }
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
    if (strcmp(s, "balanced") == 0) return VV_MODE_BALANCED;
    return (vv_mode_t)-1;
}

/* Decimal options must consume the whole argument. atoi accepts typos as
 * zero and has undefined behavior for values outside the int range. */
static int parse_number(const char *s, unsigned limit, int *out) {
    unsigned value = 0;
    if (!*s) return 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0;
        unsigned digit = (unsigned)(*s - '0');
        if (value > limit / 10 ||
            (value == limit / 10 && digit > limit % 10)) return 0;
        value = value * 10 + digit;
    }
    *out = (int)value;
    return 1;
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
    int depth_override = 0; /* --depth N: override match-finder chain depth */
    int accel = 0;          /* --accel N: lz4-style position-skip on no-match */
    int no_rep = 0;         /* --no-rep: disable rep-match probing (fast mode) */

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
        else if (strcmp(argv[i], "--no-rep") == 0) no_rep = 1;
        else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            const char *fname = argv[++i];
            if (strcmp(fname, "x86") == 0) filter_x86 = 1;
            else if (strcmp(fname, "arm64") == 0) filter_arm64 = 1;
            else if (strcmp(fname, "auto") == 0) filter_auto = 1;
            else { fprintf(stderr, "Unknown --filter %s (supported: x86, arm64, auto)\n", fname); return 1; }
        }
        else if ((strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--depth") == 0)
                 && i + 1 < argc) {
            const char *value = argv[++i];
            /* Overrides the mode's default match-finder chain depth. Higher
             * = better ratio, slower encode (smooth monotonic tradeoff).
             * 0 keeps the per-mode default. Reject out-of-range rather than
             * silently clamping so the value is never misread. */
            if (!parse_number(value, 4096, &depth_override)) {
                fprintf(stderr, "Invalid -D/--depth %s: must be 1..4096, "
                        "or 0 for the mode default\n", value);
                return 1;
            }
        }
        else if ((strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--accel") == 0)
                 && i + 1 < argc) {
            const char *value = argv[++i];
            /* Position-skip acceleration: speeds up encode on incompressible
             * input for a small ratio cost on compressible data. Zero selects
             * the mode-dependent automatic factor. Reject out-of-range rather
             * silently clamping. */
            if (!parse_number(value, 64, &accel)) {
                fprintf(stderr, "Invalid -A/--accel %s: must be 0..64 "
                        "(0 = automatic)\n", value);
                return 1;
            }
        }
        else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--window") == 0)
                 && i + 1 < argc) {
            const char *value = argv[++i];
            /* Valid window logs are 10..24 (1 KiB .. 16 MiB). The 16 MiB
             * cap is the 3-byte (24-bit) offset wire-format limit. 0 keeps
             * the per-mode adaptive default. Reject anything else rather
             * than silently clamping, so the user knows their value was
             * out of range. */
            if (!parse_number(value, 24, &window_log) ||
                (window_log != 0 && window_log < 10)) {
                fprintf(stderr, "Invalid -w/--window %s: must be 10..24 "
                        "(1 KiB .. 16 MiB), or 0 for auto\n", value);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-T") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (!parse_number(value, INT_MAX, &nthreads)) {
                fprintf(stderr, "Invalid -T %s: must be 0..%d\n", value, INT_MAX);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-h") == 0) { usage(); return 0; }
        else if (argv[i][0] != '-') {
            if (input_path) {
                fprintf(stderr, "Only one input file is supported\n"); return 1;
            }
            input_path = argv[i];
        }
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }

    if (!input_path || do_compress + do_decompress + do_test + do_bench != 1) {
        usage();
        return 1;
    }
    vv_mode_t mode = parse_mode(mode_str);
    if (mode != VV_MODE_ULTRA_FAST && mode != VV_MODE_BALANCED &&
        mode != VV_MODE_EXTREME) {
        fprintf(stderr, "Unknown compression mode: %s\n", mode_str); return 1;
    }
    if (filter_x86 && filter_arm64) {
        fprintf(stderr, "Cannot combine --filter x86 and --filter arm64\n");
        return 1;
    }

    size_t input_len = 0;
    uint8_t *input_data = read_file(input_path, &input_len);
    if (!input_data) return 1;

    /* ─── COMPRESS ─── */
    if (do_compress) {
        vv_options_t opts;
        vv_default_options(&opts);
        opts.mode = mode;
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
        opts.filter_x86 = filter_x86;
        opts.filter_arm64 = filter_arm64;
        opts.filter_auto = filter_auto;
        opts.depth_override = (uint32_t)depth_override;
        opts.accel = (uint32_t)accel;
        opts.no_rep = no_rep;

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
            snprintf(out_buf, sizeof(out_buf), "%s.zupt", input_path);
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
                /* Strip a known compressed suffix (.zupt, or legacy .vv) if
                 * present; otherwise append .orig so output never overwrites
                 * the input. */
                size_t ilen = strlen(input_path);
                if (ilen > 5 && strcmp(input_path + ilen - 5, ".zupt") == 0) {
                    snprintf(out_buf, sizeof(out_buf), "%.*s",
                             (int)(ilen - 5), input_path);
                } else if (ilen > 3 && strcmp(input_path + ilen - 3, ".vv") == 0) {
                    snprintf(out_buf, sizeof(out_buf), "%.*s",
                             (int)(ilen - 3), input_path);
                } else {
                    snprintf(out_buf, sizeof(out_buf), "%s.orig", input_path);
                }
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
