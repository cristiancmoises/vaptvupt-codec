/* Compile-test for the code samples in INTEGRATION.md.
 *
 * If this file fails to build, the integration documentation is broken.
 * Sprint 113: surfaced + fixed; this file ensures it stays correct. */

#include "vaptvupt.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* Sample 1: encode path */
int vaptvupt_compress_for_archive(const uint8_t *plaintext, size_t plaintext_len,
                              uint8_t **out_buf, size_t *out_len) {
    vv_options_t opts;
    vv_default_options(&opts);
    opts.mode = VV_MODE_EXTREME;
    opts.format_v2 = 1;
    opts.checksum = 0;

    size_t cap = vv_compress_bound(plaintext_len);
    uint8_t *buf = malloc(cap);
    if (!buf) return -1;

    int64_t clen = vv_compress(plaintext, plaintext_len, buf, cap, &opts);
    if (clen < 0) { free(buf); return -1; }

    *out_buf = buf;
    *out_len = (size_t)clen;
    return 0;
}

/* Sample 2: decode path */
int vaptvupt_decompress_from_archive(const uint8_t *cmp, size_t cmp_len,
                                 uint8_t *dst, size_t dst_cap,
                                 size_t *decoded_len) {
    int64_t dlen = vv_decompress_flags(cmp, cmp_len, dst, dst_cap,
                                        VV_DECOMPRESS_SKIP_CHECKSUM);
    if (dlen < 0) return -1;
    *decoded_len = (size_t)dlen;
    return 0;
}

/* Sample 3: streaming encode */
int sample_streaming_encode(const uint8_t *src, size_t src_len,
                            uint8_t *out_buf, size_t out_cap) {
    vv_options_t opts;
    vv_default_options(&opts);

    vv_cstream_t *cs = vv_cstream_create(&opts);
    if (!cs) return -1;

    /* Single-call simplification of the loop in the doc */
    size_t written = 0;
    int rc = vv_cstream_compress_chunk(cs, src, src_len,
                                       out_buf, out_cap,
                                       &written, /* is_last= */ 1);
    vv_cstream_destroy(cs);
    return rc;
}

/* Sample 4: streaming decode */
int sample_streaming_decode(const uint8_t *cmp, size_t cmp_len,
                            uint8_t *dst_buf, size_t dst_cap) {
    vv_dstream_t *ds = vv_dstream_create();
    if (!ds) return -1;

    size_t consumed = 0, written = 0;
    int rc = vv_dstream_decompress_chunk(ds, cmp, cmp_len,
                                         dst_buf, dst_cap,
                                         &consumed, &written);
    vv_dstream_destroy(ds);
    return rc;
}

int main(void) {
    /* Just a link-test — don't actually run. */
    (void)vaptvupt_compress_for_archive;
    (void)vaptvupt_decompress_from_archive;
    (void)sample_streaming_encode;
    (void)sample_streaming_decode;
    return 0;
}
