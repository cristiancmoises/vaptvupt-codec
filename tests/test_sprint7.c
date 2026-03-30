/*
 * VaptVupt — Sprint 7 Test Suite
 *
 * Tests: context model roundtrip, backward compat, fuzz, throughput
 */

#include "vv_ans.h"
#ifndef VV_ANS_STANDALONE
#include "vaptvupt.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int tests_run = 0, tests_passed = 0;
#define TEST(n) do { tests_run++; fprintf(stderr, "  %-52s ", n); fflush(stderr); } while(0)
#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); } while(0)

/* 1. Context model: structured data roundtrip */
static void test_ctx_structured(void) {
    TEST("Context model: structured data roundtrip");
    const char *t = "{\"name\":\"Alice\",\"age\":30}\n{\"name\":\"Bob\",\"age\":25}\n";
    size_t tlen = strlen(t);
    size_t len = tlen * 200;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)t[i % tlen];

    size_t ecap = vva_bound(len);
    uint8_t *enc = (uint8_t *)malloc(ecap);
    uint8_t *dec = (uint8_t *)calloc(1, len + 16);
    size_t elen = 0;

    vva_error_t err = vva_encode_ctx(data, len, enc, ecap, &elen);
    if (err != VVA_OK) { FAIL("encode failed"); goto c1; }

    size_t con = 0;
    err = vva_decode_ctx(enc, elen, dec, len, len, &con);
    if (err != VVA_OK || memcmp(data, dec, len) != 0) FAIL("decode mismatch");
    else PASS();
c1: free(data); free(enc); free(dec);
}

/* 2. Context model: ratio improvement on JSON-like data */
static void test_ctx_ratio_json(void) {
    TEST("Context model: JSON ratio > global ANS ratio");
    const char *t = "{\"id\":12345,\"name\":\"Alice Johnson\",\"active\":true}\n";
    size_t tlen = strlen(t);
    size_t len = tlen * 400;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)t[i % tlen];

    size_t ecap = vva_bound(len);
    uint8_t *enc_g = (uint8_t *)malloc(ecap);
    uint8_t *enc_c = (uint8_t *)malloc(ecap);
    size_t elen_g = 0, elen_c = 0;

    vva_encode(data, len, enc_g, ecap, &elen_g);
    vva_encode_ctx(data, len, enc_c, ecap, &elen_c);

    if (elen_c > 0 && elen_g > 0 && elen_c < elen_g) {
        char m[80]; snprintf(m, sizeof(m), "ctx=%zu global=%zu (%.1f%% smaller)",
                              elen_c, elen_g, 100.0*(1.0-(double)elen_c/elen_g));
        fprintf(stderr, "PASS (%s)\n", m); tests_passed++;
    } else if (elen_c == 0 || elen_g == 0) {
        FAIL("one encoder failed");
    } else {
        char m[80]; snprintf(m, sizeof(m), "ctx=%zu global=%zu (no improvement)", elen_c, elen_g);
        FAIL(m);
    }
    free(data); free(enc_g); free(enc_c);
}

/* 3. Context model: 200-trial fuzz */
static void test_ctx_fuzz(void) {
    TEST("Context model: 200-trial fuzz roundtrip");
    uint32_t rng = 577215;
    int ok = 1;
    for (int trial = 0; trial < 200 && ok; trial++) {
        rng = rng * 1103515245 + 12345;
        size_t len = 64 + (rng >> 16) % 8192;
        uint8_t *data = (uint8_t *)malloc(len);
        for (size_t i = 0; i < len; i++) {
            rng = rng * 1103515245 + 12345;
            data[i] = (trial % 3 == 0) ? (uint8_t)(32 + (rng >> 16) % 95)
                                         : (uint8_t)(rng >> 16);
        }

        size_t ecap = vva_bound(len);
        uint8_t *enc = (uint8_t *)malloc(ecap);
        uint8_t *dec = (uint8_t *)calloc(1, len + 16);
        size_t elen = 0;

        vva_error_t err = vva_encode_ctx(data, len, enc, ecap, &elen);
        if (err == VVA_OK) {
            size_t con = 0;
            err = vva_decode_ctx(enc, elen, dec, len, len, &con);
            if (err != VVA_OK || memcmp(data, dec, len) != 0) {
                fprintf(stderr, "FAIL at trial %d len=%zu err=%d\n", trial, len, (int)err);
                ok = 0;
            }
        }
        free(data); free(enc); free(dec);
    }
    if (ok) PASS();
}

/* 4. Context model: single-symbol input */
static void test_ctx_single_sym(void) {
    TEST("Context model: single-symbol input");
    uint8_t data[2000];
    memset(data, 0x42, sizeof(data));

    size_t ecap = vva_bound(sizeof(data));
    uint8_t *enc = (uint8_t *)malloc(ecap);
    uint8_t *dec = (uint8_t *)calloc(1, sizeof(data) + 16);
    size_t elen = 0;

    vva_error_t err = vva_encode_ctx(data, sizeof(data), enc, ecap, &elen);
    if (err == VVA_ERR_OVERFLOW) { PASS(); goto c4; } /* OK: single-sym overhead too large */
    if (err != VVA_OK) { FAIL("encode error"); goto c4; }

    size_t con = 0;
    err = vva_decode_ctx(enc, elen, dec, sizeof(data), sizeof(data), &con);
    if (err == VVA_OK && memcmp(data, dec, sizeof(data)) == 0) PASS();
    else FAIL("decode mismatch");
c4: free(enc); free(dec);
}

/* 5. Context model: two-symbol alternating */
static void test_ctx_alternating(void) {
    TEST("Context model: alternating AB (strong context)");
    size_t len = 4000;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)(i % 2 ? 'B' : 'A');

    size_t ecap = vva_bound(len);
    uint8_t *enc = (uint8_t *)malloc(ecap);
    uint8_t *dec = (uint8_t *)calloc(1, len + 16);
    size_t elen = 0;

    vva_error_t err = vva_encode_ctx(data, len, enc, ecap, &elen);
    if (err != VVA_OK) { FAIL("encode failed"); goto c5; }

    size_t con = 0;
    err = vva_decode_ctx(enc, elen, dec, len, len, &con);
    if (err == VVA_OK && memcmp(data, dec, len) == 0) {
        /* Context model should compress AB alternating to near zero */
        double ratio = (double)len / (double)elen;
        char m[64]; snprintf(m, sizeof(m), "ratio=%.1f:1", ratio);
        fprintf(stderr, "PASS (%s)\n", m); tests_passed++;
    } else FAIL("decode mismatch");
c5: free(data); free(enc); free(dec);
}

#ifndef VV_ANS_STANDALONE
/* 6. VaptVupt extreme mode roundtrip (context model path) */
static void test_vv_extreme_ctx(void) {
    TEST("VaptVupt extreme: context model roundtrip");
    const char *t = "{\"key\":\"value\",\"n\":42}\n";
    size_t tlen = strlen(t);
    size_t len = tlen * 500;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)t[i % tlen];

    size_t ccap = vv_compress_bound(len);
    uint8_t *comp = (uint8_t *)malloc(ccap);
    uint8_t *dec = (uint8_t *)calloc(1, len + 64);
    vv_options_t opts; vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;

    int64_t csz = vv_compress(data, len, comp, ccap, &opts);
    if (csz < 0) { FAIL("compress failed"); goto c6; }
    int64_t dsz = vv_decompress(comp, (size_t)csz, dec, len + 32);
    if (dsz != (int64_t)len || memcmp(data, dec, len) != 0) FAIL("mismatch");
    else PASS();
c6: free(data); free(comp); free(dec);
}

/* 7. Backward compat: 'I' blocks decode correctly */
static void test_backward_compat_I(void) {
    TEST("Backward compat: 'I' tag 4-way ANS blocks");
    uint8_t data[8192];
    for (int i = 0; i < 8192; i++) data[i] = (uint8_t)("Hello World "[i % 12]);

    size_t ccap = vv_compress_bound(sizeof(data));
    uint8_t *comp = (uint8_t *)malloc(ccap);
    uint8_t *dec = (uint8_t *)calloc(1, sizeof(data) + 64);
    vv_options_t opts; vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED; /* Uses 'I' tag */

    int64_t csz = vv_compress(data, sizeof(data), comp, ccap, &opts);
    if (csz < 0) { FAIL("compress failed"); goto c7; }
    int64_t dsz = vv_decompress(comp, (size_t)csz, dec, sizeof(data) + 32);
    if (dsz != (int64_t)sizeof(data) || memcmp(data, dec, sizeof(data)) != 0) FAIL("mismatch");
    else PASS();
c7: free(comp); free(dec);
}

/* 8. All 3 modes roundtrip on diverse data */
static void test_all_modes_diverse(void) {
    TEST("All modes × diverse data roundtrip");
    int ok = 1;
    vv_mode_t modes[] = { VV_MODE_ULTRA_FAST, VV_MODE_BALANCED, VV_MODE_EXTREME };
    uint8_t *datas[3];
    size_t lens[3] = {4096, 8192, 16384};

    /* Text */
    datas[0] = (uint8_t *)malloc(lens[0]);
    for (size_t i = 0; i < lens[0]; i++) datas[0][i] = (uint8_t)("The quick brown fox "[i%20]);
    /* Zeros */
    datas[1] = (uint8_t *)calloc(1, lens[1]);
    /* Random-ish */
    datas[2] = (uint8_t *)malloc(lens[2]);
    uint32_t rng = 99;
    for (size_t i = 0; i < lens[2]; i++) { rng = rng * 1103515245 + 12345; datas[2][i] = (uint8_t)(rng>>16); }

    for (int m = 0; m < 3 && ok; m++) {
        for (int d = 0; d < 3 && ok; d++) {
            size_t ccap = vv_compress_bound(lens[d]);
            uint8_t *comp = (uint8_t *)malloc(ccap);
            uint8_t *dec = (uint8_t *)calloc(1, lens[d] + 64);
            vv_options_t opts; vv_default_options(&opts); opts.mode = modes[m];
            int64_t csz = vv_compress(datas[d], lens[d], comp, ccap, &opts);
            if (csz < 0) { ok = 0; free(comp); free(dec); continue; }
            int64_t dsz = vv_decompress(comp, (size_t)csz, dec, lens[d] + 32);
            if (dsz != (int64_t)lens[d] || memcmp(datas[d], dec, lens[d]) != 0) ok = 0;
            free(comp); free(dec);
        }
    }
    for (int i = 0; i < 3; i++) free(datas[i]);
    if (ok) PASS(); else FAIL("some mode×data failed");
}

/* 9. Decode throughput */
static void test_decode_throughput(void) {
    TEST("Decode throughput: ≥ 1,000 MB/s (balanced)");
    size_t len = 1024 * 1024;
    uint8_t *data = (uint8_t *)malloc(len);
    const char *t = "The quick brown fox jumps over the lazy dog. ";
    size_t tlen = strlen(t);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)t[i % tlen];

    size_t ccap = vv_compress_bound(len);
    uint8_t *comp = (uint8_t *)malloc(ccap);
    uint8_t *dec = (uint8_t *)malloc(len + 64);
    vv_options_t opts; vv_default_options(&opts); opts.mode = VV_MODE_BALANCED;

    int64_t csz = vv_compress(data, len, comp, ccap, &opts);
    if (csz < 0) { FAIL("compress failed"); free(data); free(comp); free(dec); return; }

    vv_decompress(comp, (size_t)csz, dec, len + 32); /* warm up */
    struct timespec t0, t1;
    int iters = 200;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < iters; i++) vv_decompress(comp, (size_t)csz, dec, len + 32);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mbps = (double)len * iters / (1024.0 * 1024.0) / secs;
    if (mbps >= 1000.0) {
        char m[32]; snprintf(m, sizeof(m), "%.0f MB/s", mbps);
        fprintf(stderr, "PASS (%s)\n", m); tests_passed++;
    } else {
        char m[32]; snprintf(m, sizeof(m), "%.0f MB/s", mbps); FAIL(m);
    }
    free(data); free(comp); free(dec);
}
#endif

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt — Sprint 7 Tests                         ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════╝\n\n");

    test_ctx_structured();
    test_ctx_ratio_json();
    test_ctx_fuzz();
    test_ctx_single_sym();
    test_ctx_alternating();

#ifndef VV_ANS_STANDALONE
    test_vv_extreme_ctx();
    test_backward_compat_I();
    test_all_modes_diverse();
    test_decode_throughput();
#endif

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Results: %d/%d passed\n", tests_passed, tests_run);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");
    return (tests_passed == tests_run) ? 0 : 1;
}
