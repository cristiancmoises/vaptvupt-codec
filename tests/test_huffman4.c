/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_huffman4.c — Sprint 103 Phase A unit tests for vvh_encode4.
 *
 * Phase A produces encoder-only output. Production vvh_decode4 lands
 * in Phase B. To verify round-trip correctness now, we implement a
 * test-only inverse decoder local to this file.
 *
 * Test coverage:
 *   - Round-trip across 4-stream encoded output for various counts
 *   - Boundary cases: count % 4 in {0, 1, 2, 3}
 *   - Distributions: uniform, skewed, single-dominant
 *   - Activation threshold: src_len < 1024 returns OVERFLOW
 *   - Ratio impact: 4-stream output size vs 1-stream output size
 */

#include "vv_huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─── Test framework ─── */
static int total = 0, passed = 0;
#define TEST(name)  do { total++; printf("  TEST %2d: %s ... ", total, name); fflush(stdout); } while(0)
#define PASS()      do { passed++; printf("PASS\n"); } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); } while(0)

/* ═══════════════════════════════════════════════════════════════
 * TEST-ONLY INVERSE DECODER (Phase B will replace this with the
 * production vvh_decode4 in src/vv_huffman.c).
 *
 * Mirror image of vvh_encode4. Reads:
 *   [code-length header] [9B stream-size hdr] [4 streams]
 * and produces the original literal byte sequence.
 * ═══════════════════════════════════════════════════════════════ */

#define VVH_SYMS 256
#define MAX_CODE_LEN 15

/* Read header (mimics src/vv_huffman.c read_header). The format is:
 *   [1B max_symbol] [packed nibble code lengths]
 * For consistency with the production encoder, this test decoder
 * uses the same parsing logic.
 */
static int test_read_header(const uint8_t *src, size_t src_len,
                            uint8_t lengths[VVH_SYMS], size_t *hdr_sz_out) {
    if (src_len < 1) return 0;
    int max_sym = src[0];
    if (max_sym >= VVH_SYMS) return 0;
    memset(lengths, 0, VVH_SYMS);
    int packed_count = max_sym + 1;
    int packed_bytes = (packed_count + 1) / 2;
    if (1 + (size_t)packed_bytes > src_len) return 0;
    for (int i = 0; i <= max_sym; i++) {
        uint8_t b = src[1 + i / 2];
        /* Production layout (write_header in vv_huffman.c):
         *   even index i -> HIGH nibble of byte (1 + i/2)
         *   odd index i+1 -> LOW nibble */
        uint8_t nib = (i & 1) ? (b & 0x0F) : (b >> 4);
        lengths[i] = nib;
    }
    *hdr_sz_out = 1 + packed_bytes;
    return 1;
}

/* Build canonical codes from lengths (LSB-first, bit-reversed for
 * matching the production encoder's wire format). */
static int test_build_codes(const uint8_t lengths[VVH_SYMS],
                            uint16_t codes_rev[VVH_SYMS]) {
    /* Count symbols per length */
    int count[MAX_CODE_LEN + 1] = {0};
    for (int i = 0; i < VVH_SYMS; i++) {
        if (lengths[i] > MAX_CODE_LEN) return 0;
        count[lengths[i]]++;
    }
    count[0] = 0;
    /* Compute starting code per length */
    uint32_t next_code[MAX_CODE_LEN + 2];
    next_code[0] = 0;
    for (int len = 1; len <= MAX_CODE_LEN + 1; len++)
        next_code[len] = (next_code[len - 1] + count[len - 1]) << 1;
    /* Assign canonical code per symbol */
    for (int sym = 0; sym < VVH_SYMS; sym++) {
        int len = lengths[sym];
        if (len == 0) { codes_rev[sym] = 0; continue; }
        uint32_t code = next_code[len]++;
        /* Reverse bits to match LSB-first encoder */
        uint16_t rev = 0;
        for (int i = 0; i < len; i++)
            if (code & (1u << i)) rev |= (uint16_t)(1u << (len - 1 - i));
        codes_rev[sym] = rev;
    }
    return 1;
}

/* Bitstream reader (LSB-first, matches production br_t in vv_huffman.c) */
typedef struct {
    uint64_t bits;
    int nbits;
    const uint8_t *src;
    size_t pos;
    size_t len;
} testr_t;

static void testr_init(testr_t *r, const uint8_t *src, size_t len) {
    r->bits = 0; r->nbits = 0; r->src = src; r->pos = 0; r->len = len;
}
static void testr_refill(testr_t *r) {
    while (r->nbits <= 56 && r->pos < r->len) {
        r->bits |= (uint64_t)r->src[r->pos++] << r->nbits;
        r->nbits += 8;
    }
}

/* Decode one symbol via linear scan over codes (slow but simple — this
 * is a test-only path, not the production hot loop). */
static int test_decode_one(testr_t *r, const uint8_t lengths[VVH_SYMS],
                           const uint16_t codes_rev[VVH_SYMS], uint8_t *out) {
    if (r->nbits < MAX_CODE_LEN) testr_refill(r);
    /* Try each possible length, picking the symbol whose code matches */
    for (int len = 1; len <= MAX_CODE_LEN; len++) {
        if (r->nbits < len) return 0;
        uint32_t mask = (1u << len) - 1;
        uint32_t got = (uint32_t)(r->bits & mask);
        for (int sym = 0; sym < VVH_SYMS; sym++) {
            if (lengths[sym] == len && codes_rev[sym] == got) {
                r->bits >>= len;
                r->nbits -= len;
                *out = (uint8_t)sym;
                return 1;
            }
        }
    }
    return 0;
}

/* Test-only 4-stream decoder. Returns 1 on success, 0 on failure. */
static int test_decode4(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t num_literals) {
    uint8_t lengths[VVH_SYMS];
    uint16_t codes_rev[VVH_SYMS];
    size_t hdr_sz;
    if (!test_read_header(src, src_len, lengths, &hdr_sz)) return 0;
    if (!test_build_codes(lengths, codes_rev)) return 0;
    /* Read 9-byte stream-size header */
    if (hdr_sz + 9 > src_len) return 0;
    const uint8_t *sh = src + hdr_sz;
    size_t s1 = sh[0] | ((uint32_t)sh[1] << 8) | ((uint32_t)sh[2] << 16);
    size_t s2 = sh[3] | ((uint32_t)sh[4] << 8) | ((uint32_t)sh[5] << 16);
    size_t s3 = sh[6] | ((uint32_t)sh[7] << 8) | ((uint32_t)sh[8] << 16);
    size_t streams_off = hdr_sz + 9;
    if (streams_off > src_len) return 0;
    size_t total_streams = src_len - streams_off;
    if (s1 + s2 + s3 > total_streams) return 0;
    size_t s0 = total_streams - s1 - s2 - s3;
    /* Initialize 4 readers at correct offsets */
    testr_t r[4];
    testr_init(&r[0], src + streams_off, s0);
    testr_init(&r[1], src + streams_off + s0, s1);
    testr_init(&r[2], src + streams_off + s0 + s1, s2);
    testr_init(&r[3], src + streams_off + s0 + s1 + s2, s3);
    /* Per-stream literal counts (round-robin assignment): */
    /* size_t n[4] = { (num_literals+3)/4, (num_literals+2)/4,
                       (num_literals+1)/4, num_literals/4 }; */
    /* Round-robin output */
    for (size_t i = 0; i < num_literals; i++) {
        if (!test_decode_one(&r[i & 3], lengths, codes_rev, &dst[i])) return 0;
    }
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * TESTS
 * ═══════════════════════════════════════════════════════════════ */

static void fill_uniform(uint8_t *buf, size_t n, unsigned seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525 + 1013904223;
        buf[i] = (uint8_t)(s >> 24);
    }
}

/* English-text-like distribution (high frequency on space + lowercase) */
static void fill_textlike(uint8_t *buf, size_t n, unsigned seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525 + 1013904223;
        uint32_t r = (s >> 8) % 100;
        if (r < 18) buf[i] = ' ';
        else if (r < 28) buf[i] = 'e';
        else if (r < 36) buf[i] = 't';
        else if (r < 43) buf[i] = 'a';
        else if (r < 49) buf[i] = 'o';
        else if (r < 54) buf[i] = 'i';
        else if (r < 70) buf[i] = (uint8_t)('a' + (s % 26));
        else if (r < 95) buf[i] = (uint8_t)(' ' + (s % 95));
        else buf[i] = (uint8_t)(s & 0xFF);
    }
}

/* Single-dominant distribution: 90% one symbol */
static void fill_skewed(uint8_t *buf, size_t n, unsigned seed) {
    uint32_t s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525 + 1013904223;
        if ((s % 100) < 90) buf[i] = 0x42;
        else buf[i] = (uint8_t)(s >> 24);
    }
}

static int roundtrip_test(const char *name, const uint8_t *src, size_t src_len) {
    size_t bound = 129 + (src_len * 15 + 7) / 8 + 32;
    uint8_t *cmp = malloc(bound);
    uint8_t *dec = malloc(src_len + 16);
    if (!cmp || !dec) { free(cmp); free(dec); return 0; }
    size_t cmp_len = 0;
    vvh_error_t err = vvh_encode4(src, src_len, cmp, bound, &cmp_len);
    if (err == VVH_ERR_OVERFLOW) {
        /* Incompressible input: encoder correctly refused. This is a
         * legitimate outcome (e.g. uniform random data has ~8 bits/sym
         * entropy, no Huffman code can shrink it). Verify single-stream
         * also returns OVERFLOW for consistency. */
        size_t cmp1_len = 0;
        vvh_error_t e1 = vvh_encode(src, src_len, cmp, bound, &cmp1_len);
        if (e1 == VVH_ERR_OVERFLOW) {
            printf("[%s: incompressible — both 1strm and 4strm refused] ", name);
            free(cmp); free(dec); return 1;
        }
        printf("[encode4 returned OVERFLOW but 1strm succeeded — bug] ");
        free(cmp); free(dec); return 0;
    }
    if (err != VVH_OK) {
        printf("[encode err=%d] ", err);
        free(cmp); free(dec); return 0;
    }
    if (!test_decode4(cmp, cmp_len, dec, src_len)) {
        printf("[test decode failed] ");
        free(cmp); free(dec); return 0;
    }
    if (memcmp(src, dec, src_len) != 0) {
        printf("[mismatch] ");
        free(cmp); free(dec); return 0;
    }
    /* Also compare against single-stream Huffman size (for ratio reporting) */
    uint8_t *cmp1 = malloc(bound);
    size_t cmp1_len = 0;
    vvh_error_t e1 = vvh_encode(src, src_len, cmp1, bound, &cmp1_len);
    if (e1 == VVH_OK) {
        printf("[%s: 1strm=%zu, 4strm=%zu, Δ=%+.1f%%] ", name, cmp1_len, cmp_len,
               100.0 * ((double)cmp_len - (double)cmp1_len) / (double)cmp1_len);
    }
    free(cmp); free(cmp1); free(dec);
    return 1;
}

int main(void) {
    printf("=== Sprint 103 Phase A: vvh_encode4 unit tests ===\n");

    /* ─── 1. Activation threshold ─── */
    TEST("rejects src_len < 1024 (returns OVERFLOW)");
    {
        uint8_t src[100];
        uint8_t cmp[2000];
        size_t cmp_len = 0;
        memset(src, 'A', sizeof(src));
        vvh_error_t err = vvh_encode4(src, sizeof(src), cmp, sizeof(cmp), &cmp_len);
        if (err == VVH_ERR_OVERFLOW) PASS();
        else { char m[40]; snprintf(m, sizeof(m), "got %d (expected OVERFLOW)", err); FAIL(m); }
    }

    TEST("accepts src_len = 1024 (exactly threshold)");
    {
        uint8_t *src = malloc(1024);
        fill_textlike(src, 1024, 42);
        if (roundtrip_test("1024", src, 1024)) PASS();
        else FAIL("roundtrip failed");
        free(src);
    }

    /* ─── 2. Boundary cases (count modulo 4) ─── */
    for (int delta = 0; delta < 4; delta++) {
        size_t n = 4096 + delta;
        char name[32];
        snprintf(name, sizeof(name), "n=%zu (n%%4=%d)", n, delta);
        TEST(name);
        uint8_t *src = malloc(n);
        fill_textlike(src, n, 100 + delta);
        if (roundtrip_test(name, src, n)) PASS();
        else FAIL("roundtrip");
        free(src);
    }

    /* ─── 3. Distributions ─── */
    {
        size_t sizes[] = {1024, 4096, 16384, 65536};
        const char *dist_names[] = {"uniform", "textlike", "skewed"};
        void (*fillers[])(uint8_t *, size_t, unsigned) = {fill_uniform, fill_textlike, fill_skewed};
        for (int d = 0; d < 3; d++) {
            for (size_t si = 0; si < sizeof(sizes)/sizeof(sizes[0]); si++) {
                size_t n = sizes[si];
                char name[64];
                snprintf(name, sizeof(name), "%s n=%zu", dist_names[d], n);
                TEST(name);
                uint8_t *src = malloc(n);
                fillers[d](src, n, 1000 + (unsigned)si);
                if (roundtrip_test(name, src, n)) PASS();
                else FAIL("roundtrip");
                free(src);
            }
        }
    }

    /* ─── 4. Edge: extremely large input ─── */
    TEST("n=262144 (256 KB textlike)");
    {
        size_t n = 262144;
        uint8_t *src = malloc(n);
        fill_textlike(src, n, 9999);
        if (roundtrip_test("262144", src, n)) PASS();
        else FAIL("roundtrip");
        free(src);
    }

    /* ─── 5. Single-symbol input (degenerate Huffman tree) ─── */
    TEST("all-same-symbol (1024 × 'X')");
    {
        size_t n = 1024;
        uint8_t *src = malloc(n);
        memset(src, 'X', n);
        size_t bound = 129 + (n * 15 + 7) / 8 + 32;
        uint8_t *cmp = malloc(bound);
        size_t cmp_len = 0;
        /* For all-same-symbol input, the Huffman tree assigns length 0 or 1.
         * Our defensive check in vvh_encode4 returns CORRUPT if any encoded
         * symbol has length 0. Single-symbol files exercise this edge case.
         * The build_enc_table output for all-same-symbol may produce length=0
         * which is intentionally rejected. */
        vvh_error_t err = vvh_encode4(src, n, cmp, bound, &cmp_len);
        /* Either accepted (length=1 assigned) or cleanly rejected */
        if (err == VVH_OK || err == VVH_ERR_CORRUPT || err == VVH_ERR_OVERFLOW) PASS();
        else { char m[40]; snprintf(m, sizeof(m), "unexpected err=%d", err); FAIL(m); }
        free(src); free(cmp);
    }

    /* ─── 6. Output size is smaller than 1-stream baseline by less than X% ─── */
    TEST("4-stream output ratio penalty <2% on textlike 16KB");
    {
        size_t n = 16384;
        uint8_t *src = malloc(n);
        fill_textlike(src, n, 7777);
        size_t bound = 129 + (n * 15 + 7) / 8 + 32;
        uint8_t *cmp1 = malloc(bound);
        uint8_t *cmp4 = malloc(bound);
        size_t l1 = 0, l4 = 0;
        vvh_error_t e1 = vvh_encode(src, n, cmp1, bound, &l1);
        vvh_error_t e4 = vvh_encode4(src, n, cmp4, bound, &l4);
        if (e1 == VVH_OK && e4 == VVH_OK) {
            double overhead = 100.0 * ((double)l4 - (double)l1) / (double)l1;
            printf("[1strm=%zu, 4strm=%zu, +%.2f%%] ", l1, l4, overhead);
            /* 4-stream has 9 byte hdr + ~3 bytes alignment slop = 12 bytes
             * over 1-stream. For 16KB input compressing to ~8-12KB, this is
             * 0.1-0.15%. <2% is generous. */
            if (overhead < 2.0) PASS();
            else { char m[40]; snprintf(m, sizeof(m), "overhead %.2f%% > 2%%", overhead); FAIL(m); }
        } else { FAIL("encode error"); }
        free(src); free(cmp1); free(cmp4);
    }

    printf("\nResults: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
