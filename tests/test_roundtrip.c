/*
 * VaptVupt — Unit tests: round-trip correctness
 *
 * Tests:
 *   1. Empty input
 *   2. Single byte
 *   3. Small text (Hello World)
 *   4. Repeated data (highly compressible)
 *   5. Random binary (incompressible)
 *   6. All byte values (256 unique bytes)
 *   7. Large data (1 MB)
 *   8. All three modes
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stderr, "  %-40s ", name); \
    fflush(stderr); \
} while(0)

#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); } while(0)

static int test_roundtrip(const uint8_t *data, size_t len, vv_mode_t mode, const char *name) {
    char label[128];
    const char *mnames[] = {"fast", "balanced", "extreme"};
    snprintf(label, sizeof(label), "%s (%s, %zu bytes)", name, mnames[mode], len);
    TEST(label);

    size_t comp_cap = vv_compress_bound(len);
    uint8_t *comp = (uint8_t *)malloc(comp_cap);
    if (!comp) { FAIL("malloc comp"); return 0; }

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = mode;

    int64_t comp_size = vv_compress(data, len, comp, comp_cap, &opts);
    if (comp_size < 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "compress error %lld", (long long)comp_size);
        FAIL(msg); free(comp); return 0;
    }

    /* Decompress */
    size_t dec_cap = len + 64;  /* +64 for SIMD slack */
    uint8_t *dec = (uint8_t *)calloc(1, dec_cap + 64);
    if (!dec) { FAIL("malloc dec"); free(comp); return 0; }

    int64_t dec_size = vv_decompress(comp, (size_t)comp_size, dec, dec_cap);
    if (dec_size < 0) {
        char msg[64]; snprintf(msg, sizeof(msg), "decompress error %lld", (long long)dec_size);
        FAIL(msg); free(comp); free(dec); return 0;
    }

    if ((size_t)dec_size != len) {
        char msg[128];
        snprintf(msg, sizeof(msg), "size mismatch: expected %zu got %lld", len, (long long)dec_size);
        FAIL(msg); free(comp); free(dec); return 0;
    }

    if (len > 0 && memcmp(data, dec, len) != 0) {
        /* Find first mismatch */
        size_t i;
        for (i = 0; i < len; i++) if (data[i] != dec[i]) break;
        char msg[128];
        snprintf(msg, sizeof(msg), "data mismatch at byte %zu (expected 0x%02x got 0x%02x)",
                 i, data[i], dec[i]);
        FAIL(msg); free(comp); free(dec); return 0;
    }

    PASS();
    free(comp);
    free(dec);
    return 1;
}

/* A reset must apply the new format to both the block writer and matcher.
 * Long matches exercise v2's 65534-byte cap, while the record fixture
 * exercises its three-byte hash table across repeated enable/disable cycles. */
static void test_cstream_format_reset(vv_mode_t mode, int records) {
    char label[128];
    snprintf(label, sizeof(label), "Stream format reset (%s, %s)",
             mode == VV_MODE_BALANCED ? "balanced" : "extreme",
             records ? "records" : "long matches");
    TEST(label);
    size_t len = records ? 16384 : 262144;
    size_t cap = vv_compress_bound(len);
    uint8_t *src = (uint8_t *)malloc(len);
    uint8_t *reused = (uint8_t *)malloc(cap);
    uint8_t *fresh = (uint8_t *)malloc(cap);
    uint8_t *decoded = (uint8_t *)malloc(len);
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = mode;
    vv_cstream_t *stream = NULL;
    const char *error = NULL;
    if (!src || !reused || !fresh || !decoded) {
        error = "allocation failed";
        goto done;
    }
    for (size_t i = 0; i < len; i++) {
        if (records)
            src[i] = (uint8_t)((i % 4 == 3) ? (i / 4) : (i % 4 + i / 1024));
        else
            src[i] = (uint8_t)(i % 43);
    }
    stream = vv_cstream_create(&opts);
    if (!stream) { error = "stream create failed"; goto done; }
    /* Finish by switching fast <-> the entropy mode while retaining the
     * v2 request. Reused contexts must resolve it like freshly created ones. */
    const int formats[] = {1, 0, 1, 0, 1, 1, 1, 1};
    for (size_t pass = 0; pass < sizeof(formats) / sizeof(formats[0]); pass++) {
        opts.mode = pass >= 4 && !(pass & 1) ? VV_MODE_ULTRA_FAST : mode;
        opts.format_v2 = (uint8_t)formats[pass];
        if (vv_cstream_reset(stream, &opts) != VV_OK) {
            error = "stream reset failed";
            goto done;
        }
        vv_cstream_t *reference = vv_cstream_create(&opts);
        if (!reference) { error = "reference create failed"; goto done; }
        size_t reused_len = 0, fresh_len = 0;
        int reused_err = vv_cstream_compress_chunk(stream, src, len, reused,
                                                  cap, &reused_len, 1);
        int fresh_err = vv_cstream_compress_chunk(reference, src, len, fresh,
                                                 cap, &fresh_len, 1);
        vv_cstream_destroy(reference);
        if (reused_err != VV_OK || fresh_err != VV_OK) {
            error = "stream compress failed";
            goto done;
        }
        if (vv_decompress(reused, reused_len, decoded, len) != (int64_t)len ||
            memcmp(src, decoded, len) != 0) {
            error = "reset format roundtrip failed";
            goto done;
        }
        if (reused_len != fresh_len || memcmp(reused, fresh, fresh_len) != 0) {
            error = "reset format differs from fresh context";
            goto done;
        }
    }
done:
    vv_cstream_destroy(stream);
    free(src); free(reused); free(fresh); free(decoded);
    if (error) { FAIL(error); } else { PASS(); }
}

/* Streaming retains the fully initialized matcher. For a single v1 chunk,
 * its block/footer bytes must equal the one-shot small-input setup, including
 * boundary hashes and circular chains narrower than the advertised window. */
static void test_small_fast_setup(uint8_t window_log) {
    char label[96];
    snprintf(label, sizeof(label), "Small fast setup vs full matcher (w=%u)",
             (unsigned)window_log);
    TEST(label);
    static const size_t sizes[] = {
        0, 1, 2, 3, 4, 5, 6, 14, 15, 16, 18, 19, 20, 31, 32, 33,
        255, 256, 257, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097
    };
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_ULTRA_FAST;
    opts.window_log = window_log;
    vv_cstream_t *stream = vv_cstream_create(&opts);
    if (!stream) { FAIL("stream create failed"); return; }
    const char *error = NULL;
    for (int kind = 0; kind < 3 && !error; kind++) {
        opts.depth_override = kind ? 9 : 0;
        opts.accel = kind ? 64 : 0;
        opts.no_rep = kind ? 1 : 0;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            size_t n = sizes[s], cap = vv_compress_bound(n);
            uint8_t *src = (uint8_t *)malloc(n ? n : 1);
            uint8_t *one = (uint8_t *)malloc(cap);
            uint8_t *full = (uint8_t *)malloc(cap);
            uint8_t *decoded = (uint8_t *)malloc(n ? n : 1);
            if (!src || !one || !full || !decoded) {
                error = "allocation failed";
            } else {
                uint32_t rng = 1234567;
                for (size_t i = 0; i < n; i++) {
                    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                    src[i] = kind == 1 ? (uint8_t)rng : kind == 2 ? (uint8_t)(i % 43) :
                        (uint8_t)(i % 11 == 0 ? rng : i % 43);
                }
                size_t full_len = 0;
                int64_t one_len = vv_compress(src, n, one, cap, &opts);
                if (one_len < (int64_t)sizeof(vv_frame_header_t) ||
                    vv_cstream_reset(stream, &opts) != VV_OK ||
                    vv_cstream_compress_chunk(stream, src, n, full, cap,
                                              &full_len, 1) != VV_OK) {
                    error = "compression failed";
                } else if ((size_t)one_len != full_len ||
                           memcmp(one + sizeof(vv_frame_header_t),
                                  full + sizeof(vv_frame_header_t),
                                  full_len - sizeof(vv_frame_header_t)) != 0) {
                    /* Only content_size in the frame header differs. */
                    error = "small setup changed encoded block bytes";
                } else if (vv_decompress(one, (size_t)one_len, decoded, n) !=
                           (int64_t)n || memcmp(src, decoded, n) != 0) {
                    error = "small setup roundtrip failed";
                }
                if (!error) {
                    /* FAST cannot emit T tags, so requesting v2 must retain
                     * the same v1 tokens. The 255-byte periodic case used
                     * to encode a +3 match bias that the decoder read as +4. */
                    opts.format_v2 = 1;
                    one_len = vv_compress(src, n, one, cap, &opts);
                    opts.format_v2 = 0;
                    if (one_len < 0 ||
                        vv_decompress(one, (size_t)one_len, decoded, n) !=
                        (int64_t)n || memcmp(src, decoded, n) != 0)
                        error = "small v2 setup roundtrip failed";
                    else if ((size_t)one_len != full_len ||
                             memcmp(one + sizeof(vv_frame_header_t),
                                    full + sizeof(vv_frame_header_t),
                                    full_len - sizeof(vv_frame_header_t)) != 0)
                        error = "FAST v2 request changed plain token output";
                }
            }
            free(src); free(one); free(full); free(decoded);
            if (error) break;
        }
    }
    vv_cstream_destroy(stream);
    if (error) { FAIL(error); } else { PASS(); }
}

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt v%s — Unit Tests              ║\n", VV_VERSION_STRING);
    fprintf(stderr, "╚════════════════════════════════════════════╝\n\n");

    vv_mode_t modes[] = {VV_MODE_ULTRA_FAST, VV_MODE_BALANCED, VV_MODE_EXTREME};

    for (int m = 0; m < 3; m++) {
        /* Test 1: Empty */
        test_roundtrip((const uint8_t *)"", 0, modes[m], "Empty");

        /* Test 2: Single byte */
        test_roundtrip((const uint8_t *)"X", 1, modes[m], "Single byte");

        /* Test 3: Small text */
        {
            const char *text = "Hello, World! VaptVupt compression test.";
            test_roundtrip((const uint8_t *)text, strlen(text), modes[m], "Small text");
        }

        /* Test 4: Repeated data */
        {
            uint8_t rep[4096];
            memset(rep, 'A', sizeof(rep));
            for (int i = 0; i < 4096; i += 10) rep[i] = 'B';
            test_roundtrip(rep, sizeof(rep), modes[m], "Repeated data");
        }

        /* Test 5: Random binary */
        {
            uint8_t rnd[8192];
            uint32_t rng = 12345;
            for (int i = 0; i < 8192; i++) {
                rng = rng * 1103515245 + 12345;
                rnd[i] = (uint8_t)(rng >> 16);
            }
            test_roundtrip(rnd, sizeof(rnd), modes[m], "Random binary");
        }

        /* Test 6: All byte values */
        {
            uint8_t all[256];
            for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
            test_roundtrip(all, sizeof(all), modes[m], "All byte values");
        }

        /* Test 7: Larger data with structure */
        {
            size_t big_len = 100000;
            uint8_t *big = (uint8_t *)malloc(big_len);
            if (big) {
                for (size_t i = 0; i < big_len; i++) {
                    big[i] = (uint8_t)("The quick brown fox jumps over the lazy dog. "[i % 46]);
                }
                test_roundtrip(big, big_len, modes[m], "100KB structured");
                free(big);
            }
        }

        /* Test 8: Sparse (mostly zeros) */
        {
            uint8_t sparse[16384];
            memset(sparse, 0, sizeof(sparse));
            uint32_t rng = 99;
            for (int i = 0; i < 100; i++) {
                rng = rng * 1103515245 + 12345;
                sparse[(rng >> 16) % 16384] = (uint8_t)(rng & 0xFF);
            }
            test_roundtrip(sparse, sizeof(sparse), modes[m], "Sparse (mostly zeros)");
        }
    }

    for (int mode = VV_MODE_BALANCED; mode <= VV_MODE_EXTREME; mode++) {
        test_cstream_format_reset((vv_mode_t)mode, 0);
        test_cstream_format_reset((vv_mode_t)mode, 1);
    }

    test_small_fast_setup(10);
    test_small_fast_setup(16);
    test_small_fast_setup(24);

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Results: %d/%d passed\n", tests_passed, tests_run);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
