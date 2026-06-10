/* test_exact_buffer_decode.c — regression for the AVX2 decode wide-store
 * over-write on an exactly-content-sized output buffer.
 *
 * vv_decompress's contract allows dst_cap == content_size (no slack).
 * The fast-path match copy match_copy_32_hot does an unconditional 32-byte
 * store (lz4 decode trick) that over-writes up to 32 - mlen bytes past the
 * match. Its safety relies on a 72-byte margin checked at loop *entry*
 * (op < op_safe = op_end - 72), but op advances by the literal length
 * within the iteration, so a large literal run can push op within 32 bytes
 * of op_end before the match copy runs. When the final match of the stream
 * is a fast-path (offset >= 32) match landing near op_end, the wide store
 * writes past the exactly-content-sized buffer (heap-buffer-overflow WRITE,
 * caught under ASan; observed in decode_block_tokens_w16).
 *
 * Fixed by gating the wide-store fast path on (op_end - op) >= 32 and
 * falling back to the exact-tail match_copy_32 otherwise.
 *
 * This test compresses a range of inputs and decompresses each into a
 * buffer sized to EXACTLY content_size (the trigger), asserting a correct
 * roundtrip. Run under ASan for full assurance:
 *   gcc -O2 -g -fsanitize=address,undefined -mavx2 -Iinclude \
 *       tests/test_exact_buffer_decode.c <core .c files> -o t && ./t
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int passed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); failures++; } \
} while (0)

/* The 122-byte fixture that originally reproduced the over-write (its final
 * fast-path match lands within 32 bytes of op_end on an exact buffer). */
static const char *TRIGGER_FIXTURE =
    "The quick brown fox jumps over the lazy dog. "
    "Pack my box with five dozen liquor jugs. "
    "How vexingly quick daft zebras jump!";

/* Compress `data`/`len` at `mode`, then decompress into a buffer sized to
 * EXACTLY the decompressed length and assert a byte-correct roundtrip. */
static void roundtrip_exact(const uint8_t *data, size_t len, vv_mode_t mode,
                            const char *label) {
    size_t cap = vv_compress_bound(len);
    uint8_t *cmp = (uint8_t *)malloc(cap);
    if (!cmp) { CHECK(0, "alloc compress buffer"); return; }

    vv_options_t o;
    vv_default_options(&o);
    o.mode = mode;
    o.checksum = 1;
    o.window_log = 0;
    int64_t clen = vv_compress(data, len, cmp, cap, &o);
    CHECK(clen > 0 || len == 0, label);
    if (clen <= 0) { free(cmp); return; }

    /* EXACT-sized decode buffer — no slack. This is the trigger. The
     * malloc is exactly `len` so ASan red-zones sit immediately after the
     * last valid output byte; any wide-store overshoot is an OOB write. */
    uint8_t *dec = (uint8_t *)malloc(len ? len : 1);
    if (!dec) { free(cmp); CHECK(0, "alloc exact decode buffer"); return; }
    int64_t dlen = vv_decompress(cmp, (size_t)clen, dec, len);
    CHECK(dlen == (int64_t)len, label);
    CHECK(len == 0 || memcmp(dec, data, len) == 0, label);

    free(dec);
    free(cmp);
}

int main(void) {
    const vv_mode_t modes[3] = { VV_MODE_ULTRA_FAST, VV_MODE_BALANCED, VV_MODE_EXTREME };
    const char *mode_name[3] = { "fast", "balanced", "extreme" };

    /* 1. The exact fixture that reproduced the bug, all three modes. */
    for (int m = 0; m < 3; m++) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "trigger fixture exact decode (%s)", mode_name[m]);
        roundtrip_exact((const uint8_t *)TRIGGER_FIXTURE, strlen(TRIGGER_FIXTURE),
                        modes[m], lbl);
    }

    /* 2. Sweep input sizes so the final match lands at many offsets relative
     *    to op_end (32-byte store alignment). Moderately compressible text so
     *    balanced/extreme emit fast-path (offset >= 32) matches. */
    {
        size_t maxn = 8192;
        uint8_t *buf = (uint8_t *)malloc(maxn);
        if (buf) {
            static const char *w[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
                                      "over ", "lazy ", "dog ", "and ", "then ",
                                      "runs ", "away ", "fast "};
            for (size_t n = 48; n <= maxn; n += (n < 512 ? 1 : 97)) {
                size_t p = 0; unsigned r = 0xC0FFEEu ^ (unsigned)n;
                while (p < n) {
                    r = r * 1103515245u + 12345u;
                    const char *t = w[(r >> 16) % 13];
                    size_t l = strlen(t);
                    if (p + l > n) break;
                    memcpy(buf + p, t, l); p += l;
                }
                while (p < n) { buf[p] = (uint8_t)('a' + (p & 15)); p++; }
                /* balanced + extreme exercise the SEQ + token fast paths;
                 * fast exercises the raw-token (decode_block_tokens) path. */
                roundtrip_exact(buf, n, VV_MODE_BALANCED, "size-sweep balanced exact decode");
                if ((n & 7) == 0)
                    roundtrip_exact(buf, n, VV_MODE_ULTRA_FAST, "size-sweep fast exact decode");
            }
            free(buf);
        } else {
            CHECK(0, "alloc sweep buffer");
        }
    }

    /* 3. Highly repetitive input (long fast-path matches ending near op_end). */
    {
        size_t n = 4096;
        uint8_t *buf = (uint8_t *)malloc(n);
        if (buf) {
            for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)("ABCD0123" [i & 7]);
            for (int m = 0; m < 3; m++)
                roundtrip_exact(buf, n, modes[m], "repetitive exact decode");
            free(buf);
        }
    }

    /* 3b. Long fast-path matches (n > 32) whose copy ends exactly at op_end.
     *     match_copy_32_hot's n>32 branch must finish with an EXACT tail, not
     *     a final 32-byte over-store; otherwise the last store writes up to
     *     32 - (n & 31) bytes past an exactly-content-sized buffer. A long
     *     run of a repeating multi-byte pattern (offset >= 32) yields exactly
     *     such matches. Sweep total length so the final match length mod 32
     *     covers all residues, and the match lands flush against op_end. */
    {
        size_t maxn = 6000;
        uint8_t *buf = (uint8_t *)malloc(maxn);
        if (buf) {
            /* 40-byte repeating pattern => matches with offset 40 (>= 32) and
             * large match lengths, exercising the n>32 chunk loop + tail. */
            for (size_t i = 0; i < maxn; i++)
                buf[i] = (uint8_t)('A' + (i % 40));
            for (size_t n = 200; n <= maxn; n++) {
                roundtrip_exact(buf, n, VV_MODE_BALANCED, "long-match n>32 exact decode (balanced)");
                if ((n & 31) == 0 || (n % 40) == 0)
                    roundtrip_exact(buf, n, VV_MODE_EXTREME, "long-match n>32 exact decode (extreme)");
            }
            free(buf);
        } else {
            CHECK(0, "alloc long-match buffer");
        }
    }

    /* 4. Zero-length and tiny inputs (boundary of the exact-buffer logic). */
    roundtrip_exact((const uint8_t *)"", 0, VV_MODE_BALANCED, "empty exact decode");
    roundtrip_exact((const uint8_t *)"x", 1, VV_MODE_BALANCED, "1-byte exact decode");

    printf("test_exact_buffer_decode: %d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
