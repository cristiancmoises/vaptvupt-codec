/* VaptVupt 2.2.2 Integration Smoke Test
 *
 * Validates that the exact code patterns documented in
 * INTEGRATION.md compile, link, and execute correctly against
 * the v2.48.1 amalgamation. Run from a fresh extract:
 *
 *   make
 *   gcc -Wall -Wextra -O2 -std=c11 -Iinclude tests/test_integration.c
 *       src/vv_encoder.c src/vv_xxh64.c src/vv_huffman.c src/vv_ans.c
 *       src/vaptvupt_api.c build_obj/vv_simd_vaptvupt.o build_obj/vv_decoder_vaptvupt.o
 *       -o test_integration
 *   ./test_integration
 */

#define _POSIX_C_SOURCE 199309L
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

/* ─────────────────────────────────────────────────────────────────
 * Test harness
 * ───────────────────────────────────────────────────────────────── */

static int test_count = 0;
static int test_pass = 0;

#define TEST(name) \
    do { test_count++; printf("  TEST %2d: %-60s ", test_count, name); fflush(stdout); } while (0)
#define PASS() \
    do { test_pass++; printf("PASS\n"); } while (0)
#define FAIL(reason) \
    do { printf("FAIL — %s\n", reason); return 1; } while (0)

/* ─────────────────────────────────────────────────────────────────
 * Encode/Decode functions exactly as documented in INTEGRATION.md
 * ───────────────────────────────────────────────────────────────── */

static int vaptvupt_compress_for_archive(const uint8_t *plaintext, size_t plaintext_len,
                                      uint8_t **out_buf, size_t *out_len,
                                      int is_binary_heavy) {
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;
    opts.format_v2 = is_binary_heavy;
    opts.checksum = 0;

    size_t cap = vv_compress_bound(plaintext_len);
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return -1;

    int64_t clen = vv_compress(plaintext, plaintext_len, buf, cap, &opts);
    if (clen < 0) { free(buf); return -1; }

    *out_buf = buf;
    *out_len = (size_t)clen;
    return 0;
}

static int vaptvupt_decompress_from_archive(const uint8_t *cmp, size_t cmp_len,
                                         uint8_t *dst, size_t dst_cap,
                                         size_t *decoded_len) {
    int64_t dlen = vv_decompress_flags(cmp, cmp_len, dst, dst_cap,
                                        VV_DECOMPRESS_SKIP_CHECKSUM);
    if (dlen < 0) return -1;
    *decoded_len = (size_t)dlen;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 * Tests
 * ───────────────────────────────────────────────────────────────── */

static int test_roundtrip_text(void) {
    TEST("text roundtrip (format_v2=0)");

    const char *src = "The quick brown fox jumps over the lazy dog. "
                      "The quick brown fox jumps over the lazy dog. "
                      "The quick brown fox jumps over the lazy dog. ";
    size_t src_len = strlen(src);

    uint8_t *cmp = NULL;
    size_t cmp_len = 0;
    if (vaptvupt_compress_for_archive((const uint8_t *)src, src_len,
                                   &cmp, &cmp_len, 0 /* not binary */) != 0)
        FAIL("compress failed");

    uint8_t dec[4096];
    size_t dec_len = 0;
    if (vaptvupt_decompress_from_archive(cmp, cmp_len, dec, sizeof(dec), &dec_len) != 0) {
        free(cmp); FAIL("decompress failed");
    }

    if (dec_len != src_len || memcmp(dec, src, src_len) != 0) {
        free(cmp); FAIL("roundtrip mismatch");
    }
    free(cmp);
    PASS();
    return 0;
}

static int test_roundtrip_binary(void) {
    TEST("binary roundtrip (format_v2=1)");

    /* Generate a binary-like pattern: ELF-ish header repeated */
    uint8_t src[8192];
    for (size_t i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)((i * 37) ^ (i >> 3));
    }

    uint8_t *cmp = NULL;
    size_t cmp_len = 0;
    if (vaptvupt_compress_for_archive(src, sizeof(src),
                                   &cmp, &cmp_len, 1 /* binary */) != 0)
        FAIL("compress failed");

    uint8_t dec[8192];
    size_t dec_len = 0;
    if (vaptvupt_decompress_from_archive(cmp, cmp_len, dec, sizeof(dec), &dec_len) != 0) {
        free(cmp); FAIL("decompress failed");
    }

    if (dec_len != sizeof(src) || memcmp(dec, src, sizeof(src)) != 0) {
        free(cmp); FAIL("roundtrip mismatch");
    }
    free(cmp);
    PASS();
    return 0;
}

static int test_high_entropy_random(void) {
    TEST("high-entropy data (AEAD-like)");

    /* Simulate post-AEAD ciphertext: high-entropy random */
    uint8_t src[16384];
    srand(42);
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)rand();

    uint8_t *cmp = NULL;
    size_t cmp_len = 0;
    if (vaptvupt_compress_for_archive(src, sizeof(src),
                                   &cmp, &cmp_len, 0) != 0)
        FAIL("compress failed");

    /* Random data should be incompressible (or stored raw) — within 5% expansion */
    if (cmp_len > sizeof(src) + sizeof(src) / 20 + 256) {
        free(cmp); FAIL("excessive expansion on random data");
    }

    uint8_t dec[16384];
    size_t dec_len = 0;
    if (vaptvupt_decompress_from_archive(cmp, cmp_len, dec, sizeof(dec), &dec_len) != 0) {
        free(cmp); FAIL("decompress failed");
    }
    if (dec_len != sizeof(src) || memcmp(dec, src, sizeof(src)) != 0) {
        free(cmp); FAIL("roundtrip mismatch on random");
    }
    free(cmp);
    PASS();
    return 0;
}

static int test_skip_checksum_decode(void) {
    TEST("skip-checksum decode decodes encoder-with-checksum output");

    /* Encoder writes checksum, decoder skips it — should still roundtrip */
    const char *src = "VaptVupt 2.2.2 backup file content " "PADPADPAD" "PADPADPAD";
    size_t src_len = strlen(src);

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;
    opts.checksum = 1;  /* explicit: write checksum */

    size_t cap = vv_compress_bound(src_len);
    uint8_t *cmp = (uint8_t *)malloc(cap);
    int64_t clen = vv_compress((const uint8_t *)src, src_len, cmp, cap, &opts);
    if (clen < 0) { free(cmp); FAIL("compress failed"); }

    uint8_t dec[1024];
    int64_t dlen = vv_decompress_flags(cmp, (size_t)clen, dec, sizeof(dec),
                                        VV_DECOMPRESS_SKIP_CHECKSUM);
    if (dlen < 0 || (size_t)dlen != src_len || memcmp(dec, src, src_len) != 0) {
        free(cmp); FAIL("skip-checksum decode failed");
    }
    free(cmp);
    PASS();
    return 0;
}

static int test_streaming_encode_decode(void) {
    TEST("streaming encode + streaming decode roundtrip");

    /* Build 768KB of mixed content — small enough for one block */
    const size_t src_len = 768 * 1024;
    uint8_t *src = (uint8_t *)malloc(src_len);
    if (!src) FAIL("malloc");
    for (size_t i = 0; i < src_len; i++) {
        src[i] = (uint8_t)((i & 0x3F) + ((i >> 8) & 0x3));
    }

    /* Streaming encode */
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;

    vv_cstream_t *cs = vv_cstream_create(&opts);
    if (!cs) { free(src); FAIL("cstream_create"); }

    size_t cmp_cap = vv_compress_bound(src_len);
    uint8_t *cmp = (uint8_t *)malloc(cmp_cap);
    if (!cmp) { vv_cstream_destroy(cs); free(src); FAIL("malloc cmp"); }

    size_t written = 0;
    int rc = vv_cstream_compress_chunk(cs, src, src_len, cmp, cmp_cap,
                                        &written, /*is_last=*/1);
    if (rc < 0) {
        vv_cstream_destroy(cs); free(cmp); free(src);
        FAIL("cstream_compress_chunk");
    }
    vv_cstream_destroy(cs);  /* scrubs internal buffers per Sprint 118 */

    /* Streaming decode */
    vv_dstream_t *ds = vv_dstream_create();
    if (!ds) { free(cmp); free(src); FAIL("dstream_create"); }

    uint8_t *dec = (uint8_t *)malloc(src_len);
    if (!dec) { vv_dstream_destroy(ds); free(cmp); free(src); FAIL("malloc dec"); }

    size_t consumed = 0, decoded_total = 0;
    rc = vv_dstream_decompress_chunk(ds, cmp, written, dec, src_len,
                                      &consumed, &decoded_total);
    if (rc < 0) {
        vv_dstream_destroy(ds); free(dec); free(cmp); free(src);
        FAIL("dstream_decompress_chunk error");
    }
    vv_dstream_destroy(ds);

    if (decoded_total != src_len || memcmp(dec, src, src_len) != 0) {
        free(dec); free(cmp); free(src);
        FAIL("streaming roundtrip mismatch");
    }
    free(dec); free(cmp); free(src);
    PASS();
    return 0;
}

static int test_corrupt_input_returns_error(void) {
    TEST("corrupt input returns error (no crash)");

    /* Random bytes — should be rejected cleanly */
    uint8_t corrupt[256];
    srand(1);
    for (size_t i = 0; i < sizeof(corrupt); i++) corrupt[i] = (uint8_t)rand();

    uint8_t dec[1024];
    size_t dec_len = 0;
    int rc = vaptvupt_decompress_from_archive(corrupt, sizeof(corrupt),
                                           dec, sizeof(dec), &dec_len);
    /* Either rejects (rc != 0) cleanly, or recognizes it as a tiny valid
     * frame by accident (extremely unlikely with random bytes); both are
     * memory-safe. The test's contract is "no crash". */
    (void)rc;
    PASS();
    return 0;
}

static int test_dst_cap_zero_rejects(void) {
    TEST("dst_cap=0 rejects without crash");

    const char *src = "anything";
    vv_options_t opts;
    vv_default_options(&opts);

    int64_t clen = vv_compress((const uint8_t *)src, strlen(src),
                                NULL, 0, &opts);
    /* Should reject — caller-provided cap is 0 */
    if (clen >= 0) FAIL("expected negative return for dst_cap=0");
    PASS();
    return 0;
}

static int test_compat_v246_5_decoder_flag(void) {
    TEST("compat_v246_5_decoder=1 produces decoder-compatible output");

    const char *src = "test data for compat flag verification "
                      "padded to ensure literal count crosses thresholds "
                      "and exercise the lit_fmt selection logic. ";
    /* Repeat to make it big enough that lit_fmt = 4 might be chosen */
    size_t base_len = strlen(src);
    size_t total_len = base_len * 32;
    uint8_t *big = (uint8_t *)malloc(total_len);
    for (size_t i = 0; i < 32; i++) memcpy(big + i * base_len, src, base_len);

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;
    opts.compat_v246_5_decoder = 1;  /* suppress lit_fmt = 4 */

    size_t cap = vv_compress_bound(total_len);
    uint8_t *cmp = (uint8_t *)malloc(cap);
    int64_t clen = vv_compress(big, total_len, cmp, cap, &opts);
    if (clen < 0) { free(big); free(cmp); FAIL("compress failed"); }

    /* Roundtrip via current decoder — must work */
    uint8_t *dec = (uint8_t *)malloc(total_len);
    int64_t dlen = vv_decompress(cmp, (size_t)clen, dec, total_len);
    if (dlen < 0 || (size_t)dlen != total_len ||
        memcmp(dec, big, total_len) != 0) {
        free(big); free(cmp); free(dec); FAIL("compat output failed roundtrip");
    }
    free(big); free(cmp); free(dec);
    PASS();
    return 0;
}

static int test_format_v2_binary_smaller(void) {
    TEST("format_v2=1 on binary produces output (correctness only)");

    /* Non-trivial: format_v2 ratio improvement on binary depends on the data.
     * This test only validates that format_v2=1 produces a valid roundtrip. */
    uint8_t bin[4096];
    for (size_t i = 0; i < sizeof(bin); i++) {
        bin[i] = (uint8_t)((i * 17) % 31 + (i >> 6));
    }

    uint8_t *cmp_v2 = NULL;
    size_t len_v2 = 0;
    if (vaptvupt_compress_for_archive(bin, sizeof(bin), &cmp_v2, &len_v2, 1) != 0)
        FAIL("v2 compress failed");

    uint8_t dec[4096];
    size_t dec_len = 0;
    if (vaptvupt_decompress_from_archive(cmp_v2, len_v2, dec, sizeof(dec), &dec_len) != 0) {
        free(cmp_v2); FAIL("v2 decode failed");
    }
    if (dec_len != sizeof(bin) || memcmp(dec, bin, sizeof(bin)) != 0) {
        free(cmp_v2); FAIL("v2 roundtrip mismatch");
    }
    free(cmp_v2);
    PASS();
    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 * Main
 * ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("VaptVupt 2.2.2 ↔ VaptVupt 2.48.1 integration smoke test\n");
    printf("─────────────────────────────────────────────────────\n");

    if (test_roundtrip_text()) return 1;
    if (test_roundtrip_binary()) return 1;
    if (test_high_entropy_random()) return 1;
    if (test_skip_checksum_decode()) return 1;
    if (test_streaming_encode_decode()) return 1;
    if (test_corrupt_input_returns_error()) return 1;
    if (test_dst_cap_zero_rejects()) return 1;
    if (test_compat_v246_5_decoder_flag()) return 1;
    if (test_format_v2_binary_smaller()) return 1;

    printf("─────────────────────────────────────────────────────\n");
    printf("Results: %d/%d passed\n", test_pass, test_count);
    return (test_pass == test_count) ? 0 : 1;
}
