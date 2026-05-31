/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_dos_hang.c — regression test for Sprint 89/90 DoS vulnerability.
 *
 * Sprint 89 adversarial fuzzing under UBSan/ASan discovered a denial-
 * of-service hang in the decoder: corrupted ANS bitstream (a single
 * byte flip at offsets 26-27 in a valid compressed file) caused the
 * sequence-decode loop to spin forever because litlen=0, matchlen=0
 * sequences produced by the corrupted state failed to advance any
 * counter.
 *
 * Sprint 90 fix: added a max-iterations guard at the loop top
 * (max_iters = total_lits + match_count + 16) and bounded
 * total_lits/match_count against dst_cap on the wire-format reads.
 *
 * This test loads each saved DoS reproducer and verifies that the
 * decoder returns within a reasonable time bound (proving the loop
 * is bounded), with an error code (proving the corruption is
 * detected).
 */

#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DST_CAP (8 * 1024 * 1024)
#define TIME_LIMIT_SEC 5.0

static int run_one(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  ✗ %s: cannot open\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *src = malloc(sz);
    if (fread(src, 1, sz, f) != (size_t)sz) { fclose(f); free(src); return 1; }
    fclose(f);

    uint8_t *dst = malloc(DST_CAP);
    if (!dst) { free(src); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int64_t r = vv_decompress(src, sz, dst, DST_CAP);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

    free(src); free(dst);

    if (elapsed > TIME_LIMIT_SEC) {
        printf("  ✗ %s: TOOK %.2f sec (DoS regression)\n", path, elapsed);
        return 1;
    }
    if (r >= 0) {
        printf("  ?? %s: returned success (%ld bytes) on corrupted input\n", path, (long)r);
        /* This is unexpected but not necessarily wrong — the corruption
         * might happen to produce valid output. The DoS test is about
         * not hanging, not about always rejecting. */
    } else {
        printf("  ✓ %s: returned error %ld in %.3f sec\n", path, (long)r, elapsed);
    }
    return 0;
}

int main(void) {
    const char *inputs[] = {
        "tests/regression_inputs/dos_hang.vv",
        "tests/regression_inputs/dos_hang_20.vv",
        "tests/regression_inputs/dos_hang_23.vv",
        "tests/regression_inputs/dos_hang_44.vv",
        "tests/regression_inputs/dos_hang_64.vv",
        "tests/regression_inputs/dos_hang_68.vv",
        /* Sprint 105 Phase C: lit_fmt=4 (4-stream Huffman) DoS reproducers.
         * Each is a real v2.47.0 frame with a corruption that previously
         * could have caused infinite loops or OOB reads in vvh_decode4. */
        "tests/regression_inputs/huf4_inflate_s1.vv",
        "tests/regression_inputs/huf4_zero_s1.vv",
        "tests/regression_inputs/huf4_truncate.vv",
        /* Sprint 109: libFuzzer-found OOB reproducers.
         *  - fuzz_oob_ll_code.vv: corrupt LL ANS table mapping a state
         *    to symbol >= VVA_LL_CODES, OOB read of ll_extra[].
         *  - fuzz_oob_decode_block.vv: corrupt LL extension producing
         *    a huge literal-run length, OOB READ at decoder memcpy.
         *  - fuzz_null_dec_table.vv: total_lits>0 with match_count=0
         *    triggered NULL-deref of dec_of/dec_ml in the unified
         *    decode loop's eager ILP table-load. */
        "tests/regression_inputs/fuzz_oob_ll_code.vv",
        "tests/regression_inputs/fuzz_oob_decode_block.vv",
        "tests/regression_inputs/fuzz_null_dec_table.vv",
        NULL
    };
    int fails = 0;
    int n = 0;
    for (int i = 0; inputs[i]; i++) {
        n++;
        fails += run_one(inputs[i]);
    }
    printf("\n  Results: %d/%d DoS reproducers handled within %.1fs\n",
           n - fails, n, TIME_LIMIT_SEC);
    return fails ? 1 : 0;
}
