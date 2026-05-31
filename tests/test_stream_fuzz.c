/*
 * VaptVupt — Streaming API Fuzzer (cstream + dstream).
 *
 * API contract (per existing tests/test_streaming.c):
 *   - cstream_compress_chunk: dst is per-call output buffer, *written
 *     is bytes-this-call. Caller advances its own buffer.
 *   - dstream_decompress_chunk: dst is a STABLE buffer base passed
 *     every call; *written is cumulative total. Caller does NOT
 *     advance dst between calls.
 *
 * Tests three round-trip variants:
 *   v1: streaming-encode → one-shot decode
 *   v2: one-shot encode → streaming-decode
 *   v3: streaming-encode → streaming-decode
 *
 * Each fixture runs 15 iterations with randomized chunk sizes.
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int fixtures_run = 0;
static int fixtures_passed = 0;
static int total_iterations = 0;

static uint64_t rng_state = 1;
static void rng_seed(uint64_t s) { rng_state = s ? s : 1; }
static uint64_t rng_u64(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static uint32_t rng_range(uint32_t hi) { return (uint32_t)(rng_u64() % hi); }

/* ─── V1: streaming encode → one-shot decode ─── */
static int test_v1(const uint8_t *src, size_t src_len, uint64_t seed) {
    rng_seed(seed);
    vv_options_t opts; vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;

    vv_cstream_t *cs = vv_cstream_create(&opts);
    if (!cs) return -1;

    size_t out_cap = src_len * 3 + 4096;
    uint8_t *out = (uint8_t *)malloc(out_cap);
    if (!out) { vv_cstream_destroy(cs); return -1; }
    size_t out_len = 0;

    size_t pos = 0;
    while (pos < src_len || out_len == 0) {
        size_t chunk = 1 + rng_range(65535);
        if (chunk > src_len - pos) chunk = src_len - pos;
        int is_last = (pos + chunk == src_len);
        if (out_cap - out_len < 4096) {
            out_cap *= 2;
            uint8_t *no = (uint8_t *)realloc(out, out_cap);
            if (!no) { free(out); vv_cstream_destroy(cs); return -1; }
            out = no;
        }
        size_t written = 0;
        int rc = vv_cstream_compress_chunk(cs, src + pos, chunk,
                                           out + out_len, out_cap - out_len,
                                           &written, is_last);
        if (rc != VV_OK) { free(out); vv_cstream_destroy(cs); return -2; }
        out_len += written;
        pos += chunk;
        if (is_last) break;
    }
    vv_cstream_destroy(cs);

    size_t dcap = src_len + 64;
    if (dcap < 1024) dcap = 1024;
    uint8_t *dec = (uint8_t *)malloc(dcap);
    if (!dec) { free(out); return -1; }
    int64_t dsz = vv_decompress(out, out_len, dec, dcap);
    int ok = (dsz == (int64_t)src_len && memcmp(dec, src, src_len) == 0);
    free(out); free(dec);
    return ok ? 0 : -3;
}

/* ─── V2: one-shot encode → streaming decode ─── */
static int test_v2(const uint8_t *src, size_t src_len, uint64_t seed) {
    rng_seed(seed);
    vv_options_t opts; vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;

    size_t cap = vv_compress_bound(src_len);
    uint8_t *cbuf = (uint8_t *)malloc(cap);
    if (!cbuf) return -1;
    int64_t csz = vv_compress(src, src_len, cbuf, cap, &opts);
    if (csz <= 0) { free(cbuf); return -2; }

    vv_dstream_t *ds = vv_dstream_create();
    if (!ds) { free(cbuf); return -1; }
    size_t dcap = src_len + 64;
    if (dcap < 1024) dcap = 1024;
    uint8_t *dout = (uint8_t *)malloc(dcap);
    if (!dout) { vv_dstream_destroy(ds); free(cbuf); return -1; }

    size_t cpos = 0;
    size_t total_written = 0;
    int frame_done = 0;
    int rc = VV_OK;
    while (cpos < (size_t)csz) {
        size_t chunk = 1 + rng_range(4096);
        if (chunk > (size_t)csz - cpos) chunk = (size_t)csz - cpos;
        size_t consumed = 0, written = 0;
        rc = vv_dstream_decompress_chunk(ds, cbuf + cpos, chunk,
                                          dout, dcap,
                                          &consumed, &written);
        if (rc < 0) break;
        total_written = written;  /* cumulative */
        cpos += consumed;
        if (rc == 1) { frame_done = 1; break; }
        if (consumed == 0 && written == total_written && cpos < (size_t)csz) {
            rc = -99;
            break;
        }
    }
    vv_dstream_destroy(ds);

    int result;
    if (rc < 0)                         result = -10;
    else if (!frame_done)               result = -11;
    else if (total_written != src_len)  result = -12;
    else if (memcmp(dout, src, src_len) != 0) result = -13;
    else                                result = 0;

    free(cbuf); free(dout);
    return result;
}

/* ─── V3: streaming encode → streaming decode ─── */
static int test_v3(const uint8_t *src, size_t src_len,
                    uint64_t enc_seed, uint64_t dec_seed) {
    rng_seed(enc_seed);
    vv_options_t opts; vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;

    vv_cstream_t *cs = vv_cstream_create(&opts);
    if (!cs) return -1;

    size_t out_cap = src_len * 3 + 4096;
    uint8_t *out = (uint8_t *)malloc(out_cap);
    if (!out) { vv_cstream_destroy(cs); return -1; }
    size_t out_len = 0;

    size_t pos = 0;
    while (pos < src_len || out_len == 0) {
        size_t chunk = 1 + rng_range(65535);
        if (chunk > src_len - pos) chunk = src_len - pos;
        int is_last = (pos + chunk == src_len);
        if (out_cap - out_len < 4096) {
            out_cap *= 2;
            uint8_t *no = (uint8_t *)realloc(out, out_cap);
            if (!no) { free(out); vv_cstream_destroy(cs); return -1; }
            out = no;
        }
        size_t written = 0;
        int rc = vv_cstream_compress_chunk(cs, src + pos, chunk,
                                           out + out_len, out_cap - out_len,
                                           &written, is_last);
        if (rc != VV_OK) { free(out); vv_cstream_destroy(cs); return -2; }
        out_len += written;
        pos += chunk;
        if (is_last) break;
    }
    vv_cstream_destroy(cs);

    rng_seed(dec_seed);
    vv_dstream_t *ds = vv_dstream_create();
    if (!ds) { free(out); return -1; }
    size_t dcap = src_len + 64;
    if (dcap < 1024) dcap = 1024;
    uint8_t *dout = (uint8_t *)malloc(dcap);
    if (!dout) { vv_dstream_destroy(ds); free(out); return -1; }

    size_t cpos = 0;
    size_t total_written = 0;
    int frame_done = 0;
    int rc = VV_OK;
    while (cpos < out_len) {
        size_t chunk = 1 + rng_range(4096);
        if (chunk > out_len - cpos) chunk = out_len - cpos;
        size_t consumed = 0, written = 0;
        rc = vv_dstream_decompress_chunk(ds, out + cpos, chunk,
                                          dout, dcap,
                                          &consumed, &written);
        if (rc < 0) break;
        total_written = written;
        cpos += consumed;
        if (rc == 1) { frame_done = 1; break; }
        if (consumed == 0 && written == total_written && cpos < out_len) {
            rc = -99;
            break;
        }
    }
    vv_dstream_destroy(ds);

    int result;
    if (rc < 0)                         result = -10;
    else if (!frame_done)               result = -11;
    else if (total_written != src_len)  result = -12;
    else if (memcmp(dout, src, src_len) != 0) result = -13;
    else                                result = 0;

    free(out); free(dout);
    return result;
}

/* ─── Fixtures ─── */

static uint8_t *gen_random(size_t n, uint64_t seed) {
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return NULL;
    rng_seed(seed);
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rng_u64();
    return buf;
}
static uint8_t *gen_repeat(size_t n, uint64_t seed) {
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return NULL;
    rng_seed(seed);
    uint8_t pattern[32];
    for (int i = 0; i < 32; i++) pattern[i] = (uint8_t)rng_u64();
    for (size_t i = 0; i < n; i++) buf[i] = pattern[i & 31];
    return buf;
}
static uint8_t *gen_zeros(size_t n, uint64_t seed) {
    (void)seed; return (uint8_t *)calloc(1, n);
}
static uint8_t *gen_text(size_t n, uint64_t seed) {
    (void)seed;
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return NULL;
    const char *lorem = "The quick brown fox jumps over the lazy dog. ";
    size_t llen = strlen(lorem);
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)lorem[i % llen];
    return buf;
}

typedef struct {
    const char *name;
    uint8_t *(*gen)(size_t, uint64_t);
    size_t len;
    uint64_t seed;
} fixture_t;

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt — Streaming API Fuzzer (cstream + dstream)          ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════════╝\n\n");

    fixture_t fixtures[] = {
        { "random-1KB",   gen_random, 1024,    0xCAFE },
        { "random-64KB",  gen_random, 65536,   0xBEEF },
        { "random-256KB", gen_random, 262144,  0xDEAD },
        { "repeat-1KB",   gen_repeat, 1024,    0x1234 },
        { "repeat-64KB",  gen_repeat, 65536,   0x5678 },
        { "repeat-1MB",   gen_repeat, 1048576, 0x9ABC },
        { "zeros-1KB",    gen_zeros,  1024,    0 },
        { "zeros-256KB",  gen_zeros,  262144,  0 },
        { "zeros-1MB",    gen_zeros,  1048576, 0 },
        { "text-4KB",     gen_text,   4096,    0 },
        { "text-64KB",    gen_text,   65536,   0 },
    };
    int nfix = (int)(sizeof(fixtures) / sizeof(fixtures[0]));
    const int iters = 15;

    for (int f = 0; f < nfix; f++) {
        fixture_t *fx = &fixtures[f];
        uint8_t *src = fx->gen(fx->len, fx->seed);
        if (!src) { fprintf(stderr, "Skipping %s\n", fx->name); continue; }

        int v1f = 0, v2f = 0, v3f = 0;
        int v1e = 0, v2e = 0, v3e = 0;
        for (int i = 0; i < iters; i++) {
            uint64_t s = ((uint64_t)f << 32) | (uint64_t)(i + 1);
            int r1 = test_v1(src, fx->len, s);
            int r2 = test_v2(src, fx->len, s + 0x1000);
            int r3 = test_v3(src, fx->len, s + 0x2000, s + 0x3000);
            if (r1 != 0) { v1f++; if (!v1e) v1e = r1; }
            if (r2 != 0) { v2f++; if (!v2e) v2e = r2; }
            if (r3 != 0) { v3f++; if (!v3e) v3e = r3; }
            total_iterations += 3;
        }

        fixtures_run++;
        int total_fail = v1f + v2f + v3f;
        fprintf(stderr, "  %-18s (%7d bytes) ", fx->name, (int)fx->len);
        fflush(stderr);
        if (total_fail == 0) {
            fprintf(stderr, "PASS (%d iters × 3 tests)\n", iters);
            fixtures_passed++;
        } else {
            fprintf(stderr, "FAIL v1=%d/%d(rc=%d) v2=%d/%d(rc=%d) v3=%d/%d(rc=%d)\n",
                    v1f, iters, v1e, v2f, iters, v2e, v3f, iters, v3e);
        }
        free(src);
    }

    fprintf(stderr, "\n  Results: %d/%d fixtures passed (%d total iterations)\n",
            fixtures_passed, fixtures_run, total_iterations);
    return (fixtures_passed == fixtures_run) ? 0 : 1;
}
