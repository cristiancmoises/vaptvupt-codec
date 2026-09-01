/*
 * VaptVupt — Format specification self-test.
 *
 * Validates the on-wire format documented in FORMAT.md by parsing a
 * frame header using ONLY the byte layout described in the spec
 * (no shared #defines from vaptvupt.h beyond magic constants).
 *
 * Goal: if FORMAT.md and the encoder ever diverge, this test will fail.
 * If you change the wire format, you MUST update FORMAT.md and this
 * test (otherwise this test catches the drift).
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    fprintf(stderr, "  %-58s ", name); \
    fflush(stderr); \
} while(0)
#define PASS() do { tests_passed++; fprintf(stderr, "PASS\n"); } while(0)
#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); } while(0)

/* Read a little-endian uint32 from a byte buffer per FORMAT.md §3 */
static uint32_t read_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
    return  (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

int main(void) {
    fprintf(stderr, "\n╔════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  VaptVupt — Wire format spec self-test (FORMAT.md ↔ code)     ║\n");
    fprintf(stderr, "╚════════════════════════════════════════════════════════════════╝\n\n");

    /* Compress a known input */
    const char *src = "hello world";
    size_t src_len = strlen(src);

    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_BALANCED;

    size_t cap = vv_compress_bound(src_len);
    uint8_t *frame = (uint8_t *)malloc(cap);
    int64_t fsz = vv_compress((const uint8_t *)src, src_len, frame, cap, &opts);
    if (fsz < 0) {
        fprintf(stderr, "Setup failure: vv_compress returned %lld\n", (long long)fsz);
        free(frame);
        return 1;
    }

    /* ─── Validate frame header per FORMAT.md §2 ─── */

    TEST("FORMAT.md §2: frame is at least 16 bytes (header)");
    if (fsz >= 16) PASS(); else FAIL("frame too small");

    TEST("FORMAT.md §2: magic field is 0x56560100 (LE uint32)");
    uint32_t magic = read_u32_le(frame + 0);
    if (magic == 0x56560100u) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "got 0x%08X", magic); FAIL(m); }

    TEST("FORMAT.md §2: byte 0..3 in stream order is 00 01 56 56");
    if (frame[0] == 0x00 && frame[1] == 0x01 && frame[2] == 0x56 && frame[3] == 0x56) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "got %02X %02X %02X %02X",
        frame[0], frame[1], frame[2], frame[3]); FAIL(m); }

    TEST("FORMAT.md §2: version field at offset 4 is 0x01");
    if (frame[4] == 0x01) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "got 0x%02X", frame[4]); FAIL(m); }

    TEST("FORMAT.md §2: flags byte at offset 5 has has_checksum (bit 0) set");
    /* vv_default_options sets checksum=1 */
    if ((frame[5] & 0x01) == 0x01) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "got 0x%02X", frame[5]); FAIL(m); }

    TEST("FORMAT.md §2: encoder leaves reserved flag bits 1 and 4-7 clear");
    if ((frame[5] & 0xF2) == 0) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "got 0x%02X", frame[5]); FAIL(m); }

    TEST("FORMAT.md §2: mode_hint at offset 6 is 1 (BALANCED)");
    if (frame[6] == 0x01) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "got 0x%02X", frame[6]); FAIL(m); }

    TEST("FORMAT.md §2: window_log at offset 7 is in [10..24]");
    if (frame[7] >= 10 && frame[7] <= 24) PASS();
    else { char m[40]; snprintf(m, sizeof(m), "got 0x%02X", frame[7]); FAIL(m); }

    TEST("FORMAT.md §2: content_size at offset 8 (LE u64) equals input length");
    uint64_t cs = read_u64_le(frame + 8);
    if (cs == (uint64_t)src_len) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "expected %zu got %llu",
        src_len, (unsigned long long)cs); FAIL(m); }

    /* ─── Validate first block header per FORMAT.md §3 ─── */

    TEST("FORMAT.md §3: block header at offset 16 has last_block bit set (small input)");
    uint32_t bh = read_u32_le(frame + 16);
    int last_bit = (bh >> 2) & 1;
    if (last_bit == 1) PASS();
    else FAIL("last_block bit not set");

    TEST("FORMAT.md §3: block header dsz field equals input length");
    uint32_t dsz = (bh >> 3) & 0x1FFFFF;
    if (dsz == (uint32_t)src_len) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "expected %zu got %u",
        src_len, dsz); FAIL(m); }

    TEST("FORMAT.md §3: encoder produces zero in block header reserved bits 24-31");
    if ((bh & 0xFF000000u) == 0) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "got 0x%08X", bh); FAIL(m); }

    /* ─── Validate footer per FORMAT.md §4 ─── */

    TEST("FORMAT.md §4: footer present (since has_checksum bit set)");
    /* footer is 12 bytes at the end of the frame */
    if (fsz >= 12) PASS(); else FAIL("frame too small for footer");

    TEST("FORMAT.md §4: footer_magic at last 4 bytes is 0x56564E44 ('VVND' LE)");
    uint32_t fm = read_u32_le(frame + fsz - 4);
    if (fm == 0x56564E44u) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "got 0x%08X", fm); FAIL(m); }

    TEST("FORMAT.md §4: footer_magic bytes in stream order are 44 4E 56 56");
    if (frame[fsz - 4] == 0x44 && frame[fsz - 3] == 0x4E
        && frame[fsz - 2] == 0x56 && frame[fsz - 1] == 0x56) PASS();
    else FAIL("byte order");

    /* ─── Roundtrip via standard decoder ─── */
    TEST("Roundtrip: decoder accepts the frame");
    uint8_t out[64];
    int64_t dsz_out = vv_decompress(frame, (size_t)fsz, out, sizeof(out));
    if (dsz_out == (int64_t)src_len && memcmp(out, src, src_len) == 0) PASS();
    else { char m[64]; snprintf(m, sizeof(m), "decompress=%lld",
        (long long)dsz_out); FAIL(m); }

    /* ─── Multi-frame: concatenate two frames, verify §6 spec ─── */
    {
        TEST("FORMAT.md §6: two concatenated frames decode as multi-frame");
        uint8_t *frame2 = (uint8_t *)malloc(cap);
        const char *src2 = "second";
        size_t src2_len = strlen(src2);
        int64_t fsz2 = vv_compress((const uint8_t *)src2, src2_len,
                                    frame2, cap, &opts);
        uint8_t *combo = (uint8_t *)malloc(fsz + fsz2);
        memcpy(combo, frame, fsz);
        memcpy(combo + fsz, frame2, fsz2);
        uint8_t out_combo[128];
        int64_t dsz_combo = vv_decompress(combo, (size_t)(fsz + fsz2),
                                           out_combo, sizeof(out_combo));
        int ok = (dsz_combo == (int64_t)(src_len + src2_len))
              && (memcmp(out_combo, src, src_len) == 0)
              && (memcmp(out_combo + src_len, src2, src2_len) == 0);
        free(frame2); free(combo);
        if (ok) PASS();
        else { char m[64]; snprintf(m, sizeof(m), "got %lld bytes",
            (long long)dsz_combo); FAIL(m); }
    }

    /* ─── Magic-rejection: corrupt magic must fail ─── */
    TEST("FORMAT.md §2: corrupted magic causes decoder error");
    frame[0] ^= 0xFF;
    uint8_t out_bad[64];
    int64_t dsz_bad = vv_decompress(frame, (size_t)fsz, out_bad, sizeof(out_bad));
    if (dsz_bad < 0) PASS(); else FAIL("decoder accepted bad magic");
    frame[0] ^= 0xFF; /* restore */

    /* ─── Version-rejection: unknown version must fail ─── */
    TEST("FORMAT.md §2: unknown version causes decoder error");
    frame[4] = 0xFF;
    int64_t dsz_v = vv_decompress(frame, (size_t)fsz, out_bad, sizeof(out_bad));
    if (dsz_v < 0) PASS(); else FAIL("decoder accepted version=0xFF");

    free(frame);

    fprintf(stderr, "\n  Results: %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
