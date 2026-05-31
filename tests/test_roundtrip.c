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

    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Results: %d/%d passed\n", tests_passed, tests_run);
    fprintf(stderr, "═══════════════════════════════════════════\n\n");

    return (tests_passed == tests_run) ? 0 : 1;
}
