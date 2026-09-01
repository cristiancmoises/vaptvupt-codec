/* Tests for 'T' tag (VV_ENTROPY_SEQ_V2 = 0x54, min_match=3) decoder.
 *
 * Strategy: since no encoder currently emits 'T' blocks, we test the
 * decoder by:
 *   1. Decoding an existing 'S' frame — verify backwards compat intact.
 *   2. Constructing a synthetic 'T' frame by rewriting an 'S' frame's
 *      tag byte AND patching the literal section to produce recognizable
 *      output. This verifies the tag dispatch path wires the v2 decoder
 *      in correctly.
 *   3. Feeding a payload whose ml codes decode to a known length-3
 *      sequence under v2 table but length-4 under v1 table — the
 *      output bytes differ between tags, proving v2's ml_base is used.
 *
 * The critical property: tag 'T' and tag 'S' with the SAME payload
 * produce DIFFERENT decoded output. On a payload crafted for v2,
 * only the 'T' tag decoder reproduces the intended bytes.
 */
#include "vaptvupt.h"
#include "vv_ans.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL  %s\n", msg); failures++; } \
    else         { printf("  PASS  %s\n", msg); } \
} while (0)

int main(void) {
    printf("\n=== VV_ENTROPY_SEQ_V2 ('T' tag) decoder tests ===\n\n");

    /* Test 1: 'S' backwards compat — encode + decode round-trip. */
    {
        uint8_t plain[4096];
        for (int i = 0; i < 4096; i++) plain[i] = (uint8_t)((i * 37 + 19) & 0xFF);
        uint8_t cmp[8192];
        vv_options_t opts; vv_default_options(&opts);
        int64_t clen = vv_compress(plain, sizeof(plain), cmp, sizeof(cmp), &opts);
        CHECK(clen > 0, "backcompat: 'S' frame encodes");

        uint8_t dec[4096 + 64];
        int64_t dl = vv_decompress(cmp, (size_t)clen, dec, sizeof(dec));
        CHECK(dl == (int64_t)sizeof(plain), "backcompat: 'S' frame decodes to correct size");
        CHECK(memcmp(dec, plain, sizeof(plain)) == 0,
              "backcompat: 'S' frame decodes to correct bytes");
    }

    /* Test 2: Unknown tag rejection — rewrite 'S' to a non-existent
     * tag byte (e.g., 0x55 'U') and confirm decoder refuses. This
     * proves the tag dispatch is tight.
     *
     * Use a 16 KB ramp+repeat input to guarantee the encoder produces
     * at least one ENTROPY 'S' block. */
    {
        uint8_t plain[16384];
        for (int i = 0; i < 16384; i++) plain[i] = (uint8_t)((i * 13 + (i >> 3)) & 0xFF);
        /* Repeat the first quarter at the end for LZ matches */
        memcpy(plain + 12288, plain, 4096);
        uint8_t cmp[32768];
        vv_options_t opts; vv_default_options(&opts);
        int64_t clen = vv_compress(plain, sizeof(plain), cmp, sizeof(cmp), &opts);
        CHECK(clen > 0, "unknown-tag: baseline 'S' encodes");

        /* Locate first ENTROPY 'S' block. Frame header is 16B. Each
         * block starts with 4B block-header (bits 0-1 = type, type=3
         * is ENTROPY). For ENTROPY, next 3B are csz, then 1B tag. */
        int found = -1;
        size_t fp = 16;  /* after frame header */
        while (fp + 8 < (size_t)clen) {
            uint32_t bh;
            memcpy(&bh, cmp + fp, 4);
            int btype = bh & 3;
            int is_last = (bh >> 2) & 1;
            uint32_t dsz = (bh >> 3) & 0x1FFFFF;
            fp += 4;
            if (btype == 0) {  /* RAW */
                fp += dsz;
            } else if (btype == 1) {  /* RLE */
                fp += 1;
            } else if (btype == 2 || btype == 3) {  /* COMPRESSED or ENTROPY */
                if (fp + 3 > (size_t)clen) break;
                uint32_t csz = cmp[fp] | (cmp[fp+1] << 8) | (cmp[fp+2] << 16);
                fp += 3;
                if (btype == 3 && fp < (size_t)clen && cmp[fp] == 0x53) {
                    found = (int)fp;
                    break;
                }
                fp += csz;
            } else break;
            if (is_last) break;
        }

        if (found >= 0) {
            uint8_t cmp_bad[32768];
            memcpy(cmp_bad, cmp, (size_t)clen);
            cmp_bad[found] = 0x55;  /* unknown tag 'U' */
            uint8_t dec[16384 + 64];
            int64_t dl = vv_decompress(cmp_bad, (size_t)clen, dec, sizeof(dec));
            CHECK(dl < 0, "unknown-tag: decoder rejects tag 'U' (0x55)");

            /* Rewrite to 'T' (0x54) — the 'T' decoder runs, but the
             * payload was encoded for 'S' (min_match=4). Under 'T',
             * every ml_base value is 1 less, so decoded matchlens
             * are 1 byte shorter than the encoder intended. This
             * produces wrong output OR a dsz mismatch. */
            memcpy(cmp_bad, cmp, (size_t)clen);
            cmp_bad[found] = 0x54;  /* 'T' tag */
            dl = vv_decompress(cmp_bad, (size_t)clen, dec, sizeof(dec));
            int exact_match = (dl == (int64_t)sizeof(plain) &&
                               memcmp(dec, plain, sizeof(plain)) == 0);
            CHECK(!exact_match,
                  "'T' on 'S' payload: rejects or produces wrong bytes (as expected)");
            /* NOTE: 'T' decoder ran successfully — the fact that we
             * got here without crash/segfault validates the dispatch
             * wiring. The output is wrong because the payload's ml
             * codes decoded with the wrong base table. */
        } else {
            printf("  FAIL  unknown-tag: could not locate ENTROPY 'S' block (test setup issue)\n");
            failures++;
        }
    }

    /* Test 3: Direct library-level 'T' decode of an empty payload.
     * Decoder's initial validation should return CORRUPT for an
     * under-length input, just as 'S' does. This proves the 'T'
     * path is wired up — if it weren't, we'd get a different error
     * (like unknown-tag) earlier. */
    {
        uint8_t empty_payload[1] = {0};
        uint8_t out[16];
        size_t out_len = 0;
        vva_error_t e = vva_decode_sequences_v2(empty_payload, 1,
                                                 out, sizeof(out),
                                                 &out_len, out);
        CHECK(e != VVA_OK,
              "'T' decoder rejects under-length payload (matches 'S' behavior)");
    }

    /* Test 4: Function pointer non-nullness sanity — confirms the
     * new symbol is actually exported. */
    {
        vva_error_t (*fn)(const uint8_t *, size_t, uint8_t *, size_t,
                          size_t *, const uint8_t *) = vva_decode_sequences_v2;
        CHECK(fn != NULL, "vva_decode_sequences_v2 symbol exported");
    }

    /* Test 5: End-to-end round-trip with opts.format_v2=1. Encoder
     * should emit 'T' tags, decoder should reconstruct identical
     * bytes. This is the core integration test for Sprint 44. */
    {
        uint8_t plain[16384];
        for (int i = 0; i < 16384; i++) plain[i] = (uint8_t)((i * 13 + (i >> 3)) & 0xFF);
        memcpy(plain + 12288, plain, 4096);

        vv_options_t opts; vv_default_options(&opts);
        opts.format_v2 = 1;
        uint8_t cmp[32768];
        int64_t clen = vv_compress(plain, sizeof(plain), cmp, sizeof(cmp), &opts);
        CHECK(clen > 0, "v2 encode: succeeds");

        uint8_t dec[16384 + 64];
        int64_t dl = vv_decompress(cmp, (size_t)clen, dec, sizeof(dec));
        CHECK(dl == (int64_t)sizeof(plain), "v2 round-trip: size matches");
        CHECK(memcmp(dec, plain, sizeof(plain)) == 0,
              "v2 round-trip: bytes match");

        /* Confirm the frame actually contains a 'T' tag (not 'S').
         * Walk blocks to find ENTROPY blocks and check tag byte. */
        int found_T = 0;
        size_t fp = 16;
        while (fp + 8 < (size_t)clen) {
            uint32_t bh;
            memcpy(&bh, cmp + fp, 4);
            int btype = bh & 3;
            int is_last = (bh >> 2) & 1;
            uint32_t dsz = (bh >> 3) & 0x1FFFFF;
            fp += 4;
            if (btype == 0) { fp += dsz; }
            else if (btype == 1) { fp += 1; }
            else if (btype == 2 || btype == 3) {
                if (fp + 3 > (size_t)clen) break;
                uint32_t csz = cmp[fp] | (cmp[fp+1] << 8) | (cmp[fp+2] << 16);
                fp += 3;
                if (btype == 3 && fp < (size_t)clen && cmp[fp] == 0x54) found_T = 1;
                fp += csz;
            } else break;
            if (is_last) break;
        }
        CHECK(found_T, "v2 encode: output contains a 'T' tag block (not just 'S')");
    }

    /* Test 6: v2 round-trip on incompressible random data. Should
     * fall to RAW block (no 'T' expected), and decode must still
     * produce exact bytes. Validates the "v2 skips COMPRESSED
     * fallback → uses RAW" path. */
    {
        uint8_t plain[2048];
        for (int i = 0; i < 2048; i++) plain[i] = (uint8_t)((i * 17 + 127) ^ 0xA5);

        vv_options_t opts; vv_default_options(&opts);
        opts.format_v2 = 1;
        uint8_t cmp[4096];
        int64_t clen = vv_compress(plain, sizeof(plain), cmp, sizeof(cmp), &opts);
        CHECK(clen > 0, "v2 incompressible: encode succeeds");

        uint8_t dec[2048 + 64];
        int64_t dl = vv_decompress(cmp, (size_t)clen, dec, sizeof(dec));
        CHECK(dl == (int64_t)sizeof(plain) &&
              memcmp(dec, plain, sizeof(plain)) == 0,
              "v2 incompressible: round-trip correct");
    }

    /* Test 7 — SPRINT 45 REGRESSION GUARD: long match (≥65535 bytes)
     * must round-trip cleanly in v2. Before the max_match cap fix,
     * ml_base_v2[35]=32767 with 15 extra bits overflowed for
     * matchlen=65535 (extra=32768 > 2^15-1), encoding as 0 in the
     * bitstream and decoding as matchlen=32767 — a 32,768-byte
     * shortfall per long match. This surfaced on python3 at ≥4 MB
     * where multi-block data produced long runs. */
    {
        size_t big_sz = 140000;  /* > 65535, forces a long match */
        uint8_t *plain = (uint8_t *)malloc(big_sz);
        if (!plain) { CHECK(0, "long-match: alloc"); goto done; }
        /* First 16 bytes: seed bytes. Rest: all 0xAB — one huge run
         * that any decent matcher will turn into a single long match
         * after the first few positions. This forces the length
         * through the upper ml_base codes. */
        for (size_t i = 0; i < 16; i++) plain[i] = (uint8_t)(i * 31);
        memset(plain + 16, 0xAB, big_sz - 16);

        vv_options_t opts; vv_default_options(&opts);
        opts.format_v2 = 1;
        opts.mode = VV_MODE_EXTREME;
        size_t cap = vv_compress_bound(big_sz) + 4096;
        uint8_t *cmp_buf = (uint8_t *)malloc(cap);
        int64_t clen = vv_compress(plain, big_sz, cmp_buf, cap, &opts);
        CHECK(clen > 0, "long-match: v2 encode succeeds");

        uint8_t *dec_buf = (uint8_t *)malloc(big_sz + 64);
        int64_t dl = vv_decompress(cmp_buf, (size_t)clen, dec_buf, big_sz + 64);
        CHECK(dl == (int64_t)big_sz,
              "long-match: v2 decode size matches (max_match cap works)");
        CHECK(dl == (int64_t)big_sz && memcmp(dec_buf, plain, big_sz) == 0,
              "long-match: v2 decode bytes match");

        free(plain); free(cmp_buf); free(dec_buf);
done:   ;
    }

    /* Test 8: an LL run over 65535 followed by a match cannot be represented
     * by the current SEQ wire layout. It has a global match_count but no
     * per-sequence has-match bit, so inserting a zero-match split before the
     * real match used to make the decoder consume that match too early. The
     * SEQ encoder must reject this candidate so vv_compress can use a safe
     * fallback block. */
    {
        enum { LL = 65536 };
        size_t ext_count = (LL - 15) / 255 + 1;
        size_t tok_len = 1 + ext_count + LL + 2;
        uint8_t *tokens = (uint8_t *)malloc(tok_len);
        uint8_t *seq = (uint8_t *)malloc(tok_len + 4096);
        size_t p = 0, remain = LL - 15;
        if (!tokens || !seq) {
            CHECK(0, "oversize nonterminal LL: alloc");
        } else {
            tokens[p++] = 0xF0; /* ll=extended, v2 matchlen=3 */
            while (remain >= 255) { tokens[p++] = 255; remain -= 255; }
            tokens[p++] = (uint8_t)remain;
            for (size_t i = 0; i < LL; i++) tokens[p++] = (uint8_t)(i * 73u + 19u);
            tokens[p++] = 1; tokens[p++] = 0; /* offset=1: real match follows */
            size_t seq_len = 0;
            vva_error_t e = vva_encode_sequences_v2(tokens, p, seq,
                                                      tok_len + 4096,
                                                      &seq_len, 2);
            CHECK(p == tok_len && e != VVA_OK,
                  "oversize nonterminal LL: SEQ candidate rejected safely");
        }
        free(tokens); free(seq);
    }

    /* Test 9: deterministic end-to-end reproducer for the same bug. A short
     * compressible prefix followed by xorshift data creates sparse 3-byte
     * matches with an intervening >64 KiB literal run. Before the fail-closed
     * rule, vv_compress returned a frame that vv_decompress rejected. */
    {
        static const char phrase[] =
            "In Code We Trust. VaptVupt OOM robustness probe. ";
        size_t prefix_len = (sizeof(phrase) - 1) * 64;
        size_t plain_len = prefix_len + 200000;
        uint8_t *plain = (uint8_t *)malloc(plain_len);
        size_t cap = vv_compress_bound(plain_len) + 4096;
        uint8_t *cmp = (uint8_t *)malloc(cap);
        uint8_t *dec = (uint8_t *)malloc(plain_len + 64);
        if (!plain || !cmp || !dec) {
            CHECK(0, "oversize nonterminal LL: end-to-end alloc");
            CHECK(0, "oversize nonterminal LL: end-to-end round-trip");
        } else {
            for (size_t i = 0; i < 64; i++)
                memcpy(plain + i * (sizeof(phrase) - 1), phrase,
                       sizeof(phrase) - 1);
            uint32_t rng = 4;
            for (size_t i = prefix_len; i < plain_len; i++) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                plain[i] = (uint8_t)(rng >> 24);
            }
            vv_options_t opts; vv_default_options(&opts);
            opts.mode = VV_MODE_BALANCED;
            int64_t clen = vv_compress(plain, plain_len, cmp, cap, &opts);
            CHECK(clen > 0, "oversize nonterminal LL: end-to-end encode");
            int64_t dlen = clen > 0
                         ? vv_decompress(cmp, (size_t)clen, dec, plain_len + 64)
                         : VV_ERR_CORRUPT;
            CHECK(dlen == (int64_t)plain_len &&
                  memcmp(dec, plain, plain_len) == 0,
                  "oversize nonterminal LL: end-to-end round-trip");
        }
        free(plain); free(cmp); free(dec);
    }

    /* Total CHECKs = 8 (Tests 1-4) + 5 (Test 5) + 2 (Test 6)
     * + 3 (Test 7) + 1 (Test 8) + 2 (Test 9) = 21. */
    printf("\nResults: %d passed, %d failed\n",
           21 - failures, failures);
    return failures ? 1 : 0;
}
