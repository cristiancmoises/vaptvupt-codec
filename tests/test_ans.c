/*
 * VaptVupt — ANS/tANS codec unit tests
 */
#include "vv_ans.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef VV_ANS_TEST_HOOKS
extern int vva_test_build_dec_direct_equivalence(
    const uint16_t norm[VVA_MAX_SYMBOL]);
#endif

static int tests_run = 0, tests_passed = 0;
#define TEST(n) do { tests_run++; fprintf(stderr, "  %-48s ", n); fflush(stderr); } while(0)
#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); } while(0)

static int test_ans_rt(const uint8_t *data, size_t len, const char *name) {
    TEST(name);
    if (len == 0) { PASS(); return 1; }

    size_t ecap = vva_bound(len);
    uint8_t *enc = (uint8_t *)malloc(ecap);
    uint8_t *dec = (uint8_t *)calloc(1, len + 16);
    if (!enc || !dec) { FAIL("malloc"); free(enc); free(dec); return 0; }

    size_t elen = 0;
    vva_error_t err = vva_encode(data, len, enc, ecap, &elen);
    if (err == VVA_ERR_OVERFLOW) {
        /* Incompressible — OK */
        PASS(); free(enc); free(dec); return 1;
    }
    if (err != VVA_OK) {
        char m[64]; snprintf(m, sizeof(m), "encode err %d", (int)err);
        FAIL(m); free(enc); free(dec); return 0;
    }

    size_t consumed = 0;
    err = vva_decode(enc, elen, dec, len, len, &consumed);
    if (err != VVA_OK) {
        char m[64]; snprintf(m, sizeof(m), "decode err %d (elen=%zu)", (int)err, elen);
        FAIL(m); free(enc); free(dec); return 0;
    }

    if (memcmp(data, dec, len) != 0) {
        for (size_t i = 0; i < len; i++) {
            if (data[i] != dec[i]) {
                char m[128]; snprintf(m, sizeof(m), "mismatch at %zu (exp %02x got %02x)",
                                       i, data[i], dec[i]);
                FAIL(m); free(enc); free(dec); return 0;
            }
        }
    }

    PASS(); free(enc); free(dec); return 1;
}

static void test_invalid_literal_tables(void) {
    TEST("Literal decoders reject invalid frequency totals");
    static const struct {
        size_t len;
        uint8_t bytes[11];
    } headers[] = {
        {2, {VVA_HDR_SPARSE, 0}},
        {5, {VVA_HDR_SPARSE, 1, 0, 1, 0}},
        {8, {VVA_HDR_SPARSE, 2, 0, 1, 0, 1, 1, 0}},
        {8, {VVA_HDR_SPARSE, 2, 0, 0, 16, 1, 1, 0}},
        {6, {VVA_HDR_DENSE, 1, 1, 0, 1, 0}},
        {11, {4, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0}}
    };
    for (size_t h = 0; h < sizeof(headers) / sizeof(headers[0]); h++) {
        uint8_t encoded[600] = {0}, decoded[4];
        size_t consumed = 0;
        memcpy(encoded, headers[h].bytes, headers[h].len);
        if (vva_decode(encoded, headers[h].len + 32, decoded, sizeof(decoded),
                       sizeof(decoded), &consumed) != VVA_ERR_CORRUPT ||
            vva_decode4(encoded, headers[h].len + 32, decoded, sizeof(decoded),
                        sizeof(decoded), &consumed) != VVA_ERR_CORRUPT) {
            FAIL("single/four-stream malformed table accepted"); return;
        }

        /* Context model with every context inherited from an invalid
         * global table; states and bitstream are otherwise in range. */
        memset(encoded, 0, sizeof(encoded));
        encoded[0] = (uint8_t)headers[h].len;
        memcpy(encoded + 2, headers[h].bytes, headers[h].len);
        memset(encoded + 2 + headers[h].len, 0xFF, 32);
        if (vva_decode_ctx(encoded, sizeof(encoded), decoded, sizeof(decoded),
                           sizeof(decoded), &consumed) != VVA_ERR_CORRUPT) {
            FAIL("context global malformed table accepted"); return;
        }

        /* Valid single-symbol global table, with context zero replaced
         * by the malformed table. This exercises per-context validation. */
        memset(encoded, 0, sizeof(encoded));
        encoded[0] = 2;
        encoded[2] = VVA_HDR_SINGLE;
        memset(encoded + 4, 0xFF, 32);
        encoded[4] = 0xFE;
        encoded[37] = (uint8_t)headers[h].len;
        memcpy(encoded + 39, headers[h].bytes, headers[h].len);
        if (vva_decode_ctx(encoded, sizeof(encoded), decoded, sizeof(decoded),
                           sizeof(decoded), &consumed) != VVA_ERR_CORRUPT) {
            FAIL("context local malformed table accepted"); return;
        }
    }
    PASS();
}

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt — ANS/tANS Codec Tests               ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════╝\n\n");

    test_invalid_literal_tables();

    /* 1. All zero */
    { uint8_t d[4096]; memset(d, 0, sizeof(d));
      test_ans_rt(d, sizeof(d), "All-zero 4096B"); }

    /* 2. All same */
    { uint8_t d[1000]; memset(d, 0x42, sizeof(d));
      test_ans_rt(d, sizeof(d), "All-same 1000B"); }

    /* 3. Uniform 256 */
    { uint8_t d[256*4]; for (int i=0; i<256*4; i++) d[i]=(uint8_t)(i&0xFF);
      test_ans_rt(d, sizeof(d), "Uniform 256×4"); }

    /* 4. Skewed ASCII */
    { const char *t = "The quick brown fox jumps over the lazy dog. "
                      "Pack my box with five dozen liquor jugs. ";
      size_t tl = strlen(t); size_t bl = tl*200;
      uint8_t *b = malloc(bl);
      for (size_t i=0; i<bl; i++) b[i]=(uint8_t)t[i%tl];
      test_ans_rt(b, bl, "Skewed ASCII 18KB");
      free(b); }

    /* 5. Single byte */
    { uint8_t d[1] = {0xAA}; test_ans_rt(d, 1, "Single byte"); }

    /* 6. Two symbols */
    { uint8_t d[1000]; for (int i=0;i<1000;i++) d[i]=(uint8_t)(i%2?0xAA:0x55);
      test_ans_rt(d, sizeof(d), "Two symbols 1000B"); }

    /* 7. Fuzz */
    { TEST("Fuzz (200 random, 1-8192B)");
      uint32_t rng = 271828; int ok = 1;
      for (int trial=0; trial<200 && ok; trial++) {
          rng = rng*1103515245+12345;
          size_t len = 1 + (rng>>16)%8192;
          uint8_t *d = malloc(len);
          for (size_t i=0; i<len; i++) { rng=rng*1103515245+12345;
              d[i] = (trial%3==0) ? (uint8_t)(32+(rng>>16)%95) : (uint8_t)(rng>>16); }
          size_t ecap = vva_bound(len);
          uint8_t *e = malloc(ecap);
          uint8_t *dec = calloc(1, len+16);
          size_t elen=0;
          vva_error_t err = vva_encode(d, len, e, ecap, &elen);
          if (err == VVA_OK) {
              size_t con=0;
              err = vva_decode(e, elen, dec, len, len, &con);
              if (err != VVA_OK || memcmp(d, dec, len) != 0) {
                  fprintf(stderr, "FAIL trial %d len=%zu err=%d\n", trial, len, (int)err);
                  ok = 0;
              }
          }
          free(d); free(e); free(dec);
      }
      if (ok) PASS();
    }

    /* 8. ANS vs Huffman ratio comparison */
    {
        TEST("ANS ratio vs raw (skewed data)");
        const char *t = "The quick brown fox jumps over the lazy dog. ";
        size_t tl = strlen(t); size_t bl = tl * 400;
        uint8_t *b = malloc(bl);
        for (size_t i = 0; i < bl; i++) b[i] = (uint8_t)t[i % tl];

        size_t ecap = vva_bound(bl);
        uint8_t *e = malloc(ecap);
        size_t elen = 0;
        vva_error_t err = vva_encode(b, bl, e, ecap, &elen);

        if (err == VVA_OK && elen < bl) {
            double ratio = (double)bl / (double)elen;
            char m[64]; snprintf(m, sizeof(m), "ratio=%.2f:1 (%.1f%% of raw)", ratio, 100.0*elen/bl);
            fprintf(stderr, "PASS (%s)\n", m);
            tests_passed++;
        } else {
            FAIL("ANS didn't compress");
        }
        free(b); free(e);
    }

#ifdef VV_ANS_TEST_HOOKS
    /* The decode-only table builder must be entry-for-entry identical to
     * the reference spread + build path; the state mapping is part of the
     * existing wire format. */
    {
        TEST("Direct decode-table builder equivalence");
        uint32_t rng = 0x9E3779B9u;
        int ok = 1;
        for (int trial = 0; trial < 256 && ok; trial++) {
            uint16_t norm[VVA_MAX_SYMBOL];
            uint8_t symbols[VVA_MAX_SYMBOL];
            memset(norm, 0, sizeof(norm));
            for (int i = 0; i < VVA_MAX_SYMBOL; i++) symbols[i] = (uint8_t)i;
            for (int i = VVA_MAX_SYMBOL - 1; i > 0; i--) {
                rng = rng * 1664525u + 1013904223u;
                int j = (int)(rng % (uint32_t)(i + 1));
                uint8_t tmp = symbols[i]; symbols[i] = symbols[j]; symbols[j] = tmp;
            }

            int active = 1 + trial % 64;
            uint32_t remaining = VVA_TABLE_SIZE;
            for (int i = 0; i < active - 1; i++) {
                uint32_t reserve = (uint32_t)(active - i - 1);
                rng = rng * 1664525u + 1013904223u;
                uint32_t freq = 1 + rng % (remaining - reserve);
                norm[symbols[i]] = (uint16_t)freq;
                remaining -= freq;
            }
            norm[symbols[active - 1]] = (uint16_t)remaining;
            if (!vva_test_build_dec_direct_equivalence(norm)) ok = 0;
        }
        if (ok) PASS(); else FAIL("decode tables differ");
    }
#endif

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Results: %d/%d passed\n", tests_passed, tests_run);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
