/*
 * VaptVupt — Streaming API tests
 *
 * Tests:
 *   1. Streaming xxh64 matches one-shot
 *   2. Streaming compress → one-shot decompress
 *   3. One-shot compress → streaming decompress
 *   4. Streaming compress → streaming decompress (full loop)
 *   5. Various chunk sizes: 1, 10, 100, 1000, 4096, 65536, 1M
 *   6. Checksum off/on
 *   7. Multi-megabyte input with window-crossing boundaries
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stderr, "  %-60s ", name); \
    fflush(stderr); \
} while(0)

#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); } while(0)

/* Generate input: pseudo-random but compressible. */
static void gen_compressible(uint8_t *buf, size_t n, uint32_t seed) {
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        /* 50% reuse recent bytes, 50% new pseudo-random */
        if ((s & 1) && i > 200) {
            buf[i] = buf[i - 200 + (s & 0x7F)];
        } else {
            buf[i] = (uint8_t)(s >> 24);
        }
    }
}

/* Test 1: Streaming xxh64 matches one-shot */
static int test_xxh64_streaming(void) {
    TEST("xxh64 streaming matches one-shot");

    const char *s = "The quick brown fox jumps over the lazy dog and beyond";
    size_t n = strlen(s);
    uint64_t oneshot = vv_xxh64(s, n, 42);

    /* Test split at every position */
    for (size_t split = 0; split <= n; split++) {
        vv_xxh64_state_t st;
        vv_xxh64_init(&st, 42);
        vv_xxh64_update(&st, s, split);
        vv_xxh64_update(&st, s + split, n - split);
        if (vv_xxh64_finalize(&st) != oneshot) {
            char msg[64]; snprintf(msg, sizeof(msg), "split=%zu mismatch", split);
            FAIL(msg); return 0;
        }
    }

    /* Test larger input, many-way split */
    size_t N = 100000;
    uint8_t *big = (uint8_t *)malloc(N);
    gen_compressible(big, N, 12345);
    uint64_t ref = vv_xxh64(big, N, 0);
    vv_xxh64_state_t st;
    vv_xxh64_init(&st, 0);
    for (size_t off = 0; off < N; off += 777) {
        size_t len = 777 < N - off ? 777 : N - off;
        vv_xxh64_update(&st, big + off, len);
    }
    if (vv_xxh64_finalize(&st) != ref) { free(big); FAIL("big split mismatch"); return 0; }
    free(big);

    PASS(); return 1;
}

/* Test 2: Streaming compress → one-shot decompress roundtrip */
static int test_streaming_compress(size_t n, size_t chunk, vv_mode_t mode, const char *name) {
    TEST(name);

    uint8_t *buf = (uint8_t *)malloc(n);
    gen_compressible(buf, n, 42);

    size_t cap = vv_compress_bound(n) + 1024 * 1024;
    uint8_t *comp = (uint8_t *)malloc(cap);

    vv_options_t opts; vv_default_options(&opts);
    opts.mode = mode;
    vv_cstream_t *c = vv_cstream_create(&opts);
    if (!c) { FAIL("cstream_create"); free(buf); free(comp); return 0; }

    size_t total = 0, pos = 0;
    while (pos < n) {
        size_t rem = n - pos;
        size_t chk = chunk < rem ? chunk : rem;
        int is_last = (pos + chk >= n);
        size_t w;
        int err = vv_cstream_compress_chunk(c, buf + pos, chk, comp + total, cap - total, &w, is_last);
        if (err != VV_OK) { vv_cstream_destroy(c); free(buf); free(comp); FAIL("compress chunk err"); return 0; }
        total += w;
        pos += chk;
    }
    vv_cstream_destroy(c);

    uint8_t *dec = (uint8_t *)malloc(n + 32);
    int64_t dsz = vv_decompress(comp, total, dec, n + 32);
    if (dsz < 0 || (size_t)dsz != n || memcmp(dec, buf, n) != 0) {
        free(buf); free(comp); free(dec); FAIL("roundtrip"); return 0;
    }

    free(buf); free(comp); free(dec);
    PASS(); return 1;
}

/* Test 3: One-shot compress → streaming decompress */
static int test_streaming_decompress(size_t n, size_t chunk, vv_mode_t mode, const char *name) {
    TEST(name);

    uint8_t *buf = (uint8_t *)malloc(n);
    gen_compressible(buf, n, 99);

    size_t cap = vv_compress_bound(n);
    uint8_t *comp = (uint8_t *)malloc(cap);
    vv_options_t opts; vv_default_options(&opts);
    opts.mode = mode;
    int64_t csz = vv_compress(buf, n, comp, cap, &opts);
    if (csz < 0) { free(buf); free(comp); FAIL("one-shot compress"); return 0; }

    uint8_t *dec = (uint8_t *)malloc(n + 32);
    vv_dstream_t *d = vv_dstream_create();
    if (!d) { free(buf); free(comp); free(dec); FAIL("dstream_create"); return 0; }

    size_t src_pos = 0, dec_written = 0;
    int ret = 0;
    while (src_pos < (size_t)csz) {
        size_t rem = (size_t)csz - src_pos;
        size_t chk = chunk < rem ? chunk : rem;
        size_t consumed, written;
        ret = vv_dstream_decompress_chunk(d, comp + src_pos, chk, dec, n + 32, &consumed, &written);
        if (ret < 0) { vv_dstream_destroy(d); free(buf); free(comp); free(dec); FAIL("decompress err"); return 0; }
        src_pos += consumed;
        dec_written = written;
        if (ret == 1) break;
    }
    vv_dstream_destroy(d);

    if (ret != 1 || dec_written != n || memcmp(dec, buf, n) != 0) {
        free(buf); free(comp); free(dec); FAIL("roundtrip"); return 0;
    }

    free(buf); free(comp); free(dec);
    PASS(); return 1;
}

/* Test 4: Full streaming loop */
static int test_streaming_full(size_t n, size_t enc_chunk, size_t dec_chunk, vv_mode_t mode, int checksum, const char *name) {
    TEST(name);

    uint8_t *buf = (uint8_t *)malloc(n);
    gen_compressible(buf, n, 7);

    size_t cap = vv_compress_bound(n) + 1024 * 1024;
    uint8_t *comp = (uint8_t *)malloc(cap);

    vv_options_t opts; vv_default_options(&opts);
    opts.mode = mode;
    opts.checksum = checksum;

    vv_cstream_t *c = vv_cstream_create(&opts);
    size_t total = 0, pos = 0;
    while (pos < n) {
        size_t rem = n - pos;
        size_t chk = enc_chunk < rem ? enc_chunk : rem;
        int is_last = (pos + chk >= n);
        size_t w;
        if (vv_cstream_compress_chunk(c, buf + pos, chk, comp + total, cap - total, &w, is_last) != VV_OK) {
            vv_cstream_destroy(c); free(buf); free(comp); FAIL("compress"); return 0;
        }
        total += w;
        pos += chk;
    }
    vv_cstream_destroy(c);

    uint8_t *dec = (uint8_t *)malloc(n + 32);
    vv_dstream_t *d = vv_dstream_create();
    size_t src_pos = 0, dec_written = 0;
    int ret = 0;
    while (src_pos < total) {
        size_t rem = total - src_pos;
        size_t chk = dec_chunk < rem ? dec_chunk : rem;
        size_t consumed, written;
        ret = vv_dstream_decompress_chunk(d, comp + src_pos, chk, dec, n + 32, &consumed, &written);
        if (ret < 0) { vv_dstream_destroy(d); free(buf); free(comp); free(dec); FAIL("decompress"); return 0; }
        src_pos += consumed;
        dec_written = written;
        if (ret == 1) break;
    }
    vv_dstream_destroy(d);

    if (ret != 1 || dec_written != n || memcmp(dec, buf, n) != 0) {
        free(buf); free(comp); free(dec); FAIL("roundtrip"); return 0;
    }

    free(buf); free(comp); free(dec);
    PASS(); return 1;
}

int main(void) {
    fprintf(stderr, "\n═══ VaptVupt streaming tests ═══\n");

    test_xxh64_streaming();

    /* Streaming compress → one-shot decompress */
    test_streaming_compress(1000,    100,      VV_MODE_BALANCED,   "stream-enc 1KB chunk=100 balanced");
    test_streaming_compress(100000,  4096,     VV_MODE_BALANCED,   "stream-enc 100KB chunk=4K balanced");
    test_streaming_compress(1000000, 65536,    VV_MODE_BALANCED,   "stream-enc 1MB chunk=64K balanced");
    test_streaming_compress(3000000, 1048576,  VV_MODE_BALANCED,   "stream-enc 3MB chunk=1M balanced");
    test_streaming_compress(100000,  4096,     VV_MODE_ULTRA_FAST, "stream-enc 100KB chunk=4K fast");

    /* One-shot compress → streaming decompress */
    test_streaming_decompress(1000,    1,       VV_MODE_BALANCED, "stream-dec 1KB chunk=1 balanced");
    test_streaming_decompress(100000,  100,     VV_MODE_BALANCED, "stream-dec 100KB chunk=100 balanced");
    test_streaming_decompress(1000000, 4096,    VV_MODE_BALANCED, "stream-dec 1MB chunk=4K balanced");
    test_streaming_decompress(3000000, 1048576, VV_MODE_BALANCED, "stream-dec 3MB chunk=1M balanced");

    /* Full streaming loop: varied chunk pairs */
    test_streaming_full(100000, 4096,    4096,    VV_MODE_BALANCED,   1, "full stream 100KB enc=dec=4K balanced+cks");
    test_streaming_full(100000, 4096,    4096,    VV_MODE_BALANCED,   0, "full stream 100KB enc=dec=4K balanced nocks");
    test_streaming_full(100000, 100,     65536,   VV_MODE_BALANCED,   1, "full stream 100KB enc=100 dec=64K");
    test_streaming_full(100000, 65536,   100,     VV_MODE_BALANCED,   1, "full stream 100KB enc=64K dec=100");
    test_streaming_full(3000000, 1048576, 1048576,VV_MODE_BALANCED,   1, "full stream 3MB enc=dec=1M");
    test_streaming_full(3000000, 13337,   7777,   VV_MODE_BALANCED,   1, "full stream 3MB odd chunks");

    /* Reset tests — compress multiple independent frames through one ctx */
    {
        TEST("cstream_reset: 3 sequential files in one ctx");
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        vv_cstream_t *c = vv_cstream_create(&opts);
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            size_t n = 50000 + (size_t)i * 20000;
            uint8_t *buf = (uint8_t *)malloc(n);
            gen_compressible(buf, n, (uint32_t)(100 + i));
            uint8_t *comp = (uint8_t *)malloc(vv_compress_bound(n) + 4096);

            if (vv_cstream_reset(c, NULL) != VV_OK) { ok = 0; free(buf); free(comp); break; }

            size_t total = 0, pos = 0;
            while (pos < n) {
                size_t chk = 8192 < n - pos ? 8192 : n - pos;
                int is_last = (pos + chk >= n);
                size_t w;
                if (vv_cstream_compress_chunk(c, buf + pos, chk, comp + total, vv_compress_bound(n) + 4096 - total, &w, is_last) != VV_OK) {
                    ok = 0; break;
                }
                total += w; pos += chk;
            }
            if (!ok) { free(buf); free(comp); break; }

            uint8_t *dec = (uint8_t *)malloc(n + 32);
            int64_t dsz = vv_decompress(comp, total, dec, n + 32);
            if (dsz != (int64_t)n || memcmp(dec, buf, n) != 0) { ok = 0; free(buf); free(comp); free(dec); break; }
            free(buf); free(comp); free(dec);
        }
        vv_cstream_destroy(c);
        if (ok) PASS(); else FAIL("reset loop");
    }

    {
        TEST("dstream_reset: 3 sequential frames in one ctx");
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        vv_dstream_t *d = vv_dstream_create();
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            size_t n = 30000 + (size_t)i * 15000;
            uint8_t *buf = (uint8_t *)malloc(n);
            gen_compressible(buf, n, (uint32_t)(200 + i));
            uint8_t *comp = (uint8_t *)malloc(vv_compress_bound(n));
            int64_t csz = vv_compress(buf, n, comp, vv_compress_bound(n), &opts);
            if (csz < 0) { ok = 0; free(buf); free(comp); break; }

            if (vv_dstream_reset(d) != VV_OK) { ok = 0; free(buf); free(comp); break; }

            uint8_t *dec = (uint8_t *)malloc(n + 32);
            size_t src_pos = 0, written = 0; int ret = 0;
            while (src_pos < (size_t)csz) {
                size_t chk = 1024 < (size_t)csz - src_pos ? 1024 : (size_t)csz - src_pos;
                size_t consumed, w;
                ret = vv_dstream_decompress_chunk(d, comp + src_pos, chk, dec, n + 32, &consumed, &w);
                if (ret < 0) { ok = 0; break; }
                src_pos += consumed; written = w;
                if (ret == 1) break;
            }
            if (!ok || ret != 1 || written != n || memcmp(dec, buf, n) != 0) { ok = 0; free(buf); free(comp); free(dec); break; }
            free(buf); free(comp); free(dec);
        }
        vv_dstream_destroy(d);
        if (ok) PASS(); else FAIL("dreset loop");
    }

    {
        TEST("vv_get_frame_info: one-shot frame");
        uint8_t buf[5000]; gen_compressible(buf, sizeof(buf), 1);
        uint8_t comp[6000];
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        int64_t csz = vv_compress(buf, sizeof(buf), comp, sizeof(comp), &opts);
        vv_frame_info_t info;
        int r = vv_get_frame_info(comp, (size_t)csz, &info);
        if (r != VV_OK || info.version != 1 || info.content_size != sizeof(buf) || !info.has_checksum) {
            FAIL("one-shot info");
        } else PASS();
    }

    {
        TEST("vv_get_frame_info: streaming frame has content_size=0");
        uint8_t buf[3000]; gen_compressible(buf, sizeof(buf), 2);
        uint8_t comp[4000];
        vv_options_t opts; vv_default_options(&opts);
        vv_cstream_t *c = vv_cstream_create(&opts);
        size_t w;
        vv_cstream_compress_chunk(c, buf, sizeof(buf), comp, sizeof(comp), &w, 1);
        vv_cstream_destroy(c);
        vv_frame_info_t info;
        int r = vv_get_frame_info(comp, w, &info);
        if (r != VV_OK || info.content_size != 0) { FAIL("streaming info"); }
        else PASS();
    }

    {
        TEST("vv_get_frame_info: bad magic returns error");
        uint8_t bad[16]; memset(bad, 0xAB, 16);
        vv_frame_info_t info;
        int r = vv_get_frame_info(bad, sizeof(bad), &info);
        if (r != VV_ERR_BAD_MAGIC) FAIL("should reject bad magic");
        else PASS();
    }

    {
        TEST("Multi-frame: concatenated frames decode correctly");
        size_t N1 = 4096, N2 = 8000;
        uint8_t *a = (uint8_t *)malloc(N1); gen_compressible(a, N1, 101);
        uint8_t *b = (uint8_t *)malloc(N2); gen_compressible(b, N2, 102);
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        size_t cap1 = vv_compress_bound(N1);
        size_t cap2 = vv_compress_bound(N2);
        uint8_t *c1 = (uint8_t *)malloc(cap1);
        uint8_t *c2 = (uint8_t *)malloc(cap2);
        int64_t sz1 = vv_compress(a, N1, c1, cap1, &opts);
        int64_t sz2 = vv_compress(b, N2, c2, cap2, &opts);
        uint8_t *multi = (uint8_t *)malloc(sz1 + sz2);
        memcpy(multi, c1, sz1);
        memcpy(multi + sz1, c2, sz2);
        uint8_t *out = (uint8_t *)malloc(N1 + N2);
        int64_t dsz = vv_decompress(multi, sz1 + sz2, out, N1 + N2);
        int ok = (dsz == (int64_t)(N1 + N2)) && (memcmp(out, a, N1) == 0) && (memcmp(out + N1, b, N2) == 0);
        free(a); free(b); free(c1); free(c2); free(multi); free(out);
        if (ok) PASS(); else FAIL("multi-frame decode");
    }

    {
        TEST("vv_compress_mt: roundtrip on large input");
        size_t N = 5 * 1024 * 1024;  /* 5 MB to trigger chunking */
        uint8_t *buf = (uint8_t *)malloc(N); gen_compressible(buf, N, 999);
        size_t cap = vv_compress_bound(N) + 1024 * 1024;
        uint8_t *comp = (uint8_t *)malloc(cap);
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        int64_t sz = vv_compress_mt(buf, N, comp, cap, &opts, 0, 0);
        int ok = sz > 0;
        if (ok) {
            uint8_t *dec = (uint8_t *)malloc(N + 32);
            int64_t dsz = vv_decompress(comp, sz, dec, N + 32);
            ok = (dsz == (int64_t)N) && (memcmp(dec, buf, N) == 0);
            free(dec);
        }
        free(buf); free(comp);
        if (ok) PASS(); else FAIL("MT roundtrip");
    }

    {
        TEST("vv_compress_mt: small input uses single frame");
        size_t N = 1000;
        uint8_t *buf = (uint8_t *)malloc(N); gen_compressible(buf, N, 5);
        size_t cap = vv_compress_bound(N);
        uint8_t *comp = (uint8_t *)malloc(cap);
        vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
        int64_t sz = vv_compress_mt(buf, N, comp, cap, &opts, 4, 0);
        int ok = sz > 0;
        if (ok) {
            uint8_t *dec = (uint8_t *)malloc(N + 32);
            int64_t dsz = vv_decompress(comp, sz, dec, N + 32);
            ok = (dsz == (int64_t)N) && (memcmp(dec, buf, N) == 0);
            free(dec);
        }
        free(buf); free(comp);
        if (ok) PASS(); else FAIL("MT small input");
    }

    fprintf(stderr, "\n  Results: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
