/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Compare public one-shot APIs on small, deterministic inputs. This is a
 * userspace microbenchmark, not a kernel, zram, or filesystem benchmark.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "vaptvupt.h"
#include <lz4.h>
#include <zstd.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#ifndef VV_BENCH_BUILD_FLAGS
#define VV_BENCH_BUILD_FLAGS "not recorded; retain the build command"
#endif

enum { MAX_INPUT = 65536, CODECS = 5, FIXTURES = 4, MAX_SAMPLES = 31 };
static const char *const codec_names[] = {
    "vv-fast", "vv-balanced", "lz4-default", "zstd-1", "zstd-3"
};
static const char *const fixture_names[] = {
    "text", "records", "random", "repeating"
};

struct config {
    size_t size;
    size_t samples;
    size_t min_ms;
    int fixture;
    int codec;
    int self_test;
};

struct buffers {
    unsigned char *input;
    unsigned char *compressed;
    unsigned char *reference;
    unsigned char *decoded;
    size_t size;
    size_t capacity;
    size_t compressed_size;
    int codec;
    vv_options_t options;
};

struct result {
    size_t compressed_size;
    size_t encode_iterations;
    size_t decode_iterations;
    double encode_seconds;
    double decode_seconds;
};

static double seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void make_fixture(unsigned char *dst, size_t size, int fixture)
{
    static const char *const words[] = {
        "page ", "memory ", "request ", "offset ", "worker ", "read ",
        "write ", "compression ", "buffer ", "record ", "cache ", "linux "
    };
    uint32_t state = UINT32_C(0x6d2b79f5);
    size_t pos = 0;

    while (pos < size) {
        uint32_t value = next_random(&state);
        if (fixture == 0) {
            const char *word = words[value % (sizeof(words) / sizeof(words[0]))];
            size_t length = strlen(word);
            if (length > size - pos)
                length = size - pos;
            memcpy(dst + pos, word, length);
            pos += length;
        } else if (fixture == 1) {
            /* Fixed-size records mix counters and repeated field names with
             * an unpredictable payload; they are not captured kernel pages. */
            unsigned char record[64] = {0};
            static const unsigned char label[] = "worker:ready;page:resident;";
            uint32_t id = (uint32_t)(pos / sizeof(record));
            memcpy(record, label, sizeof(label) - 1);
            for (size_t i = 0; i < 4; i++)
                record[28 + i] = (unsigned char)(id >> (i * 8));
            record[32] = (unsigned char)(id % 8);
            for (size_t i = 40; i < sizeof(record); i++)
                record[i] = (unsigned char)next_random(&state);
            size_t length = sizeof(record);
            if (length > size - pos)
                length = size - pos;
            memcpy(dst + pos, record, length);
            pos += length;
        } else if (fixture == 2) {
            dst[pos++] = (unsigned char)value;
        } else {
            dst[pos] = (unsigned char)(pos % 43);
            pos++;
        }
    }
}

static uint64_t fingerprint(const unsigned char *src, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ src[i]) * UINT64_C(1099511628211);
    return hash;
}

static size_t encode(struct buffers *b)
{
    if (b->codec < 2) {
        int64_t size = vv_compress(b->input, b->size, b->compressed,
                                   b->capacity, &b->options);
        return size > 0 ? (size_t)size : 0;
    }
    if (b->codec == 2) {
        int size = LZ4_compress_default((const char *)b->input,
                                       (char *)b->compressed, (int)b->size,
                                       (int)b->capacity);
        return size > 0 ? (size_t)size : 0;
    }
    size_t size = ZSTD_compress(b->compressed, b->capacity, b->input,
                               b->size, b->codec == 3 ? 1 : 3);
    return ZSTD_isError(size) ? 0 : size;
}

static size_t decode(struct buffers *b)
{
    if (b->codec < 2) {
        int64_t size = vv_decompress(b->compressed, b->compressed_size,
                                     b->decoded, b->size);
        return size >= 0 ? (size_t)size : SIZE_MAX;
    }
    if (b->codec == 2) {
        int size = LZ4_decompress_safe((const char *)b->compressed,
                                      (char *)b->decoded,
                                      (int)b->compressed_size, (int)b->size);
        return size >= 0 ? (size_t)size : SIZE_MAX;
    }
    size_t size = ZSTD_decompress(b->decoded, b->size, b->compressed,
                                 b->compressed_size);
    return ZSTD_isError(size) ? SIZE_MAX : size;
}

static int verify(struct buffers *b)
{
    if (memcmp(b->compressed, b->reference, b->compressed_size) != 0 ||
        decode(b) != b->size || memcmp(b->input, b->decoded, b->size) != 0) {
        fprintf(stderr, "%s: compressed bytes changed or roundtrip failed\n",
                codec_names[b->codec]);
        return 0;
    }
    return 1;
}

static int batch(struct buffers *b, int decoding, size_t iterations,
                 double *elapsed)
{
    size_t expected = decoding ? b->size : b->compressed_size;
    double start = seconds();
    for (size_t i = 0; i < iterations; i++) {
        size_t actual = decoding ? decode(b) : encode(b);
        if (actual != expected) {
            fprintf(stderr, "%s: %s returned an unexpected size\n",
                    codec_names[b->codec], decoding ? "decode" : "encode");
            return 0;
        }
    }
    *elapsed = seconds() - start;
    /* Keep byte comparisons outside the timed loop. Every call checks its
     * return value; each completed batch checks its final output bytes. */
    if (decoding && memcmp(b->input, b->decoded, b->size) != 0) {
        fprintf(stderr, "%s: decoded bytes changed\n", codec_names[b->codec]);
        return 0;
    }
    return verify(b);
}

static int calibrate(struct buffers *b, int decoding, double minimum,
                     size_t *iterations)
{
    double elapsed;
    *iterations = 1;
    for (;;) {
        if (!batch(b, decoding, *iterations, &elapsed))
            return 0;
        if (elapsed >= minimum)
            return 1;
        if (*iterations > SIZE_MAX / 2) {
            fprintf(stderr, "calibration iteration overflow\n");
            return 0;
        }
        *iterations *= 2;
    }
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static int measure(struct buffers *b, const struct config *cfg,
                   struct result *result)
{
    double encoded[MAX_SAMPLES], decoded[MAX_SAMPLES];
    b->compressed_size = encode(b);
    if (b->compressed_size == 0 || b->compressed_size > b->capacity) {
        fprintf(stderr, "%s: initial compression failed\n", codec_names[b->codec]);
        return 0;
    }
    memcpy(b->reference, b->compressed, b->compressed_size);
    if (!verify(b))
        return 0;
    result->compressed_size = b->compressed_size;
    if (cfg->self_test)
        return 1;

    /* Caller buffers are reused, but no codec context is reused. Allocations
     * performed inside the public one-shot APIs remain in the measurement. */
    for (size_t warmup = 0; warmup < 3; warmup++) {
        double ignored;
        if (!batch(b, 0, 1, &ignored) || !batch(b, 1, 1, &ignored))
            return 0;
    }
    double minimum = (double)cfg->min_ms / 1000.0;
    if (!calibrate(b, 0, minimum, &result->encode_iterations) ||
        !calibrate(b, 1, minimum, &result->decode_iterations))
        return 0;

    for (size_t sample = 0; sample < cfg->samples; sample++) {
        /* Alternate encode/decode order to avoid always warming one with the
         * other. Calibration batches are discarded from both medians. */
        for (int step = 0; step < 2; step++) {
            int decoding = (step + (int)(sample % 2)) % 2;
            size_t iterations = decoding ? result->decode_iterations :
                                           result->encode_iterations;
            double elapsed;
            if (!batch(b, decoding, iterations, &elapsed))
                return 0;
            if (decoding)
                decoded[sample] = elapsed / (double)iterations;
            else
                encoded[sample] = elapsed / (double)iterations;
        }
    }
    qsort(encoded, cfg->samples, sizeof(encoded[0]), compare_double);
    qsort(decoded, cfg->samples, sizeof(decoded[0]), compare_double);
    result->encode_seconds = encoded[cfg->samples / 2];
    result->decode_seconds = decoded[cfg->samples / 2];
    return 1;
}

static int parse_number(const char *text, size_t maximum, size_t *out)
{
    size_t value = 0;
    if (!*text)
        return 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9')
            return 0;
        unsigned digit = (unsigned)(*text - '0');
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10))
            return 0;
        value = value * 10 + digit;
    }
    *out = value;
    return 1;
}

static int lookup(const char *name, const char *const *names, int count)
{
    for (int i = 0; i < count; i++)
        if (strcmp(name, names[i]) == 0)
            return i;
    return -1;
}

static void usage(FILE *out, const char *program)
{
    fprintf(out,
            "usage: %s [--self-test] [--size BYTES] [--samples N] [--min-ms N]\n"
            "       [--fixture text|records|random|repeating]\n"
            "       [--codec vv-fast|vv-balanced|lz4-default|zstd-1|zstd-3]\n"
            "Defaults: all fixtures/codecs, 4096/16384/65536 bytes, 7 samples,\n"
            "50 ms calibration minimum, 3 warmup calls per operation.\n"
            "BYTES: 1..65536; samples: odd, 3..31; min-ms: 50..10000.\n"
            "--self-test verifies outputs without collecting timings.\n",
            program);
}

int main(int argc, char **argv)
{
    struct config cfg = {0, 7, 50, -1, -1, 0};
    unsigned seen = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        unsigned option;
        if (strcmp(arg, "--help") == 0 && argc == 2) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(arg, "--self-test") == 0) option = 1;
        else if (strcmp(arg, "--size") == 0) option = 2;
        else if (strcmp(arg, "--samples") == 0) option = 4;
        else if (strcmp(arg, "--min-ms") == 0) option = 8;
        else if (strcmp(arg, "--fixture") == 0) option = 16;
        else if (strcmp(arg, "--codec") == 0) option = 32;
        else goto bad_args;
        if (seen & option)
            goto bad_args;
        seen |= option;
        if (option == 1) {
            cfg.self_test = 1;
            continue;
        }
        if (++i == argc)
            goto bad_args;
        if (option == 2) {
            if (!parse_number(argv[i], MAX_INPUT, &cfg.size) || cfg.size == 0)
                goto bad_args;
        } else if (option == 4) {
            if (!parse_number(argv[i], MAX_SAMPLES, &cfg.samples) ||
                cfg.samples < 3 || cfg.samples % 2 == 0)
                goto bad_args;
        } else if (option == 8) {
            if (!parse_number(argv[i], 10000, &cfg.min_ms) || cfg.min_ms < 50)
                goto bad_args;
        } else if (option == 16) {
            cfg.fixture = lookup(argv[i], fixture_names, FIXTURES);
            if (cfg.fixture < 0)
                goto bad_args;
        } else {
            cfg.codec = lookup(argv[i], codec_names, CODECS);
            if (cfg.codec < 0)
                goto bad_args;
        }
    }

    struct utsname host;
    printf("# suite=pages-v1; environment=userspace; api=one-shot; timer=CLOCK_MONOTONIC\n");
    printf("# vv=%s; lz4=%s; zstd=%s; compiler=%s\n", VV_VERSION_STRING,
           LZ4_versionString(), ZSTD_versionString(), __VERSION__);
    printf("# build_flags=%s\n", VV_BENCH_BUILD_FLAGS);
    if (uname(&host) == 0)
        printf("# system=%s %s; machine=%s\n", host.sysname, host.release, host.machine);
    printf("# fixtures=synthetic; seed=0x6d2b79f5; repeated_input=cache-warm; not_kernel_results=true\n");
    printf("# caller_buffer_allocation=excluded; internal_API_allocations=included; reused_contexts=none\n");
    printf("# framing=VV-v1-and-Zstd-frames,LZ4-raw-blocks; checksums=VV-on,LZ4-none,Zstd-default-off\n");
    printf("# vv_options=defaults-except-mode; zstd_levels=1,3; lz4=LZ4_compress_default\n");
    printf("# rate=uncompressed_decimal_MB/s; ratio=input_bytes/compressed_bytes; validation=outside_timed_batches\n");
    printf("# samples=%zu; statistic=median; calibration_min_ms=%zu; warmup_calls=3; self_test=%d\n",
           cfg.samples, cfg.min_ms, cfg.self_test);
    printf("fixture,input_bytes,input_fnv1a64,codec,compressed_bytes,ratio,encode_MBps,decode_MBps,encode_iterations,decode_iterations\n");

    size_t sizes[] = {4096, 16384, 65536};
    size_t size_count = sizeof(sizes) / sizeof(sizes[0]);
    if (cfg.size) {
        sizes[0] = cfg.size;
        size_count = 1;
    }
    for (size_t s = 0; s < size_count; s++) {
        struct buffers b = {0};
        b.size = sizes[s];
        b.capacity = vv_compress_bound(b.size);
        size_t zstd_bound = ZSTD_compressBound(b.size);
        size_t lz4_bound = (size_t)LZ4_compressBound((int)b.size);
        if (zstd_bound > b.capacity) b.capacity = zstd_bound;
        if (lz4_bound > b.capacity) b.capacity = lz4_bound;
        b.input = malloc(b.size);
        b.compressed = malloc(b.capacity);
        b.reference = malloc(b.capacity);
        b.decoded = malloc(b.size);
        int success = b.input && b.compressed && b.reference && b.decoded;
        if (!success)
            fprintf(stderr, "benchmark buffer allocation failed\n");
        for (int fixture = 0; success && fixture < FIXTURES; fixture++) {
            struct result results[CODECS] = {{0}};
            if (cfg.fixture >= 0 && cfg.fixture != fixture)
                continue;
            make_fixture(b.input, b.size, fixture);
            uint64_t hash = fingerprint(b.input, b.size);
            /* Rotate codec order across inputs; keep the CSV order stable. */
            for (int index = 0; success && index < CODECS; index++) {
                b.codec = (index + fixture + (int)s) % CODECS;
                if (cfg.codec >= 0 && cfg.codec != b.codec)
                    continue;
                vv_default_options(&b.options);
                b.options.mode = b.codec == 0 ? VV_MODE_ULTRA_FAST : VV_MODE_BALANCED;
                success = measure(&b, &cfg, &results[b.codec]);
            }
            if (!success)
                break;
            for (int codec = 0; codec < CODECS; codec++) {
                if (cfg.codec >= 0 && cfg.codec != codec)
                    continue;
                const struct result *r = &results[codec];
                printf("%s,%zu,%016" PRIx64 ",%s,%zu,%.6f,", fixture_names[fixture],
                       b.size, hash, codec_names[codec], r->compressed_size,
                       (double)b.size / (double)r->compressed_size);
                if (cfg.self_test)
                    printf("NA,NA,0,0\n");
                else
                    printf("%.3f,%.3f,%zu,%zu\n", (double)b.size / r->encode_seconds / 1e6,
                           (double)b.size / r->decode_seconds / 1e6,
                           r->encode_iterations, r->decode_iterations);
            }
            fflush(stdout);
        }
        free(b.input);
        free(b.compressed);
        free(b.reference);
        free(b.decoded);
        if (!success)
            return 1;
    }
    return 0;

bad_args:
    usage(stderr, argv[0]);
    return 2;
}
