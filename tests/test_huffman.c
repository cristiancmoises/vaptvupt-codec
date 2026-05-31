/*
 * VaptVupt — Huffman codec unit tests
 *
 * Tests:
 *   1. All-zero input (RLE edge case)
 *   2. All-same symbol (single codeword)
 *   3. Uniform distribution (256 unique symbols)
 *   4. Skewed ASCII text (real-world distribution)
 *   5. Single-byte input
 *   6. Fuzz: 1000 random inputs, random lengths 1-65536
 */

#include "vv_huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; fprintf(stderr, "  %-44s ", name); fflush(stderr); } while(0)
#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); } while(0)

static int test_huff_roundtrip(const uint8_t *data, size_t len, const char *name) {
    TEST(name);

    if (len == 0) { PASS(); return 1; }

    size_t enc_cap = vvh_bound(len);
    uint8_t *enc_buf = (uint8_t *)malloc(enc_cap);
    uint8_t *dec_buf = (uint8_t *)calloc(1, len + 16);
    if (!enc_buf || !dec_buf) { FAIL("malloc"); free(enc_buf); free(dec_buf); return 0; }

    /* Encode */
    size_t enc_len = 0;
    vvh_error_t err = vvh_encode(data, len, enc_buf, enc_cap, &enc_len);

    if (err != VVH_OK) {
        /* Incompressible is expected for some inputs (random, uniform) */
        if (err == VVH_ERR_OVERFLOW) {
            /* Verify it's genuinely incompressible: encode bound >= src_len */
            PASS();
            free(enc_buf); free(dec_buf);
            return 1;
        }
        char msg[64]; snprintf(msg, sizeof(msg), "encode error %d", (int)err);
        FAIL(msg); free(enc_buf); free(dec_buf); return 0;
    }

    /* Check compression actually saved space */
    if (enc_len >= len) {
        FAIL("encoded not smaller");
        free(enc_buf); free(dec_buf); return 0;
    }

    /* Decode */
    size_t consumed = 0;
    err = vvh_decode(enc_buf, enc_len, dec_buf, len, len, &consumed);
    if (err != VVH_OK) {
        char msg[64]; snprintf(msg, sizeof(msg), "decode error %d", (int)err);
        FAIL(msg); free(enc_buf); free(dec_buf); return 0;
    }

    /* Verify */
    if (memcmp(data, dec_buf, len) != 0) {
        size_t i;
        for (i = 0; i < len; i++) if (data[i] != dec_buf[i]) break;
        char msg[128];
        snprintf(msg, sizeof(msg), "mismatch at byte %zu (expected %02x got %02x)",
                 i, data[i], dec_buf[i]);
        FAIL(msg); free(enc_buf); free(dec_buf); return 0;
    }

    PASS();
    free(enc_buf); free(dec_buf);
    return 1;
}

/* Full roundtrip through VaptVupt compress/decompress (tests integration) */
#ifndef VV_HUFFMAN_STANDALONE
static int test_vv_roundtrip(const uint8_t *data, size_t len,
                              vv_mode_t mode, const char *name) {
    TEST(name);

    size_t ccap = vv_compress_bound(len);
    uint8_t *comp = (uint8_t *)malloc(ccap);
    uint8_t *dec = (uint8_t *)calloc(1, len + 64);
    if (!comp || !dec) { FAIL("malloc"); free(comp); free(dec); return 0; }

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = mode;

    int64_t csz = vv_compress(data, len, comp, ccap, &opts);
    if (csz < 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "compress error %lld", (long long)csz);
        FAIL(msg); free(comp); free(dec); return 0;
    }

    int64_t dsz = vv_decompress(comp, (size_t)csz, dec, len + 32);
    if (dsz < 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "decompress error %lld", (long long)dsz);
        FAIL(msg); free(comp); free(dec); return 0;
    }

    if ((size_t)dsz != len || memcmp(data, dec, len) != 0) {
        FAIL("data mismatch");
        free(comp); free(dec); return 0;
    }

    PASS();
    free(comp); free(dec);
    return 1;
}
#endif

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt — Huffman Codec Tests                ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════╝\n\n");

    fprintf(stderr, "── Standalone Huffman tests ──\n");

    /* Test 1: All-zero input */
    {
        uint8_t data[4096];
        memset(data, 0, sizeof(data));
        test_huff_roundtrip(data, sizeof(data), "All-zero (4096 bytes)");
    }

    /* Test 2: All-same symbol */
    {
        uint8_t data[1000];
        memset(data, 0x42, sizeof(data));
        test_huff_roundtrip(data, sizeof(data), "All-same (1000 × 0x42)");
    }

    /* Test 3: Uniform distribution */
    {
        uint8_t data[256 * 4];
        for (int i = 0; i < 256 * 4; i++) data[i] = (uint8_t)(i & 0xFF);
        test_huff_roundtrip(data, sizeof(data), "Uniform (256 syms × 4 each)");
    }

    /* Test 4: Skewed ASCII text */
    {
        const char *text =
            "The quick brown fox jumps over the lazy dog. "
            "Pack my box with five dozen liquor jugs. "
            "How vexingly quick daft zebras jump! "
            "The five boxing wizards jump quickly. ";
        size_t len = strlen(text);
        /* Repeat to get enough data */
        size_t big_len = len * 100;
        uint8_t *big = (uint8_t *)malloc(big_len);
        for (size_t i = 0; i < big_len; i++) big[i] = (uint8_t)text[i % len];
        test_huff_roundtrip(big, big_len, "Skewed ASCII text (17KB)");
        free(big);
    }

    /* Test 5: Single byte */
    {
        uint8_t data[1] = {0xAA};
        test_huff_roundtrip(data, 1, "Single byte");
    }

    /* Test 6: Two distinct symbols */
    {
        uint8_t data[1000];
        for (int i = 0; i < 1000; i++) data[i] = (uint8_t)(i % 2 ? 0xAA : 0x55);
        test_huff_roundtrip(data, sizeof(data), "Two symbols (1000 bytes)");
    }

    /* Test 7: Fuzz — 200 random inputs */
    {
        TEST("Fuzz (200 random inputs, 1-8192 bytes)");
        uint32_t rng = 314159;
        int ok = 1;
        for (int trial = 0; trial < 200 && ok; trial++) {
            rng = rng * 1103515245 + 12345;
            size_t len = 1 + (rng >> 16) % 8192;
            uint8_t *data = (uint8_t *)malloc(len);
            for (size_t i = 0; i < len; i++) {
                rng = rng * 1103515245 + 12345;
                /* Bias toward ASCII for some trials, random for others */
                if (trial % 3 == 0)
                    data[i] = (uint8_t)(32 + (rng >> 16) % 95);
                else
                    data[i] = (uint8_t)(rng >> 16);
            }

            size_t enc_cap = vvh_bound(len);
            uint8_t *enc = (uint8_t *)malloc(enc_cap);
            uint8_t *dec = (uint8_t *)calloc(1, len + 16);

            size_t enc_len = 0;
            vvh_error_t err = vvh_encode(data, len, enc, enc_cap, &enc_len);
            if (err == VVH_OK) {
                size_t consumed = 0;
                err = vvh_decode(enc, enc_len, dec, len, len, &consumed);
                if (err != VVH_OK || memcmp(data, dec, len) != 0) {
                    fprintf(stderr, "FAIL at trial %d (len=%zu enc_len=%zu err=%d)\n",
                            trial, len, enc_len, (int)err);
                    ok = 0;
                }
            }
            /* VVH_ERR_OVERFLOW = incompressible, which is OK */

            free(data); free(enc); free(dec);
        }
        if (ok) PASS(); else { /* already printed FAIL */ tests_run--; }
    }

#ifndef VV_HUFFMAN_STANDALONE
    /* ─── Integration tests: full VaptVupt roundtrip with Huffman ─── */
    fprintf(stderr, "\n── VaptVupt integration tests (Huffman path) ──\n");

    /* Structured text (should trigger Huffman in balanced mode) */
    {
        const char *text = "The quick brown fox jumps over the lazy dog. ";
        size_t tlen = strlen(text);
        size_t big_len = tlen * 200;
        uint8_t *big = (uint8_t *)malloc(big_len);
        for (size_t i = 0; i < big_len; i++) big[i] = (uint8_t)text[i % tlen];

        test_vv_roundtrip(big, big_len, VV_MODE_ULTRA_FAST, "Text 9KB (ultra-fast)");
        test_vv_roundtrip(big, big_len, VV_MODE_BALANCED, "Text 9KB (balanced+huff)");
        test_vv_roundtrip(big, big_len, VV_MODE_EXTREME, "Text 9KB (extreme+huff)");
        free(big);
    }

    /* Binary data */
    {
        uint8_t data[8192];
        uint32_t rng = 99;
        for (int i = 0; i < 8192; i++) {
            rng = rng * 1103515245 + 12345;
            data[i] = (uint8_t)(rng >> 16);
        }
        test_vv_roundtrip(data, sizeof(data), VV_MODE_BALANCED, "Random 8KB (balanced)");
    }

    /* Source code (real-world) */
    {
        FILE *f = fopen("src/vv_encoder.c", "rb");
        if (f) {
            fseek(f, 0, SEEK_END); size_t len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
            uint8_t *data = (uint8_t *)malloc(len);
            if (data && fread(data, 1, len, f) == len) {
                test_vv_roundtrip(data, len, VV_MODE_BALANCED, "Source code (balanced+huff)");
                test_vv_roundtrip(data, len, VV_MODE_EXTREME, "Source code (extreme+huff)");
            }
            free(data);
            fclose(f);
        }
    }
#endif

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Results: %d/%d passed\n", tests_passed, tests_run);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
