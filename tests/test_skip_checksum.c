/* Tests for VV_DECOMPRESS_SKIP_CHECKSUM flag.
 *
 * Validates:
 *   1. Skip flag produces correct output on valid input
 *   2. Default (no skip) catches XXH64 corruption
 *   3. Skip flag accepts XXH64 corruption (by design — caller said so)
 *   4. Skip flag still catches structural corruption (truncated footer magic)
 *   5. Backward-compat: vv_decompress() behaves exactly like
 *      vv_decompress_flags(..., VV_DECOMPRESS_DEFAULT)
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL  %s\n", msg); failures++; } \
} while (0)

static size_t compress_fixture(const uint8_t *src, size_t src_len,
                               uint8_t *dst, size_t dst_cap) {
    vv_options_t opts; vv_default_options(&opts);
    int64_t n = vv_compress(src, src_len, dst, dst_cap, &opts);
    if (n < 0) { fprintf(stderr, "compress fail: %lld\n", (long long)n); exit(1); }
    return (size_t)n;
}

int main(void) {
    printf("\n=== VV_DECOMPRESS_SKIP_CHECKSUM tests ===\n\n");

    /* Fixture: 4 KB of known data */
    uint8_t plain[4096];
    for (int i = 0; i < 4096; i++) plain[i] = (uint8_t)((i * 37 + 19) & 0xFF);
    uint8_t cmp[8192];
    size_t clen = compress_fixture(plain, sizeof(plain), cmp, sizeof(cmp));

    uint8_t dec[4096 + 64];

    /* Test 1: default flag path works */
    memset(dec, 0, sizeof(dec));
    int64_t r = vv_decompress_flags(cmp, clen, dec, sizeof(dec),
                                     VV_DECOMPRESS_DEFAULT);
    CHECK(r == (int64_t)sizeof(plain), "default: correct decode size");
    CHECK(memcmp(dec, plain, sizeof(plain)) == 0, "default: correct bytes");

    /* Test 2: skip flag works */
    memset(dec, 0, sizeof(dec));
    r = vv_decompress_flags(cmp, clen, dec, sizeof(dec),
                             VV_DECOMPRESS_SKIP_CHECKSUM);
    CHECK(r == (int64_t)sizeof(plain), "skip: correct decode size");
    CHECK(memcmp(dec, plain, sizeof(plain)) == 0, "skip: correct bytes");

    /* Test 3: default flag catches corrupted XXH64 */
    uint8_t cmp_bad[8192];
    memcpy(cmp_bad, cmp, clen);
    /* XXH64 footer is 12 bytes before end: 8B checksum + 4B magic */
    cmp_bad[clen - 12] ^= 0x55;  /* flip bits in the checksum */
    memset(dec, 0, sizeof(dec));
    r = vv_decompress_flags(cmp_bad, clen, dec, sizeof(dec),
                             VV_DECOMPRESS_DEFAULT);
    CHECK(r < 0, "default: rejects flipped-checksum input");

    /* Test 4: skip flag accepts flipped-checksum (by design) */
    memset(dec, 0, sizeof(dec));
    r = vv_decompress_flags(cmp_bad, clen, dec, sizeof(dec),
                             VV_DECOMPRESS_SKIP_CHECKSUM);
    CHECK(r == (int64_t)sizeof(plain),
          "skip: accepts flipped-checksum (as documented)");
    CHECK(memcmp(dec, plain, sizeof(plain)) == 0,
          "skip: still decodes correct bytes when only checksum corrupted");

    /* Test 5: skip flag STILL catches footer-magic corruption */
    uint8_t cmp_bad2[8192];
    memcpy(cmp_bad2, cmp, clen);
    cmp_bad2[clen - 4] ^= 0x55;  /* corrupt the footer magic */
    r = vv_decompress_flags(cmp_bad2, clen, dec, sizeof(dec),
                             VV_DECOMPRESS_SKIP_CHECKSUM);
    CHECK(r < 0, "skip: still rejects footer-magic corruption");

    /* Test 6: backward compat — vv_decompress equals default path */
    memset(dec, 0, sizeof(dec));
    int64_t r1 = vv_decompress(cmp, clen, dec, sizeof(dec));
    uint8_t dec2[4096 + 64]; memset(dec2, 0, sizeof(dec2));
    int64_t r2 = vv_decompress_flags(cmp, clen, dec2, sizeof(dec2),
                                      VV_DECOMPRESS_DEFAULT);
    CHECK(r1 == r2, "backward-compat: return value matches");
    CHECK(memcmp(dec, dec2, sizeof(plain)) == 0,
          "backward-compat: output bytes match");

    /* Test 7: encode-side checksum=0 produces a shorter frame
     * (no 12-byte footer) and still decodes correctly with
     * both default and skip-flag decoders. */
    vv_options_t opts; vv_default_options(&opts);
    opts.checksum = 0;
    uint8_t cmp_nocks[8192];
    int64_t nocks_len = vv_compress(plain, sizeof(plain),
                                      cmp_nocks, sizeof(cmp_nocks), &opts);
    CHECK(nocks_len > 0, "nocks: compress succeeds");
    CHECK(nocks_len == (int64_t)clen - 12,
          "nocks: frame is exactly 12 bytes shorter (no XXH64 footer)");

    memset(dec, 0, sizeof(dec));
    r = vv_decompress_flags(cmp_nocks, (size_t)nocks_len, dec, sizeof(dec),
                             VV_DECOMPRESS_DEFAULT);
    CHECK(r == (int64_t)sizeof(plain),
          "nocks: default decode produces correct size");
    CHECK(memcmp(dec, plain, sizeof(plain)) == 0,
          "nocks: default decode produces correct bytes");

    memset(dec, 0, sizeof(dec));
    r = vv_decompress_flags(cmp_nocks, (size_t)nocks_len, dec, sizeof(dec),
                             VV_DECOMPRESS_SKIP_CHECKSUM);
    CHECK(r == (int64_t)sizeof(plain),
          "nocks + skip: correct size");
    CHECK(memcmp(dec, plain, sizeof(plain)) == 0,
          "nocks + skip: correct bytes");

    printf("\nResults: %d passed, %d failed\n",
           18 - failures, failures);
    return failures ? 1 : 0;
}
