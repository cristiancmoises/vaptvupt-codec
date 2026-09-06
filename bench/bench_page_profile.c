/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Userspace page profiles. Individual calls provide latency samples; separate
 * batches provide throughput. Neither is a kernel or consumer benchmark.
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
#include <time.h>

enum { MAX_PAGE = 65536, MAX_PAGES = 256, MAX_ITERS = 1048576 };
enum api { VV_ONESHOT, VV_CONTEXT, LZ4_ONESHOT, LZ4_STATE, ZSTD_ONESHOT, ZSTD_CONTEXT };
struct variant { const char *name; enum api api; int level; int checksum; };
static const struct variant variants[] = {
    {"vv-oneshot-c0", VV_ONESHOT, 0, 0},
    {"vv-oneshot-c1", VV_ONESHOT, 0, 1},
    {"vv-context-c0", VV_CONTEXT, 0, 0},
    {"vv-context-c1", VV_CONTEXT, 0, 1},
    {"lz4-default", LZ4_ONESHOT, 0, 0},
    {"lz4-extstate", LZ4_STATE, 0, 0},
    {"zstd-oneshot-1-c0", ZSTD_ONESHOT, 1, 0},
    {"zstd-oneshot-3-c0", ZSTD_ONESHOT, 3, 0},
    {"zstd-context-1-c0", ZSTD_CONTEXT, 1, 0},
    {"zstd-context-1-c1", ZSTD_CONTEXT, 1, 1},
    {"zstd-context-3-c0", ZSTD_CONTEXT, 3, 0},
    {"zstd-context-3-c1", ZSTD_CONTEXT, 3, 1}
};
static const char *const fixtures[] = {"text", "records", "random", "repeating", "zero", "same-filled"};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

struct config {
    size_t size, pages, samples, batch_samples, min_batch_ms;
    int fixture, variant, self_test;
};
struct codec {
    const struct variant *variant;
    vv_options_t opts;
    void *storage;
    vv_fast_context_t *vv;
    ZSTD_CCtx *zc;
    ZSTD_DCtx *zd;
    size_t storage_size;
};
struct cohort {
    unsigned char *input, *compressed, *scratch, *decoded;
    size_t *lengths, *framing;
    uint64_t *hashes;
    size_t size, pages, capacity;
    int fixture;
};

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        perror("FAIL: clock_gettime");
        exit(1);
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

static uint32_t random32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return x;
}

static uint64_t fingerprint(const unsigned char *p, size_t n)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; i++)
        hash = (hash ^ p[i]) * UINT64_C(1099511628211);
    return hash;
}

static void make_page(unsigned char *p, size_t n, int fixture, size_t page)
{
    static const char *const words[] = {
        "page ", "memory ", "request ", "offset ", "worker ", "read ",
        "write ", "compression ", "buffer ", "record ", "cache ", "linux "
    };
    uint32_t state = UINT32_C(0x6d2b79f5) ^ (uint32_t)page * UINT32_C(0x9e3779b9);
    if (!state) state = 1;
    if (fixture >= 4) {
        memset(p, fixture == 4 ? 0 : 0x5a, n);
        return;
    }
    for (size_t pos = 0; pos < n;) {
        uint32_t x = random32(&state);
        if (fixture == 0) {
            const char *word = words[x % COUNT(words)];
            size_t length = strlen(word);
            if (length > n - pos) length = n - pos;
            memcpy(p + pos, word, length);
            pos += length;
        } else if (fixture == 1) {
            unsigned char record[64] = {0};
            static const char label[] = "worker:ready;page:resident;";
            uint32_t id = (uint32_t)(pos / sizeof(record) + page * 1024);
            memcpy(record, label, sizeof(label) - 1);
            for (size_t i = 0; i < 4; i++) record[28 + i] = (unsigned char)(id >> (i * 8));
            record[32] = (unsigned char)(id % 8);
            for (size_t i = 40; i < sizeof(record); i++) record[i] = (unsigned char)random32(&state);
            size_t length = sizeof(record);
            if (length > n - pos) length = n - pos;
            memcpy(p + pos, record, length);
            pos += length;
        } else if (fixture == 2) {
            p[pos++] = (unsigned char)x;
        } else {
            p[pos] = (unsigned char)((pos + page) % 43);
            pos++;
        }
    }
}

static void release_codec(struct codec *c)
{
    free(c->storage);
    ZSTD_freeCCtx(c->zc);
    ZSTD_freeDCtx(c->zd);
    memset(c, 0, sizeof(*c));
}

static int init_codec(struct codec *c, const struct variant *v, size_t n)
{
    memset(c, 0, sizeof(*c));
    c->variant = v;
    vv_default_options(&c->opts);
    c->opts.mode = VV_MODE_ULTRA_FAST;
    c->opts.checksum = v->checksum;
    if (v->api == VV_CONTEXT) {
        size_t alignment = vv_fast_context_alignment();
        c->storage_size = vv_fast_context_size(n);
        if (!alignment || (alignment & (alignment - 1)) || !c->storage_size)
            return 0;
        size_t rounded = (c->storage_size + alignment - 1) & ~(alignment - 1);
        c->storage = aligned_alloc(alignment, rounded);
        if (!c->storage || vv_fast_context_init(c->storage, c->storage_size, n,
                                               &c->opts, &c->vv) != VV_OK)
            goto fail;
    } else if (v->api == LZ4_STATE) {
        c->storage_size = (size_t)LZ4_sizeofState();
        c->storage = malloc(c->storage_size);
        if (!c->storage) goto fail;
    } else if (v->api == ZSTD_CONTEXT) {
        c->zc = ZSTD_createCCtx();
        c->zd = ZSTD_createDCtx();
        if (!c->zc || !c->zd ||
            ZSTD_isError(ZSTD_CCtx_setParameter(c->zc, ZSTD_c_compressionLevel, v->level)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(c->zc, ZSTD_c_checksumFlag, v->checksum)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(c->zc, ZSTD_c_nbWorkers, 0))) goto fail;
    }
    return 1;
fail:
    release_codec(c);
    return 0;
}

static size_t encode(struct codec *c, const unsigned char *src, size_t n,
                     unsigned char *dst, size_t capacity)
{
    int64_t vv_size;
    int lz4_size;
    size_t zstd_size;
    switch (c->variant->api) {
    case VV_ONESHOT:
        vv_size = vv_compress(src, n, dst, capacity, &c->opts);
        return vv_size > 0 ? (size_t)vv_size : 0;
    case VV_CONTEXT:
        vv_size = vv_fast_context_compress(c->vv, src, n, dst, capacity);
        return vv_size > 0 ? (size_t)vv_size : 0;
    case LZ4_ONESHOT:
        lz4_size = LZ4_compress_default((const char *)src, (char *)dst, (int)n, (int)capacity);
        return lz4_size > 0 ? (size_t)lz4_size : 0;
    case LZ4_STATE:
        lz4_size = LZ4_compress_fast_extState(c->storage, (const char *)src,
                                            (char *)dst, (int)n, (int)capacity, 1);
        return lz4_size > 0 ? (size_t)lz4_size : 0;
    case ZSTD_ONESHOT:
        zstd_size = ZSTD_compress(dst, capacity, src, n, c->variant->level);
        return ZSTD_isError(zstd_size) ? 0 : zstd_size;
    case ZSTD_CONTEXT:
        /* compress2 starts an independent frame while retaining parameters
         * and allocated capacity. It does not reuse the previous page history. */
        zstd_size = ZSTD_compress2(c->zc, dst, capacity, src, n);
        return ZSTD_isError(zstd_size) ? 0 : zstd_size;
    }
    return 0;
}

static size_t decode(struct codec *c, const unsigned char *src, size_t n,
                     unsigned char *dst, size_t capacity)
{
    if (c->variant->api <= VV_CONTEXT) {
        int64_t size = vv_decompress(src, n, dst, capacity);
        return size >= 0 ? (size_t)size : SIZE_MAX;
    }
    if (c->variant->api <= LZ4_STATE) {
        int size = LZ4_decompress_safe((const char *)src, (char *)dst, (int)n, (int)capacity);
        return size >= 0 ? (size_t)size : SIZE_MAX;
    }
    size_t size = c->variant->api == ZSTD_CONTEXT ?
        ZSTD_decompressDCtx(c->zd, dst, capacity, src, n) : ZSTD_decompress(dst, capacity, src, n);
    return ZSTD_isError(size) ? SIZE_MAX : size;
}

/* Account for container/block headers only. This never decodes payloads and
 * does not claim that subtracting framing makes different token formats equal. */
static size_t framing_bytes(const struct codec *c, const unsigned char *p, size_t n, size_t raw)
{
    if (c->variant->api <= VV_CONTEXT) {
        if (n < 20) return SIZE_MAX;
        uint32_t bh;
        memcpy(&bh, p + 16, sizeof(bh));
        if (!vv_bh_last(bh) || vv_bh_size(bh) != raw) return SIZE_MAX;
        size_t footer = p[5] & 1 ? sizeof(vv_frame_footer_t) : 0;
        size_t header = 20, payload;
        if (vv_bh_type(bh) == VV_BLOCK_RAW) payload = raw;
        else if (vv_bh_type(bh) == VV_BLOCK_RLE) payload = 1;
        else if (vv_bh_type(bh) == VV_BLOCK_COMPRESSED && n >= 23) {
            header += 3;
            payload = (size_t)p[20] | (size_t)p[21] << 8 | (size_t)p[22] << 16;
        } else return SIZE_MAX;
        return header + payload + footer == n ? header + footer : SIZE_MAX;
    }
    if (c->variant->api <= LZ4_STATE) return 0;
    /* Zstd format v0.4.3: frame descriptor, optional window/dictionary/FCS,
     * three-byte block headers, and an optional four-byte content checksum.
     * https://github.com/facebook/zstd/blob/v1.5.7/doc/zstd_compression_format.md */
    if (n < 6 || p[0] != 0x28 || p[1] != 0xb5 || p[2] != 0x2f || p[3] != 0xfd)
        return SIZE_MAX;
    unsigned desc = p[4], single = (desc >> 5) & 1, fcs = desc >> 6;
    static const unsigned dict_size[] = {0, 1, 2, 4};
    size_t pos = 5 + !single + dict_size[desc & 3] + (fcs ? (1u << fcs) : single);
    size_t overhead = pos, footer = desc & 4 ? 4 : 0;
    if (pos > n) return SIZE_MAX;
    for (;;) {
        if (n - pos < 3) return SIZE_MAX;
        uint32_t bh = (uint32_t)p[pos] | (uint32_t)p[pos + 1] << 8 | (uint32_t)p[pos + 2] << 16;
        unsigned type = (bh >> 1) & 3;
        if (type == 3) return SIZE_MAX;
        size_t payload = type == 1 ? 1 : bh >> 3;
        pos += 3; overhead += 3;
        if (payload > n - pos) return SIZE_MAX;
        pos += payload;
        if (bh & 1) break;
    }
    return n - pos == footer ? overhead + footer : SIZE_MAX;
}

static void free_cohort(struct cohort *b)
{
    free(b->input); free(b->compressed); free(b->scratch); free(b->decoded);
    free(b->lengths); free(b->framing); free(b->hashes);
}

static int make_cohort(struct cohort *b, size_t n, size_t pages, int fixture)
{
    memset(b, 0, sizeof(*b));
    b->size = n; b->pages = pages; b->fixture = fixture;
    b->capacity = vv_compress_bound(n);
    if (ZSTD_compressBound(n) > b->capacity) b->capacity = ZSTD_compressBound(n);
    if ((size_t)LZ4_compressBound((int)n) > b->capacity) b->capacity = (size_t)LZ4_compressBound((int)n);
    b->input = malloc(n * pages);
    b->compressed = malloc(b->capacity * pages);
    b->scratch = malloc(b->capacity);
    b->decoded = malloc(n);
    b->lengths = calloc(pages, sizeof(*b->lengths));
    b->framing = calloc(pages, sizeof(*b->framing));
    b->hashes = calloc(pages, sizeof(*b->hashes));
    if (!b->input || !b->compressed || !b->scratch || !b->decoded ||
        !b->lengths || !b->framing || !b->hashes) return 0;
    for (size_t page = 0; page < pages; page++) {
        make_page(b->input + page * n, n, fixture, page);
        b->hashes[page] = fingerprint(b->input + page * n, n);
    }
    return 1;
}

static int prepare(struct codec *c, struct cohort *b)
{
    for (size_t page = 0; page < b->pages; page++) {
        unsigned char *src = b->input + page * b->size;
        unsigned char *comp = b->compressed + page * b->capacity;
        size_t size = encode(c, src, b->size, comp, b->capacity);
        if (!size || size > b->capacity || decode(c, comp, size, b->decoded, b->size) != b->size ||
            memcmp(src, b->decoded, b->size)) return 0;
        b->lengths[page] = size;
        b->framing[page] = framing_bytes(c, comp, size, b->size);
        if (b->framing[page] == SIZE_MAX) return 0;
        if (c->variant->api == VV_CONTEXT) {
            int64_t ordinary = vv_compress(src, b->size, b->scratch, b->capacity, &c->opts);
            if (ordinary != (int64_t)size || memcmp(comp, b->scratch, size)) return 0;
        }
    }
    return 1;
}

static size_t context_bytes(const struct codec *c)
{
    return c->storage_size + (c->zc ? ZSTD_sizeof_CCtx(c->zc) : 0) +
           (c->zd ? ZSTD_sizeof_DCtx(c->zd) : 0);
}

static void status_row(const struct codec *c, const struct cohort *b, const char *status,
                       const char *phase, size_t sample, size_t page,
                       size_t iterations, uint64_t elapsed)
{
    size_t size = b->lengths[page], framing = b->framing[page];
    printf("%s,%s,%zu,%zu,%016" PRIx64 ",%d,%s,%d,%s,%zu,%zu,%" PRIu64
           ",%.3f,%zu,%zu,%zu,%d,%zu\n", status, fixtures[b->fixture], b->size,
           page, b->hashes[page], b->fixture >= 4, c->variant->name,
           c->variant->checksum, phase, sample, iterations, elapsed,
           (double)elapsed / (double)iterations, size, framing, size - framing,
           size >= b->size, context_bytes(c));
}

static void row(const struct codec *c, const struct cohort *b, const char *phase,
                size_t sample, size_t page, size_t iterations, uint64_t elapsed)
{
    status_row(c, b, "PASS", phase, sample, page, iterations, elapsed);
}

static int one_call(struct codec *c, struct cohort *b, size_t page, int decoding, uint64_t *elapsed)
{
    const unsigned char *src = b->input + page * b->size;
    const unsigned char *comp = b->compressed + page * b->capacity;
    size_t expected = decoding ? b->size : b->lengths[page];
    memset(b->decoded, 0xa5, b->size);
    uint64_t start = now_ns();
    size_t actual = decoding ? decode(c, comp, b->lengths[page], b->decoded, b->size) :
                              encode(c, src, b->size, b->scratch, b->capacity);
    *elapsed = now_ns() - start;
    return actual == expected && (decoding ? !memcmp(src, b->decoded, b->size) :
                                                !memcmp(comp, b->scratch, actual));
}

static int batch(struct codec *c, struct cohort *b, int decoding, size_t count, uint64_t *elapsed)
{
    uint64_t start = now_ns();
    for (size_t i = 0; i < count; i++) {
        size_t page = i % b->pages;
        size_t actual = decoding ?
            decode(c, b->compressed + page * b->capacity, b->lengths[page], b->decoded, b->size) :
            encode(c, b->input + page * b->size, b->size, b->scratch, b->capacity);
        if (actual != (decoding ? b->size : b->lengths[page])) return 0;
    }
    *elapsed = now_ns() - start;
    size_t last = (count - 1) % b->pages;
    return decoding ? !memcmp(b->input + last * b->size, b->decoded, b->size) :
                      !memcmp(b->compressed + last * b->capacity, b->scratch, b->lengths[last]);
}

static int measure(struct codec *c, struct cohort *b, const struct config *cfg)
{
    if (!prepare(c, b)) return 0;
    for (size_t page = 0; page < b->pages; page++) row(c, b, "size", 0, page, 1, 0);
    if (cfg->self_test) return 1;
    for (size_t warm = 0; warm < 3; warm++) {
        uint64_t ignored;
        if (!one_call(c, b, warm % b->pages, 0, &ignored) ||
            !one_call(c, b, warm % b->pages, 1, &ignored)) return 0;
    }
    for (size_t sample = 0; sample < cfg->samples; sample++) {
        size_t page = sample % b->pages;
        for (int step = 0; step < 2; step++) {
            int decoding = (step + (int)(sample & 1)) & 1;
            uint64_t elapsed;
            if (!one_call(c, b, page, decoding, &elapsed)) return 0;
            row(c, b, decoding ? "decode_call" : "encode_call", sample, page, 1, elapsed);
        }
    }
    for (int decoding = 0; decoding < 2; decoding++) {
        size_t count = 1;
        uint64_t elapsed;
        for (;;) {
            if (!batch(c, b, decoding, count, &elapsed)) return 0;
            if (elapsed >= cfg->min_batch_ms * UINT64_C(1000000)) break;
            if (count >= MAX_ITERS) {
                fprintf(stderr, "NOT RUN: %s %s batch calibration reached %u iterations\n",
                        c->variant->name, decoding ? "decode" : "encode", MAX_ITERS);
                status_row(c, b, "NOT_RUN", decoding ? "decode_batch" : "encode_batch",
                           0, (count - 1) % b->pages, count, elapsed);
                count = 0;
                break;
            }
            count *= 2;
        }
        if (!count) continue;
        for (size_t sample = 0; sample < cfg->batch_samples; sample++) {
            if (!batch(c, b, decoding, count, &elapsed)) return 0;
            row(c, b, decoding ? "decode_batch" : "encode_batch", sample,
                (count - 1) % b->pages, count, elapsed);
        }
    }
    if (c->variant->api == VV_CONTEXT || c->variant->api == LZ4_STATE || c->variant->api == ZSTD_CONTEXT) {
        for (size_t sample = 0; sample < cfg->samples; sample++) {
            struct codec fresh;
            uint64_t start = now_ns();
            int ok = init_codec(&fresh, c->variant, b->size);
            uint64_t elapsed = now_ns() - start;
            if (!ok) return 0;
            row(&fresh, b, "context_setup", sample, 0, 1, elapsed);
            size_t page = sample % b->pages;
            if (!one_call(&fresh, b, page, 0, &elapsed)) { release_codec(&fresh); return 0; }
            row(&fresh, b, "first_encode_call", sample, page, 1, elapsed);
            if (!one_call(&fresh, b, page, 1, &elapsed)) { release_codec(&fresh); return 0; }
            row(&fresh, b, "first_decode_call", sample, page, 1, elapsed);
            release_codec(&fresh);
            if (c->variant->api == VV_CONTEXT) {
                start = now_ns();
                ok = vv_fast_context_init(c->storage, c->storage_size, b->size,
                                          &c->opts, &c->vv) == VV_OK;
                elapsed = now_ns() - start;
                if (!ok) return 0;
                row(c, b, "vv_init_preallocated", sample, 0, 1, elapsed);
            }
        }
    }
    return 1;
}

static int number(const char *s, size_t max, size_t *out)
{
    size_t n = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        unsigned digit = (unsigned)(*s - '0');
        if (n > max / 10 || (n == max / 10 && digit > max % 10)) return 0;
        n = n * 10 + digit;
    }
    *out = n;
    return 1;
}

static void usage(FILE *out, const char *name)
{
    fprintf(out, "usage: %s [--self-test] [--size 4096|16384|65536] [--pages 1..256]\n"
                 "  [--samples 101..10001] [--batch-samples 3..31] [--min-batch-ms 1..100]\n"
                 "  [--fixture text|records|random|repeating|zero|same-filled] [--codec NAME]\n"
                 "Defaults: all sizes/fixtures/codecs;64 pages;101 individual samples;\n"
                 "7 batch samples;10ms minimum calibration;maximum1048576 iterations.\n", name);
    for (size_t i = 0; i < COUNT(variants); i++) fprintf(out, "  %s\n", variants[i].name);
}

int main(int argc, char **argv)
{
    struct config cfg = {0, 64, 101, 7, 10, -1, -1, 0};
    unsigned seen = 0;
    for (int i = 1; i < argc; i++) {
        unsigned bit = 0;
        if (!strcmp(argv[i], "--help") && argc == 2) { usage(stdout, argv[0]); return 0; }
        if (!strcmp(argv[i], "--self-test")) bit = 1;
        else if (!strcmp(argv[i], "--size")) bit = 2;
        else if (!strcmp(argv[i], "--pages")) bit = 4;
        else if (!strcmp(argv[i], "--samples")) bit = 8;
        else if (!strcmp(argv[i], "--batch-samples")) bit = 16;
        else if (!strcmp(argv[i], "--min-batch-ms")) bit = 32;
        else if (!strcmp(argv[i], "--fixture")) bit = 64;
        else if (!strcmp(argv[i], "--codec")) bit = 128;
        else goto bad_args;
        if (seen & bit) goto bad_args;
        seen |= bit;
        if (bit == 1) { cfg.self_test = 1; continue; }
        if (++i >= argc) goto bad_args;
        if (bit == 2) {
            if (!number(argv[i], MAX_PAGE, &cfg.size) ||
                (cfg.size != 4096 && cfg.size != 16384 && cfg.size != 65536)) goto bad_args;
        } else if (bit == 4) {
            if (!number(argv[i], MAX_PAGES, &cfg.pages) || !cfg.pages) goto bad_args;
        } else if (bit == 8) {
            if (!number(argv[i], 10001, &cfg.samples) || cfg.samples < 101) goto bad_args;
        } else if (bit == 16) {
            if (!number(argv[i], 31, &cfg.batch_samples) || cfg.batch_samples < 3) goto bad_args;
        } else if (bit == 32) {
            if (!number(argv[i], 100, &cfg.min_batch_ms) || !cfg.min_batch_ms) goto bad_args;
        } else if (bit == 64) {
            for (size_t f = 0; f < COUNT(fixtures); f++) if (!strcmp(argv[i], fixtures[f])) cfg.fixture = (int)f;
            if (cfg.fixture < 0) goto bad_args;
        } else {
            for (size_t v = 0; v < COUNT(variants); v++) if (!strcmp(argv[i], variants[v].name)) cfg.variant = (int)v;
            if (cfg.variant < 0) goto bad_args;
        }
    }
    printf("# suite=page-profile-v1;environment=userspace;kernel_runtime=NOT_RUN\n");
    printf("# vv=%s;lz4=%s;zstd=%s;compiler=%s\n", VV_VERSION_STRING, LZ4_versionString(), ZSTD_versionString(), __VERSION__);
    printf("# vv_context_alignment=%zu;size_4096=%zu;size_16384=%zu;size_65536=%zu\n",
           vv_fast_context_alignment(), vv_fast_context_size(4096),
           vv_fast_context_size(16384), vv_fast_context_size(65536));
    printf("# lzo_rle=NOT_RUN;reason=no_suitable_userspace_LZO-RLE_implementation;ordinary_LZO_is_not_a_substitute\n");
    printf("# fixtures=synthetic;seed=0x6d2b79f5_xor_page_times_0x9e3779b9;seed_zero_replaced_with_one\n");
    printf("# input_fnv1a64=noncryptographic_page_fingerprint;fixture_recipe_SHA256_recorded_by_runner\n");
    printf("# zero_and_same_filled=consumer_bypassed_controls;not_a_claim_of_consumer_compression_benefit\n");
    printf("# pages=%zu;individual_samples=%zu;batch_samples=%zu;min_batch_ms=%zu;self_test=%d\n",
           cfg.pages, cfg.samples, cfg.batch_samples, cfg.min_batch_ms, cfg.self_test);
    printf("# latency=individual_calls_including_clock_and_dispatch_overhead;percentiles_are_not_batch_percentiles\n");
    printf("# throughput=separate_calibrated_batches;validation=outside_timing;cache=primed_synthetic_cohort\n");
    printf("# context_setup=allocation_plus_initialization_without_first_compression;cleanup_excluded\n");
    printf("# vv_init_preallocated=only_public_init;Zstd_setup_may_defer_workspace_allocation_until_first_compression\n");
    printf("# first_encode_call_and_first_decode_call=fresh_context_including_lazy_first_use_work\n");
    printf("# state_bytes=caller_storage_or_ZSTD_sizeof_CCtx_plus_DCtx;oneshot_hidden_workspace_not_measured;not_RSS\n");
    printf("# context_history=independent_per_page;LZ4_extState_resets_table_inside_each_compress_call\n");
    printf("# all_decodes=matching_codec_full_frame_or_LZ4_raw_block;no_unrelated_decoder_substitution\n");
    printf("# framing_bytes=container_and_block_headers_plus_checksum;payload_bytes_is_accounting_not_raw_codec_benchmark\n");
    printf("# checksums=VV_XXH64_64bit,Zstd_XXH64_low32bit,LZ4_none;no_authentication\n");
    if (!cfg.self_test) {
        uint64_t minimum = UINT64_MAX;
        for (size_t i = 0; i < 1001; i++) {
            uint64_t start = now_ns(), elapsed = now_ns() - start;
            if (elapsed < minimum) minimum = elapsed;
        }
        printf("# clock_pair_min_ns=%" PRIu64 ";not_subtracted_from_samples\n", minimum);
    }
    printf("status,fixture,input_bytes,page_index,input_fnv1a64,consumer_bypassed,codec,checksum,phase,sample,iterations,elapsed_ns,ns_per_call,compressed_bytes,framing_bytes,payload_bytes,not_smaller_than_input,state_bytes\n");
    const size_t sizes[] = {4096, 16384, 65536};
    for (size_t s = 0; s < COUNT(sizes); s++) {
        if (cfg.size && cfg.size != sizes[s]) continue;
        for (size_t fixture = 0; fixture < COUNT(fixtures); fixture++) {
            if (cfg.fixture >= 0 && cfg.fixture != (int)fixture) continue;
            struct cohort b;
            int ok = make_cohort(&b, sizes[s], cfg.pages, (int)fixture);
            for (size_t index = 0; ok && index < COUNT(variants); index++) {
                size_t v = (index + fixture + s) % COUNT(variants);
                if (cfg.variant >= 0 && cfg.variant != (int)v) continue;
                struct codec c;
                ok = init_codec(&c, &variants[v], b.size);
                if (ok) ok = measure(&c, &b, &cfg);
                release_codec(&c);
                if (!ok) fprintf(stderr, "FAIL: fixture=%s size=%zu codec=%s\n", fixtures[fixture], sizes[s], variants[v].name);
                if (ferror(stdout) || fflush(stdout)) ok = 0;
            }
            free_cohort(&b);
            if (!ok) { fprintf(stderr, "FAIL: allocation, validation, or output error\n"); return 1; }
        }
    }
    return ferror(stdout) || fflush(stdout) ? 1 : 0;
bad_args:
    usage(stderr, argv[0]);
    return 2;
}
