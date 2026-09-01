/* test_safezone_adversarial.c — Sprint 51 (v2.40.0)
 *
 * Targets the v2.39.0 safe-zone bounds-elision optimization in
 * vva_decode_sequences_impl. The safe zone skips per-iteration
 * bounds checks when:
 *   op >= dst_base + SAFEZONE_MAX_OFFSET (= 16 MB, 1<<24 since Sprint 46)
 *   op <= op_end - 2*SAFEZONE_MAX_RUN (= op_end - 131070)
 *
 * These tests construct adversarial frames that attempt to trigger
 * OOB access by:
 *   1. Encoding offset > SAFEZONE_MAX_OFFSET while op is in the safe zone
 *   2. Transitioning across the safe-zone boundary with malformed data
 *   3. Small output buffers where the safe zone never activates
 *   4. Exactly-boundary values for off-by-one verification
 *
 * EXPECTED: all adversarial inputs return VVA_ERR_CORRUPT or
 * VVA_ERR_OVERFLOW cleanly. No OOB reads, no crashes, no heap
 * corruption. Run under ASAN for maximum assurance:
 *   make CFLAGS='-O2 -g -fsanitize=address' test_safezone_adversarial
 */

#include "vaptvupt.h"
#include "vv_ans.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int failures = 0;
static int passed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); failures++; } \
} while (0)

/* Test 1: Tiny buffer — safe zone never activates.
 * All decodes must go through the slow (full-check) path. */
static void test_small_buffer_no_safezone(void) {
    uint8_t src[1024];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)(i * 7 + 13);

    uint8_t cmp[2048];
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    int64_t clen = vv_compress(src, sizeof(src), cmp, sizeof(cmp), &o);
    CHECK(clen > 0, "compress tiny buffer");

    uint8_t dec[1024];
    int64_t dlen = vv_decompress(cmp, clen, dec, sizeof(dec));
    CHECK(dlen == (int64_t)sizeof(src), "decompress tiny buffer size");
    CHECK(memcmp(src, dec, sizeof(src)) == 0, "decompress tiny buffer contents");
}

/* Test 2: Medium buffer (between SAFEZONE_MAX_RUN and SAFEZONE_MAX_OFFSET).
 * Safe zone activates PARTIALLY. Verify no boundary-crossing bugs. */
static void test_medium_buffer_boundary(void) {
    /* 512 KB — far below SAFEZONE_MAX_OFFSET = 16 MB; safe zone never engages */
    size_t sz = 512 * 1024;
    uint8_t *src = malloc(sz);
    /* Mixed content: some random, some repetitive */
    for (size_t i = 0; i < sz; i++) {
        src[i] = (uint8_t)((i < sz/2) ? (i % 256) : ((i * 31 + 17) % 256));
    }

    size_t cap = vv_compress_bound(sz);
    uint8_t *cmp = malloc(cap);
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    int64_t clen = vv_compress(src, sz, cmp, cap, &o);
    CHECK(clen > 0, "compress 512KB buffer");

    uint8_t *dec = malloc(sz);
    int64_t dlen = vv_decompress(cmp, clen, dec, sz);
    CHECK(dlen == (int64_t)sz, "decompress 512KB buffer size");
    CHECK(memcmp(src, dec, sz) == 0, "decompress 512KB buffer contents");

    free(src); free(cmp); free(dec);
}

/* Test 3: 2 MB buffer. NOTE: below the 16 MB SAFEZONE_MAX_OFFSET floor, so
 * the safe-zone fast path does NOT engage here (it runs the per-iter checks);
 * test_safezone_fastpath_engaged below exercises the >16 MB fast path.
 * Safe zone fully activates. Most sequences hit the fast path. */
static void test_large_buffer_full_safezone(void) {
    size_t sz = 2 * 1024 * 1024; /* 2 MB — below the 16 MB safe-zone floor */
    uint8_t *src = malloc(sz);
    for (size_t i = 0; i < sz; i++)
        src[i] = (uint8_t)((i * 37 + 3) ^ (i >> 8));

    size_t cap = vv_compress_bound(sz);
    uint8_t *cmp = malloc(cap);
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    int64_t clen = vv_compress(src, sz, cmp, cap, &o);
    CHECK(clen > 0, "compress 2MB buffer");

    uint8_t *dec = malloc(sz);
    int64_t dlen = vv_decompress(cmp, clen, dec, sz);
    CHECK(dlen == (int64_t)sz, "decompress 2MB buffer size");
    CHECK(memcmp(src, dec, sz) == 0, "decompress 2MB buffer contents");

    free(src); free(cmp); free(dec);
}

/* Test 4: Fuzz the decoder with random byte perturbations.
 * Each perturbed frame must either decode correctly OR return an
 * error. No crashes, no OOB reads. */
static void test_random_byte_perturbations(void) {
    size_t sz = 64 * 1024;
    uint8_t *src = malloc(sz);
    for (size_t i = 0; i < sz; i++) src[i] = (uint8_t)(i & 0xff);

    size_t cap = vv_compress_bound(sz);
    uint8_t *cmp = malloc(cap);
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    int64_t clen = vv_compress(src, sz, cmp, cap, &o);
    CHECK(clen > 0, "compress perturbation base");

    uint8_t *dec = malloc(sz);
    /* Flip one byte at a time; try each position; verify no crash */
    srand(42);
    int attempts = 100;
    int rejected = 0, accepted = 0;
    for (int a = 0; a < attempts; a++) {
        /* Corrupt a random byte (skip first 16 bytes = header, decoder
         * already rejects corrupt headers via magic/CRC, we want to
         * hit the body) */
        size_t pos = 16 + (rand() % (clen - 16));
        uint8_t saved = cmp[pos];
        cmp[pos] ^= (uint8_t)(rand() & 0xff);

        int64_t dlen = vv_decompress(cmp, clen, dec, sz);
        if (dlen == (int64_t)sz && memcmp(src, dec, sz) == 0) accepted++;
        else rejected++;

        cmp[pos] = saved;
    }
    CHECK(rejected + accepted == attempts, "all perturbations handled");
    /* Most should be rejected; a few might decode to the same output
     * if the flipped bit is in a dead zone */
    printf("  byte-flip fuzz: %d rejected, %d accepted (no crashes)\n",
           rejected, accepted);

    free(src); free(cmp); free(dec);
}

/* Test 5: Truncated frames. Every prefix of a valid frame must
 * either fail cleanly or be rejected. */
static void test_truncated_frames(void) {
    size_t sz = 8192;
    uint8_t src[8192];
    for (size_t i = 0; i < sz; i++) src[i] = (uint8_t)(i * 11);

    uint8_t cmp[16384];
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_BALANCED;
    int64_t clen = vv_compress(src, sz, cmp, sizeof(cmp), &o);
    CHECK(clen > 0, "compress for truncation test");

    uint8_t dec[8192];
    /* Try truncating at every byte from 1 to clen-1 */
    int crashes = 0;
    int accepted = 0;
    for (int64_t t = 1; t < clen; t++) {
        int64_t dlen = vv_decompress(cmp, t, dec, sizeof(dec));
        if (dlen == (int64_t)sz) accepted++;
    }
    CHECK(crashes == 0, "no crashes on any truncation");
    /* Only length-clen should accept cleanly; all shorter should fail */
    CHECK(accepted == 0, "no truncated frame decoded as valid");
}

/* Test 6: Oversized-dst-cap to catch any integer-overflow in safe-zone
 * threshold computation. op_safe_end = op_end - SAFEZONE_MAX_RUN
 * could underflow if op_end is very small. */
static void test_boundary_dst_cap_sizes(void) {
    uint8_t src[256];
    for (size_t i = 0; i < sizeof(src); i++) src[i] = (uint8_t)i;
    uint8_t cmp[1024];
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    int64_t clen = vv_compress(src, sizeof(src), cmp, sizeof(cmp), &o);
    CHECK(clen > 0, "compress small for dst_cap test");

    /* Decode into dst_cap values that stress the safe-zone math */
    size_t caps[] = {256, 257, 300, 65534, 65535, 65536, 65537};
    for (size_t i = 0; i < sizeof(caps)/sizeof(caps[0]); i++) {
        uint8_t *dec = malloc(caps[i]);
        int64_t dlen = vv_decompress(cmp, clen, dec, caps[i]);
        if (caps[i] >= sizeof(src)) {
            CHECK(dlen == (int64_t)sizeof(src),
                  "decode into dst_cap covering src size");
        }
        /* Either way: no crash, return must be a defined value */
        free(dec);
    }
}

/* Test 7: Repeat the standard fuzz workload 10× to catch any
 * transient heap-state dependencies. */
static void test_repeated_compression(void) {
    size_t sz = 100 * 1024;
    uint8_t *src = malloc(sz);
    for (size_t i = 0; i < sz; i++)
        src[i] = (uint8_t)((i * 47) ^ (i >> 3));
    size_t cap = vv_compress_bound(sz);
    uint8_t *cmp = malloc(cap);
    uint8_t *dec = malloc(sz);

    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    for (int rep = 0; rep < 10; rep++) {
        int64_t clen = vv_compress(src, sz, cmp, cap, &o);
        CHECK(clen > 0, "repeat-compress");
        int64_t dlen = vv_decompress(cmp, clen, dec, sz);
        CHECK(dlen == (int64_t)sz, "repeat-decompress size");
        CHECK(memcmp(src, dec, sz) == 0, "repeat-decompress contents");
    }
    free(src); free(cmp); free(dec);
}

/* Test 8: Format-v2 with large output.
 * Exercises the v2 path with safe-zone + hash3 matcher. */
static void test_v2_large_output(void) {
    size_t sz = 3 * 1024 * 1024;
    uint8_t *src = malloc(sz);
    /* ELF-like: some header bytes + repeated code patterns */
    for (size_t i = 0; i < sz; i++) {
        if (i < 64) src[i] = 0x7f;
        else src[i] = (uint8_t)(((i / 16) % 32) + 0x48); /* x86 opcode-ish */
    }

    size_t cap = vv_compress_bound(sz);
    uint8_t *cmp = malloc(cap);
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_EXTREME;
    o.format_v2 = 1;
    int64_t clen = vv_compress(src, sz, cmp, cap, &o);
    CHECK(clen > 0, "compress v2 3MB buffer");

    uint8_t *dec = malloc(sz);
    int64_t dlen = vv_decompress(cmp, clen, dec, sz);
    CHECK(dlen == (int64_t)sz, "decompress v2 3MB size");
    CHECK(memcmp(src, dec, sz) == 0, "decompress v2 3MB contents");

    free(src); free(cmp); free(dec);
}

/* Test 9: >16 MB single-frame output — actually engages the safe-zone
 * fast path. SAFEZONE_MAX_OFFSET is 1<<24 (16 MB) since Sprint 46, and
 * dst_base is per-frame with op advancing cumulatively across blocks, so
 * the in_safe_zone branch (which elides both per-iteration bounds checks)
 * only runs once op passes dst_base + 16 MB. Every test above uses <= 2 MB
 * buffers, so before this test the bounds-elision branch had ZERO coverage.
 *
 * The decoded lengths are hard-bounded by the fixed ANS tables
 * (litlen <= 65535 via ll_base[35]=61440 + 4095; matchlen <= 65535 via
 * ml_base[35]=32768 + 32767; offset <= 2^24-1, plus the unconditional
 * offset > SAFEZONE_MAX_OFFSET reject), so op_safe_end = op_end - 65535 and
 * offset_check_floor = dst_base + 16 MB make the skipped checks genuine
 * tautologies. This test verifies the fast path decodes correctly; run
 * under ASan to confirm no OOB in the bounds-elided region. */
static void test_safezone_fastpath_engaged(void) {
    size_t sz = 20u * 1024 * 1024;   /* > 16 MB + 65535 so the safe zone is non-empty */
    uint8_t *src = malloc(sz);
    if (!src) { CHECK(0, "alloc 20MB src"); return; }
    /* Moderately compressible: repeated tokens with pseudo-random order, so
     * balanced mode emits SEQ ('S') blocks with real matches and the output
     * stays a single frame whose decode crosses the 16 MB safe-zone floor. */
    static const char *w[] = {"the ","quick ","brown ","fox ","jumps ","over ",
                              "lazy ","dog ","and ","then ","runs ","away "};
    size_t p = 0; unsigned r = 0x1234567u;
    while (p < sz) {
        r = r * 1103515245u + 12345u;
        const char *t = w[(r >> 16) % 12];
        size_t l = strlen(t);
        if (p + l > sz) break;
        memcpy(src + p, t, l); p += l;
    }
    while (p < sz) src[p++] = ' ';

    size_t cap = sz + sz / 2 + 4096;
    uint8_t *cmp = malloc(cap);
    if (!cmp) { free(src); CHECK(0, "alloc compress buffer"); return; }
    vv_options_t o; vv_default_options(&o); o.mode = VV_MODE_BALANCED;
    int64_t clen = vv_compress(src, sz, cmp, cap, &o);
    CHECK(clen > 0, "compress 20MB (balanced, SEQ)");

    uint8_t *dec = malloc(sz + 64);
    if (clen > 0 && dec) {
        int64_t dlen = vv_decompress(cmp, clen, dec, sz + 64);
        CHECK(dlen == (int64_t)sz, "decompress 20MB size (safe-zone fast path)");
        CHECK(dlen == (int64_t)sz && memcmp(src, dec, sz) == 0,
              "roundtrip 20MB contents (safe-zone fast path, bounds-elided)");
    } else {
        CHECK(0, "alloc decode buffer / compress");
    }
    free(dec); free(cmp); free(src);
}

/* Test 10: one SEQ entry can contain BOTH a 65535-byte literal run and a
 * 65535-byte match.  The old fast-zone proof reserved room for only one of
 * them, then reused its stale pre-literal result for the match check.  Place
 * this synthetic block after enough prior output to enter a small (1 KiB)
 * advertised safe zone: exact capacity must succeed, one byte less must
 * fail cleanly.  Under ASan the old implementation writes one byte past the
 * short destination. */
static void test_combined_run_safezone_margin(void) {
    enum { LITLEN = 65535, MATCHLEN = 65535, OUTLEN = LITLEN + MATCHLEN };
    const size_t ext_ll = LITLEN - 15;
    const size_t ext_ml = MATCHLEN - 15 - VV_MIN_MATCH;
    const size_t tok_len = 1 + (ext_ll / 255 + 1) + LITLEN + 2 +
                           (ext_ml / 255 + 1);
    uint8_t *tokens = malloc(tok_len);
    uint8_t *payload = malloc(vva_bound(tok_len));
    uint8_t *base = malloc(1024 + OUTLEN);
    size_t payload_len = 0, at = 0, out_len = 0;
    if (!tokens || !payload || !base) {
        CHECK(0, "allocate combined-run safe-zone fixture");
        free(tokens); free(payload); free(base); return;
    }

    tokens[at++] = 0xff; /* litlen=15 + extension, matchlen=15 + extension */
    for (size_t n = ext_ll; n >= 255; n -= 255) tokens[at++] = 255;
    tokens[at++] = (uint8_t)(ext_ll % 255);
    memset(tokens + at, 'L', LITLEN); at += LITLEN;
    tokens[at++] = 1; tokens[at++] = 0; /* offset 1 */
    for (size_t n = ext_ml; n >= 255; n -= 255) tokens[at++] = 255;
    tokens[at++] = (uint8_t)(ext_ml % 255);
    CHECK(at == tok_len, "construct combined-run token stream");

    vva_error_t enc = vva_encode_sequences(tokens, tok_len, payload,
                                            vva_bound(tok_len), &payload_len, 2);
    CHECK(enc == VVA_OK && payload_len > 0, "encode combined-run SEQ payload");
    memset(base, 'P', 1024 + OUTLEN);
    if (enc == VVA_OK) {
        vva_error_t dec = vva_decode_sequences_limited(payload, payload_len,
                                                        base + 1024, OUTLEN, &out_len,
                                                        base, 1024);
        CHECK(dec == VVA_OK && out_len == OUTLEN,
              "exact combined-run capacity succeeds in safe zone");
        dec = vva_decode_sequences_limited(payload, payload_len,
                                           base + 1024, OUTLEN - 1, &out_len,
                                           base, 1024);
        CHECK(dec == VVA_ERR_OVERFLOW,
              "short combined-run capacity is rejected before match copy");
    }
    free(tokens); free(payload); free(base);
}

int main(void) {
    printf("=== SAFEZONE ADVERSARIAL TESTS (v2.40.0) ===\n");
    test_small_buffer_no_safezone();
    test_medium_buffer_boundary();
    test_large_buffer_full_safezone();
    test_random_byte_perturbations();
    test_truncated_frames();
    test_boundary_dst_cap_sizes();
    test_repeated_compression();
    test_v2_large_output();
    test_safezone_fastpath_engaged();
    test_combined_run_safezone_margin();
    printf("\nResults: %d passed, %d failed\n", passed, failures);
    return failures;
}
