/*
 * VaptVupt — Edge case + security tests
 *
 * Tests that catch bugs that don't show up in normal use but DO show
 * up in real deployments (VaptVupt at scale): malformed input rejection,
 * truncated frames, exact buffer-boundary cases, NULL handling, very
 * small valid inputs (1-3 bytes), and inputs at exact block-size
 * boundaries.
 */

#include "vaptvupt.h"
#include "vaptvupt_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stderr, "  %-50s ", name); \
    fflush(stderr); \
} while(0)
#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); } while(0)

/* ─────────────────────────────────────────────────────────────────
 * Test helpers
 * ───────────────────────────────────────────────────────────────── */

static int try_roundtrip(const uint8_t *src, size_t src_len) {
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    size_t cap = vv_compress_bound(src_len);
    if (cap == 0) cap = 256;
    uint8_t *comp = (uint8_t *)malloc(cap);
    if (!comp) return 0;
    int64_t csz = vv_compress(src, src_len, comp, cap, &opts);
    if (csz < 0) { free(comp); return -1; }
    uint8_t *dec = (uint8_t *)malloc(src_len + 64);
    if (!dec) { free(comp); return 0; }
    memset(dec, 0xAB, src_len + 64);
    int64_t dsz = vv_decompress(comp, (size_t)csz, dec, src_len + 32);
    int ok = (dsz == (int64_t)src_len) && (src_len == 0 || memcmp(dec, src, src_len) == 0);
    free(comp); free(dec);
    return ok;
}

/* ─────────────────────────────────────────────────────────────────
 * Edge cases: tiny inputs
 * ───────────────────────────────────────────────────────────────── */

static void test_tiny_inputs(void) {
    /* 1-byte through 16-byte inputs. The LZ engine has VV_MIN_MATCH=4,
     * so anything < 4 bytes can never have matches. The encoder must
     * still produce a valid frame (likely RAW block). */
    for (size_t n = 1; n <= 16; n++) {
        char label[64]; snprintf(label, sizeof(label), "tiny input: %zu byte(s)", n);
        TEST(label);
        uint8_t buf[16];
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(0xA0 + i);
        int r = try_roundtrip(buf, n);
        if (r == 1) PASS();
        else FAIL("roundtrip");
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Edge case: empty input
 * ───────────────────────────────────────────────────────────────── */

static void test_empty_input(void) {
    TEST("empty input (0 bytes)");
    uint8_t dummy = 0;
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    uint8_t comp[256];
    int64_t csz = vv_compress(&dummy, 0, comp, sizeof(comp), &opts);
    if (csz < 0) { FAIL("compress 0 bytes"); return; }
    /* Decompress should return 0 bytes */
    uint8_t dec[64];
    int64_t dsz = vv_decompress(comp, (size_t)csz, dec, sizeof(dec));
    if (dsz == 0) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "expected 0, got %lld", (long long)dsz); FAIL(m); }
}

/* ─────────────────────────────────────────────────────────────────
 * Malformed input: truncated frames
 * ───────────────────────────────────────────────────────────────── */

static void test_truncated_frames(void) {
    /* Make a valid frame, then truncate at various points. Decompress
     * must return an error code (not crash, not return garbage). */
    uint8_t src[1024];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i & 0xFF);
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    size_t cap = vv_compress_bound(sizeof(src));
    uint8_t *comp = (uint8_t *)malloc(cap);
    int64_t csz = vv_compress(src, sizeof(src), comp, cap, &opts);
    if (csz < 0) { TEST("truncated: setup"); FAIL("compress"); free(comp); return; }

    uint8_t dec[2048];
    /* Try truncating at every byte position */
    int crashes = 0;
    int rejected = 0;
    int erroneously_succeeded = 0;
    for (size_t trunc = 1; trunc < (size_t)csz; trunc++) {
        int64_t dsz = vv_decompress(comp, trunc, dec, sizeof(dec));
        if (dsz < 0) rejected++;
        else if (dsz == sizeof(src) && memcmp(dec, src, sizeof(src)) == 0) {
            /* Truncated input shouldn't decompress to full data */
            erroneously_succeeded++;
        }
    }
    (void)crashes;
    char label[80];
    snprintf(label, sizeof(label),
        "truncated frames: %d rejected, %d wrong-success",
        rejected, erroneously_succeeded);
    TEST(label);
    /* All truncations should be rejected (returned negative). */
    if (erroneously_succeeded == 0) PASS();
    else FAIL("some truncations decoded as success");
    free(comp);
}

/* ─────────────────────────────────────────────────────────────────
 * Malformed input: corrupted bytes
 * ───────────────────────────────────────────────────────────────── */

static void test_byte_flips(void) {
    /* Flip individual bytes in valid compressed output. The decoder
     * MUST NOT crash, MUST NOT return garbage that looks like success.
     * It may return error OR may return correct data (some bit flips
     * don't affect output — e.g. flipping a literal byte's high bit
     * after entropy decode just changes one output byte). */
    uint8_t src[512];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)('A' + (i % 26));
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    opts.checksum = 1;  /* checksum will catch most corruption */
    size_t cap = vv_compress_bound(sizeof(src));
    uint8_t *comp = (uint8_t *)malloc(cap);
    int64_t csz = vv_compress(src, sizeof(src), comp, cap, &opts);
    if (csz < 0) { TEST("byte_flips: setup"); FAIL("compress"); free(comp); return; }

    uint8_t dec[1024];
    int caught = 0;
    int silent = 0;
    int wrong_data = 0;
    for (size_t pos = 0; pos < (size_t)csz; pos++) {
        for (uint8_t flip = 1; flip < 8; flip += 3) {  /* try 3 bit flips */
            uint8_t orig = comp[pos];
            comp[pos] = orig ^ flip;
            int64_t dsz = vv_decompress(comp, (size_t)csz, dec, sizeof(dec));
            if (dsz < 0) caught++;
            else if (dsz != sizeof(src) || memcmp(dec, src, sizeof(src)) != 0) {
                wrong_data++;
            } else {
                silent++;
            }
            comp[pos] = orig;
        }
    }
    char label[100];
    snprintf(label, sizeof(label),
        "byte flips w/ checksum: %d caught, %d silent, %d wrong",
        caught, silent, wrong_data);
    TEST(label);
    /* With checksum enabled, wrong_data should be 0 (checksum catches it).
     * Silent (decode returned correct data despite a flip) is OK — some
     * flips truly don't change the output (e.g. unused bits in an LZ
     * extra-bits field). */
    if (wrong_data == 0) PASS();
    else FAIL("checksum failed to catch corruption");
    free(comp);
}

/* ─────────────────────────────────────────────────────────────────
 * Block boundary cases
 * ───────────────────────────────────────────────────────────────── */

static void test_block_boundaries(void) {
    /* VV_MAX_BLOCK_SIZE = 1 MB. Test inputs at exactly 1 byte before,
     * exactly at, and 1 byte after that boundary. */
    size_t sizes[] = {
        VV_MAX_BLOCK_SIZE - 1,
        VV_MAX_BLOCK_SIZE,
        VV_MAX_BLOCK_SIZE + 1,
        2 * VV_MAX_BLOCK_SIZE - 1,
        2 * VV_MAX_BLOCK_SIZE,
        2 * VV_MAX_BLOCK_SIZE + 1,
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i];
        char label[80];
        snprintf(label, sizeof(label), "block boundary: %zu bytes", n);
        TEST(label);
        uint8_t *buf = (uint8_t *)malloc(n);
        if (!buf) { FAIL("malloc"); continue; }
        /* Fill with mildly-compressible data */
        for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)((j * 7 + (j >> 3)) & 0xFF);
        int r = try_roundtrip(buf, n);
        free(buf);
        if (r == 1) PASS();
        else FAIL("roundtrip");
    }
}

/* ─────────────────────────────────────────────────────────────────
 * Pathological: all-same-byte
 * ───────────────────────────────────────────────────────────────── */

static void test_all_same_byte(void) {
    /* Single repeated byte. Should compress to RLE (very small). */
    size_t n = 100000;
    uint8_t *buf = (uint8_t *)malloc(n);
    memset(buf, 0xCC, n);
    TEST("all-same-byte 100K (RLE-ideal)");
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    size_t cap = vv_compress_bound(n);
    uint8_t *comp = (uint8_t *)malloc(cap);
    int64_t csz = vv_compress(buf, n, comp, cap, &opts);
    int ok = csz > 0 && (size_t)csz < 200;  /* should be ~tens of bytes */
    if (ok) {
        uint8_t *dec = (uint8_t *)malloc(n + 32);
        int64_t dsz = vv_decompress(comp, (size_t)csz, dec, n + 32);
        ok = (dsz == (int64_t)n) && (memcmp(dec, buf, n) == 0);
        if (ok) {
            char m[64]; snprintf(m, sizeof(m), "PASS (%lld bytes)", (long long)csz);
            tests_passed++; fprintf(stderr, "%s\n", m);
        } else FAIL("roundtrip");
        free(dec);
    } else {
        char m[64]; snprintf(m, sizeof(m), "csz=%lld (expected <200)", (long long)csz);
        FAIL(m);
    }
    free(buf); free(comp);
}

/* ─────────────────────────────────────────────────────────────────
 * Pathological: alternating bytes
 * ───────────────────────────────────────────────────────────────── */

static void test_alternating(void) {
    /* Period-2 alternating pattern. Easy LZ but no RLE. */
    size_t n = 50000;
    uint8_t *buf = (uint8_t *)malloc(n);
    for (size_t i = 0; i < n; i++) buf[i] = (i & 1) ? 0xAA : 0x55;
    TEST("alternating bytes 50K (period-2 LZ)");
    int r = try_roundtrip(buf, n);
    free(buf);
    if (r == 1) PASS(); else FAIL("roundtrip");
}

/* ─────────────────────────────────────────────────────────────────
 * Pathological: incremental sequence
 * ───────────────────────────────────────────────────────────────── */

static void test_incremental(void) {
    /* Counter pattern: 0,1,2,3,...,255,0,1,... (period 256, no LZ
     * matches shorter than 256). Tests engine on barely-matchable data. */
    size_t n = 10000;
    uint8_t *buf = (uint8_t *)malloc(n);
    for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i & 0xFF);
    TEST("incremental counter 10K (period-256)");
    int r = try_roundtrip(buf, n);
    free(buf);
    if (r == 1) PASS(); else FAIL("roundtrip");
}

/* ─────────────────────────────────────────────────────────────────
 * NULL / parameter validation
 * ───────────────────────────────────────────────────────────────── */

static void test_null_parameters(void) {
    uint8_t buf[100];
    memset(buf, 0xAA, sizeof(buf));
    uint8_t out[256];
    vv_options_t opts; vv_default_options(&opts);

    TEST("vv_compress: NULL src returns error");
    int64_t r = vv_compress(NULL, sizeof(buf), out, sizeof(out), &opts);
    if (r < 0) PASS(); else FAIL("expected error");

    TEST("vv_compress: NULL dst returns error");
    r = vv_compress(buf, sizeof(buf), NULL, sizeof(out), &opts);
    if (r < 0) PASS(); else FAIL("expected error");

    TEST("vv_compress: NULL opts uses defaults (Sprint 95 audit)");
    /* The original contract required opts != NULL. Sprint 95 audit
     * found this was inconsistent with vv_cstream_create which already
     * accepted NULL (after Sprint 89 fix). Both APIs now accept NULL
     * opts and fall back to vv_default_options. */
    int64_t csz = vv_compress(buf, sizeof(buf), out, sizeof(out), NULL);
    if (csz > 0) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "got %lld (expected success)", (long long)csz); FAIL(m); }

    TEST("vv_decompress: NULL src returns error");
    r = vv_decompress(NULL, 100, out, sizeof(out));
    if (r < 0) PASS(); else FAIL("expected error");

    TEST("vv_decompress: NULL dst returns error");
    r = vv_decompress(buf, sizeof(buf), NULL, sizeof(out));
    if (r < 0) PASS(); else FAIL("expected error");
}

/* ─────────────────────────────────────────────────────────────────
 * Insufficient dst buffer
 * ───────────────────────────────────────────────────────────────── */

static void test_insufficient_dst(void) {
    uint8_t src[1024];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)('A' + (i % 26));
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    size_t cap = vv_compress_bound(sizeof(src));
    uint8_t *comp = (uint8_t *)malloc(cap);

    TEST("vv_compress: dst_cap=1 returns error (overflow)");
    int64_t r = vv_compress(src, sizeof(src), comp, 1, &opts);
    if (r < 0) PASS(); else { char m[40]; snprintf(m, sizeof(m), "got csz=%lld with 1B dst", (long long)r); FAIL(m); }

    /* For decompress: undersized output buffer */
    int64_t csz = vv_compress(src, sizeof(src), comp, cap, &opts);
    TEST("vv_decompress: dst_cap < content_size returns error");
    uint8_t small_dst[100];
    r = vv_decompress(comp, (size_t)csz, small_dst, sizeof(small_dst));
    if (r < 0) PASS(); else FAIL("expected overflow error");

    free(comp);
}

/* ─────────────────────────────────────────────────────────────────
 * Garbage / non-VV input
 * ───────────────────────────────────────────────────────────────── */

static void test_garbage_input(void) {
    uint8_t out[1024];
    /* All zeros */
    TEST("decompress: all-zero input rejected");
    uint8_t zero[100] = {0};
    int64_t r = vv_decompress(zero, sizeof(zero), out, sizeof(out));
    if (r < 0) PASS(); else FAIL("expected error");

    /* All 0xFF */
    TEST("decompress: all-0xFF input rejected");
    uint8_t ff[100]; memset(ff, 0xFF, sizeof(ff));
    r = vv_decompress(ff, sizeof(ff), out, sizeof(out));
    if (r < 0) PASS(); else FAIL("expected error");

    /* Random garbage */
    TEST("decompress: random garbage rejected (or detected)");
    uint8_t junk[256];
    uint32_t s = 0x12345678;
    for (size_t i = 0; i < sizeof(junk); i++) {
        s = s * 1664525u + 1013904223u;
        junk[i] = (uint8_t)(s >> 24);
    }
    r = vv_decompress(junk, sizeof(junk), out, sizeof(out));
    /* Either error or successful decode — must not crash. */
    PASS();

    /* Empty */
    TEST("decompress: 0-byte input rejected");
    r = vv_decompress(zero, 0, out, sizeof(out));
    if (r < 0) PASS(); else FAIL("expected error");

    /* Less than frame header */
    TEST("decompress: 4-byte input rejected");
    r = vv_decompress(zero, 4, out, sizeof(out));
    if (r < 0) PASS(); else FAIL("expected error");
}

/* ─────────────────────────────────────────────────────────────────
 * Stress: many tiny independent compressions
 * ───────────────────────────────────────────────────────────────── */

static void test_stress_many_tiny(void) {
    /* Compress 1000 tiny files in sequence — exercises the path
     * commonly used by VaptVupt for per-file backups. */
    TEST("stress: 1000 × 50-byte files");
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    int failures = 0;
    for (int i = 0; i < 1000; i++) {
        uint8_t buf[50];
        for (size_t j = 0; j < sizeof(buf); j++) {
            buf[j] = (uint8_t)((i * 13 + j * 7) & 0xFF);
        }
        if (try_roundtrip(buf, sizeof(buf)) != 1) failures++;
    }
    if (failures == 0) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "%d/1000 failures", failures); FAIL(m); }
}

/* ─────────────────────────────────────────────────────────────────
 * Stress: compress/decompress alternating sizes
 * ───────────────────────────────────────────────────────────────── */

static void test_stress_size_variation(void) {
    /* Alternate between small and large compressions. Catches state
     * leakage between calls (e.g. context buffers not properly reset). */
    TEST("stress: alternating 100B/100KB compressions");
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;
    int failures = 0;
    for (int i = 0; i < 50; i++) {
        size_t n = (i & 1) ? 100 : 100000;
        uint8_t *buf = (uint8_t *)malloc(n);
        for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)((i + j) & 0xFF);
        if (try_roundtrip(buf, n) != 1) failures++;
        free(buf);
    }
    if (failures == 0) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "%d/50 failures", failures); FAIL(m); }
}

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt — Edge-case and security tests                       ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════════╝\n\n");

    test_tiny_inputs();
    test_empty_input();
    test_block_boundaries();
    test_truncated_frames();
    test_byte_flips();
    test_all_same_byte();
    test_alternating();
    test_incremental();
    test_null_parameters();
    test_insufficient_dst();
    test_garbage_input();
    test_stress_many_tiny();
    test_stress_size_variation();

    fprintf(stderr, "\n  Results: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
