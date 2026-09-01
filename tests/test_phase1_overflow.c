/* test_phase1_overflow.c — regression for the AVX2 decode phase-1
 * (warmup) match-length output-bound check.
 *
 * Before the fix, decode_block_tokens_impl's phase-1 warmup loop
 * validated the offset but did NOT check op + mlen <= op_end before
 * the match copy (phase 2 and the general/tail path already did). A
 * corrupt match-length extension could therefore drive any of the
 * four match-copy variants (match_copy_32_hot, _16, _8, match_overlap)
 * to write past op_end — a heap-buffer-overflow WRITE found by audit
 * (AddressSanitizer confirmed, e.g. in match_overlap for offset < 8).
 *
 * This test crafts token streams that reach phase 1 (ip_len > 48, a
 * moderate output buffer so the safe zone is active) and then present
 * a match whose extended length overruns the output buffer, for each
 * offset class. EXPECTED: a clean error return (VV_ERR_OVERFLOW /
 * VV_ERR_CORRUPT), no crash, no OOB. Run under ASan for full assurance:
 *   gcc -O2 -g -fsanitize=address,undefined -mavx2 -Iinclude \
 *       tests/test_phase1_overflow.c <core objs minus vv_decoder> -o t
 *
 * It includes vv_decoder.c directly to reach the static
 * decode_block_tokens_impl entry point. */

#include "../src/vv_decoder.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int passed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passed++; } \
    else { printf("FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); failures++; } \
} while (0)

/* Build a token stream that warms up phase 1 (advancing op past the
 * requested offset and consuming enough input that ip_len > 48), then
 * emits a single match with the given offset and a corrupt extended
 * match length (mc==15 + a 0xFF extension byte => mlen += 255), then
 * pads so the malicious token sits > 48 bytes before ip_end (keeping
 * the warmup loop active at that token). Returns the stream length. */
static size_t build_overflow_stream(uint8_t *ip, size_t cap, uint32_t offset) {
    size_t n = 0;
    /* Warmup: 8 tokens of 14 literals + a 4-byte match (offset 1).
     * 8 * (14 + 4) = 144 bytes of output, > any offset we test below. */
    for (int k = 0; k < 8 && n + 17 < cap; k++) {
        ip[n++] = (uint8_t)((14u << 4) | 0u);          /* ll=14, mc=0 -> mlen=4 */
        for (int j = 0; j < 14; j++) ip[n++] = (uint8_t)('A' + (j & 31));
        ip[n++] = 1; ip[n++] = 0;                       /* offset = 1 (2-byte) */
    }
    /* Malicious match: ll=0, mc=15 (extended length). */
    ip[n++] = (uint8_t)((0u << 4) | 15u);
    ip[n++] = (uint8_t)(offset & 0xFF);
    ip[n++] = (uint8_t)((offset >> 8) & 0xFF);          /* 2-byte offset */
    ip[n++] = 0xFF; ip[n++] = 0x00;                     /* mlen += 255 -> mlen huge */
    /* Trailing padding so the malicious token is > 48 bytes before ip_end. */
    while (n < 200 && n < cap) ip[n++] = 0;
    return n;
}

static void test_phase1_offset_class(uint32_t offset, const char *label) {
    uint8_t ip[256];
    size_t ip_len = build_overflow_stream(ip, sizeof(ip), offset);

    /* Small heap buffer: op + (huge mlen) must exceed op_end while the
     * warmup loop is still active (op < op_end - 72, op < 0xFFFF). */
    size_t dst_cap = 300;
    uint8_t *op = (uint8_t *)malloc(dst_cap);
    CHECK(op != NULL, "alloc output");
    if (!op) return;
    size_t out_len = 12345;
    /* dst_base == op: single-block decode, offsets validated against
     * the buffer start. off_bytes = 2 (window log <= 16). */
    vv_error_t r = decode_block_tokens_impl(ip, ip_len, op, dst_cap, &out_len, op, 2);
    CHECK(r != VV_OK, label);          /* must reject corrupt overrun, not crash */
    free(op);
}

/* A long literal extension consumes the AVX2 loop's normal input lookahead.
 * Leave fewer offset bytes than the selected window encoding requires and
 * require both public decode surfaces to reject it without reading past src. */
static void test_truncated_offset_after_literals(void) {
    for (uint8_t wlog = 16; wlog <= 17; wlog++) {
        size_t off_bytes = (wlog > 16) ? 3u : 2u;
        size_t csz = 1u + 1u + 80u + off_bytes - 1u;
        size_t frame_len = sizeof(vv_frame_header_t) + 4u + 3u + csz;
        uint8_t *frame = (uint8_t *)calloc(1, frame_len);
        uint8_t *dst = (uint8_t *)calloc(1, 80);
        CHECK(frame != NULL && dst != NULL, "allocate truncated-offset frame");
        if (!frame || !dst) { free(frame); free(dst); continue; }

        vv_frame_header_t fh;
        memset(&fh, 0, sizeof(fh));
        fh.magic = VV_MAGIC;
        fh.version = 1;
        fh.mode_hint = VV_MODE_BALANCED;
        fh.window_log = wlog;
        fh.content_size = 80;
        memcpy(frame, &fh, sizeof(fh));

        uint32_t bh = vv_bh_pack(VV_BLOCK_COMPRESSED, 1, 80);
        memcpy(frame + sizeof(fh), &bh, 4);
        uint8_t *p = frame + sizeof(fh) + 4;
        p[0] = (uint8_t)csz;
        p[1] = (uint8_t)(csz >> 8);
        p[2] = (uint8_t)(csz >> 16);
        p += 3;
        *p++ = 0xF0;             /* ll extension follows */
        *p++ = 65;               /* 15 + 65 = 80 literals */
        memset(p, 'L', 80);
        p += 80;
        memset(p, 0, off_bytes - 1u); /* deliberately truncated offset */

        int64_t one_shot = vv_decompress(frame, frame_len, dst, 80);
        CHECK(one_shot < 0, wlog == 16 ?
              "one-shot rejects truncated 2-byte offset" :
              "one-shot rejects truncated 3-byte offset");

        vv_dstream_t *ds = vv_dstream_create();
        size_t consumed = 0, written = 0;
        int streaming = ds ? vv_dstream_decompress_chunk(ds, frame, frame_len,
                                                          dst, 80, &consumed,
                                                          &written) : VV_ERR_NOMEM;
        CHECK(streaming < 0, wlog == 16 ?
              "streaming rejects truncated 2-byte offset" :
              "streaming rejects truncated 3-byte offset");
        vv_dstream_destroy(ds);
        free(frame);
        free(dst);
    }
}

int main(void) {
    /* One case per match-copy branch in the warmup loop. */
    test_phase1_offset_class(64, "offset>=32 (match_copy_32_hot) rejects overrun");
    test_phase1_offset_class(20, "offset 16-31 (match_copy_16) rejects overrun");
    test_phase1_offset_class(10, "offset 8-15 (match_copy_8) rejects overrun");
    test_phase1_offset_class(1,  "offset<8 (match_overlap) rejects overrun");

    /* 3-byte-offset path (window log > 16) shares the same impl. */
    {
        uint8_t ip[256];
        size_t n = 0;
        for (int k = 0; k < 8; k++) {
            ip[n++] = (uint8_t)((14u << 4) | 0u);
            for (int j = 0; j < 14; j++) ip[n++] = (uint8_t)('A' + (j & 31));
            ip[n++] = 1; ip[n++] = 0; ip[n++] = 0;       /* offset = 1 (3-byte) */
        }
        ip[n++] = (uint8_t)((0u << 4) | 15u);
        ip[n++] = 1; ip[n++] = 0; ip[n++] = 0;           /* offset = 1 */
        ip[n++] = 0xFF; ip[n++] = 0x00;                  /* mlen huge */
        while (n < 220) ip[n++] = 0;
        size_t dst_cap = 300;
        uint8_t *op = (uint8_t *)malloc(dst_cap);
        size_t out_len = 0;
        vv_error_t r = decode_block_tokens_impl(ip, n, op, dst_cap, &out_len, op, 3);
        CHECK(r != VV_OK, "3-byte offset path rejects overrun");
        free(op);
    }

    test_truncated_offset_after_literals();

    printf("test_phase1_overflow: %d passed, %d failed\n", passed, failures);
    return failures ? 1 : 0;
}
