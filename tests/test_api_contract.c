/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_api_contract.c — public API contract tests.
 *
 * Sprint 95 audit: verifies NULL-parameter handling, empty-input
 * acceptance, and other contract guarantees on all public entry
 * points. Found 2 inconsistencies (NULL opts rejection, empty src
 * rejection) which were fixed in v2.46.4.
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg) do { \
    if (cond) printf("  ✓ %s\n", msg); \
    else { printf("  ✗ FAIL: %s\n", msg); fails++; } \
} while(0)

int main(void) {
    int fails = 0;
    uint8_t buf[1024];
    uint8_t small[8] = {0};
    int64_t r;
    int ir;
    vv_options_t opts;
    
    /* === vv_compress NULL/bad-arg checks === */
    printf("vv_compress edge cases:\n");
    
    /* NULL src */
    r = vv_compress(NULL, 100, buf, sizeof(buf), NULL);
    CHECK(r < 0, "NULL src returns error");
    
    /* NULL dst */
    r = vv_compress(small, 8, NULL, 1024, NULL);
    CHECK(r < 0, "NULL dst returns error");
    
    /* dst_cap = 0 */
    r = vv_compress(small, 8, buf, 0, NULL);
    CHECK(r < 0, "dst_cap=0 returns error");
    
    /* src_len = 0 (valid: empty input) */
    r = vv_compress(small, 0, buf, sizeof(buf), NULL);
    CHECK(r >= 0, "src_len=0 succeeds (empty input is valid)");
    
    /* NULL opts (should use defaults) */
    r = vv_compress(small, 8, buf, sizeof(buf), NULL);
    CHECK(r > 0, "NULL opts uses defaults");

    /* Only the three declared mode values are valid, regardless of whether
     * the window is automatic or explicit. */
    for (int w = 0; w < 2; w++) {
        vv_default_options(&opts);
        opts.window_log = w ? 16 : 0;
        opts.mode = (vv_mode_t)3;
        r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
        CHECK(r == VV_ERR_PARAM, "mode=3 is rejected as a bad parameter");
        opts.mode = (vv_mode_t)-1;
        r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
        CHECK(r == VV_ERR_PARAM, "mode=-1 is rejected as a bad parameter");
    }

    /* Architecture filters are mutually exclusive. Validate before the
     * nonempty-input BCJ branch so an empty frame cannot bypass the rule. */
    vv_default_options(&opts);
    opts.filter_x86 = 1;
    opts.filter_arm64 = 1;
    r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
    CHECK(r == VV_ERR_PARAM, "simultaneous x86/ARM64 filters are rejected");
    r = vv_compress(small, 0, buf, sizeof(buf), &opts);
    CHECK(r == VV_ERR_PARAM, "filter exclusivity also applies to empty input");

    /* Window validation also happens in the wrapper, before an explicitly
     * requested BCJ filter can allocate and transform a private input copy. */
    vv_default_options(&opts);
    opts.filter_x86 = 1;
    opts.window_log = 9;
    r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
    CHECK(r == VV_ERR_PARAM, "window_log=9 is rejected before BCJ work");
    opts.window_log = 25;
    r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
    CHECK(r == VV_ERR_PARAM, "window_log=25 is rejected before BCJ work");
    
    /* === vv_decompress NULL/bad-arg checks === */
    printf("\nvv_decompress edge cases:\n");
    
    r = vv_decompress(NULL, 100, buf, sizeof(buf));
    CHECK(r < 0, "NULL src returns error");
    
    r = vv_decompress(small, 8, NULL, 1024);
    CHECK(r < 0, "NULL dst returns error");
    
    /* Garbage input */
    uint8_t garbage[16] = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0,0,0,0,0,0,0,0};
    r = vv_decompress(garbage, 16, buf, sizeof(buf));
    CHECK(r < 0, "garbage input rejected (bad magic)");

    /* Mutually exclusive BCJ bits are a wire-format invariant as well as an
     * encoder option invariant.  Exercise all public header consumers. */
    vv_default_options(&opts);
    r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
    if (r > 0) buf[5] |= 0x0C;
    CHECK(r > 0 && vv_decompress(buf, (size_t)r, small, sizeof(small)) == VV_ERR_CORRUPT,
          "one-shot decoder rejects simultaneous BCJ flags");
    
    /* === vv_get_frame_info === */
    printf("\nvv_get_frame_info edge cases:\n");
    vv_frame_info_t info;
    ir = vv_get_frame_info(NULL, 100, &info);
    CHECK(ir < 0, "NULL src returns error");
    ir = vv_get_frame_info(small, 8, NULL);
    CHECK(ir < 0, "NULL info returns error");
    ir = vv_get_frame_info(small, 4, &info);
    CHECK(ir < 0, "src_len < header rejected");
    ir = r > 0 ? vv_get_frame_info(buf, (size_t)r, &info) : VV_ERR_CORRUPT;
    CHECK(ir == VV_ERR_CORRUPT, "frame info rejects simultaneous BCJ flags");
    
    /* === vv_cstream === */
    printf("\nvv_cstream edge cases:\n");
    vv_cstream_t *cs = vv_cstream_create(NULL);
    CHECK(cs != NULL, "create NULL opts succeeds");
    if (cs) vv_cstream_destroy(cs);
    
    vv_default_options(&opts);
    cs = vv_cstream_create(&opts);
    CHECK(cs != NULL, "create with defaults succeeds");
    if (cs) {
        size_t written = 0;
        ir = vv_cstream_compress_chunk(cs, NULL, 1, buf, sizeof(buf),
                                       &written, 0);
        CHECK(ir == VV_ERR_PARAM, "cstream rejects NULL chunk with nonzero length");
        ir = vv_cstream_reset(cs, NULL);
        CHECK(ir == 0, "reset NULL opts succeeds");
        vv_cstream_destroy(cs);
    }

    vv_default_options(&opts);
    opts.mode = (vv_mode_t)3;
    cs = vv_cstream_create(&opts);
    CHECK(cs == NULL, "cstream create rejects invalid mode");

    vv_default_options(&opts);
    opts.filter_auto = 1;
    cs = vv_cstream_create(&opts);
    CHECK(cs == NULL, "cstream create rejects unsupported BCJ filtering");

    vv_default_options(&opts);
    cs = vv_cstream_create(&opts);
    if (cs) {
        vv_options_t bad = opts;
        bad.mode = (vv_mode_t)-1;
        ir = vv_cstream_reset(cs, &bad);
        CHECK(ir == VV_ERR_PARAM, "cstream reset rejects invalid mode");
        bad = opts;
        bad.filter_x86 = 1;
        ir = vv_cstream_reset(cs, &bad);
        CHECK(ir == VV_ERR_PARAM, "cstream reset rejects unsupported BCJ filtering");
        vv_cstream_destroy(cs);
    } else {
        CHECK(0, "cstream reset rejects invalid mode");
        CHECK(0, "cstream reset rejects unsupported BCJ filtering");
    }
    
    /* destroy NULL: should be safe */
    vv_cstream_destroy(NULL);
    printf("  ✓ destroy NULL doesn't crash\n");
    
    /* === vv_dstream === */
    printf("\nvv_dstream edge cases:\n");
    vv_dstream_t *ds = vv_dstream_create();
    CHECK(ds != NULL, "create succeeds");
    if (ds) {
        size_t consumed = 0, written = 0;
        ir = vv_dstream_decompress_chunk(ds, NULL, 1, buf, sizeof(buf),
                                         &consumed, &written);
        CHECK(ir == VV_ERR_PARAM, "dstream rejects NULL src with nonzero length");
        ir = vv_dstream_reset(ds);
        CHECK(ir == 0, "reset succeeds");
        ir = r > 0 ? vv_dstream_decompress_chunk(ds, buf, (size_t)r,
                                                  small, sizeof(small),
                                                  &consumed, &written)
                   : VV_ERR_CORRUPT;
        CHECK(ir == VV_ERR_CORRUPT, "dstream rejects simultaneous BCJ flags");
        vv_dstream_destroy(ds);
    }
    vv_dstream_destroy(NULL);
    printf("  ✓ destroy NULL doesn't crash\n");

    /* Completion is idempotent, including the documented cumulative
     * written count. A post-completion call consumes no further input. */
    for (int checksum = 0; checksum < 2; checksum++) {
        vv_default_options(&opts);
        opts.checksum = checksum;
        r = vv_compress(small, sizeof(small), buf, sizeof(buf), &opts);
        ds = vv_dstream_create();
        int ok = r > 0 && ds != NULL;
        size_t consumed = 0, written = 0;
        if (ok) {
            ir = vv_dstream_decompress_chunk(ds, buf, (size_t)r, small,
                                              sizeof(small), &consumed, &written);
            ok = ir == 1 && consumed == (size_t)r && written == sizeof(small);
        }
        for (int repeat = 0; ok && repeat < 2; repeat++) {
            consumed = written = SIZE_MAX;
            ir = vv_dstream_decompress_chunk(ds, repeat ? buf : NULL,
                                              repeat ? (size_t)r : 0, small,
                                              sizeof(small), &consumed, &written);
            ok = ir == 1 && consumed == 0 && written == sizeof(small);
        }
        CHECK(ok, "completed dstream preserves cumulative written with and without new input");
        if (ds) {
            vv_dstream_reset(ds);
            ir = vv_dstream_decompress_chunk(ds, NULL, 0, small, sizeof(small),
                                              &consumed, &written);
            CHECK(ir == VV_OK && consumed == 0 && written == 0,
                  "dstream reset clears the completed cumulative written count");
        }
        vv_dstream_destroy(ds);
    }
    
    /* === vv_xxh64 === */
    printf("\nvv_xxh64 edge cases:\n");
    uint64_t h1 = vv_xxh64("", 0, 0);
    uint64_t h2 = vv_xxh64("", 0, 0);
    CHECK(h1 == h2, "empty input is deterministic");
    h1 = vv_xxh64("a", 1, 0);
    h2 = vv_xxh64("a", 1, 1);
    CHECK(h1 != h2, "different seed → different hash");
    /* NULL data with len=0 should be OK (per most hash APIs) */
    h1 = vv_xxh64(NULL, 0, 0);
    printf("  ✓ NULL data with len=0 returned 0x%lx\n", (unsigned long)h1);
    
    printf("\n%s: %d fails / %d total\n", fails ? "FAIL" : "PASS",
           fails, 35);
    return fails ? 1 : 0;
}
