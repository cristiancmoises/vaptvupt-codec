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
    
    /* === vv_get_frame_info === */
    printf("\nvv_get_frame_info edge cases:\n");
    vv_frame_info_t info;
    ir = vv_get_frame_info(NULL, 100, &info);
    CHECK(ir < 0, "NULL src returns error");
    ir = vv_get_frame_info(small, 8, NULL);
    CHECK(ir < 0, "NULL info returns error");
    ir = vv_get_frame_info(small, 4, &info);
    CHECK(ir < 0, "src_len < header rejected");
    
    /* === vv_cstream === */
    printf("\nvv_cstream edge cases:\n");
    vv_cstream_t *cs = vv_cstream_create(NULL);
    CHECK(cs != NULL, "create NULL opts succeeds");
    if (cs) vv_cstream_destroy(cs);
    
    vv_options_t opts; vv_default_options(&opts);
    cs = vv_cstream_create(&opts);
    CHECK(cs != NULL, "create with defaults succeeds");
    if (cs) {
        ir = vv_cstream_reset(cs, NULL);
        CHECK(ir == 0, "reset NULL opts succeeds");
        vv_cstream_destroy(cs);
    }
    
    /* destroy NULL: should be safe */
    vv_cstream_destroy(NULL);
    printf("  ✓ destroy NULL doesn't crash\n");
    
    /* === vv_dstream === */
    printf("\nvv_dstream edge cases:\n");
    vv_dstream_t *ds = vv_dstream_create();
    CHECK(ds != NULL, "create succeeds");
    if (ds) {
        ir = vv_dstream_reset(ds);
        CHECK(ir == 0, "reset succeeds");
        vv_dstream_destroy(ds);
    }
    vv_dstream_destroy(NULL);
    printf("  ✓ destroy NULL doesn't crash\n");
    
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
           fails, 17);
    return fails ? 1 : 0;
}
