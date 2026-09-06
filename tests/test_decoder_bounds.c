/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact-endpoint coverage for decoder framing, lengths and lookahead.
 *
 * One-shot calls use exact-sized input/output allocations (or a one-past
 * pointer for an empty span). ASan detects accesses outside these spans;
 * remaining-length checks also preserve the C pointer-formation invariant,
 * which ordinary ASan/UBSan do not necessarily diagnose on the old code.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned failures;

#define CHECK(condition, label) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

static size_t header(uint8_t *frame, uint8_t flags, uint8_t window, size_t size) {
    vv_frame_header_t fh;
    memset(&fh, 0, sizeof(fh));
    fh.magic = VV_MAGIC;
    fh.version = 1;
    fh.flags = flags;
    fh.window_log = window;
    fh.content_size = size;
    memcpy(frame, &fh, sizeof(fh));
    return sizeof(fh);
}

static size_t block(uint8_t *frame, size_t pos, vv_block_type_t type,
                    int last, uint32_t size) {
    uint32_t bh = vv_bh_pack(type, last, size);
    memcpy(frame + pos, &bh, sizeof(bh));
    return pos + sizeof(bh);
}

static size_t compressed_size(uint8_t *frame, size_t pos, uint32_t size) {
    frame[pos++] = (uint8_t)size;
    frame[pos++] = (uint8_t)(size >> 8);
    frame[pos++] = (uint8_t)(size >> 16);
    return pos;
}

static void decode_case(const char *label, const uint8_t *frame, size_t len,
                        size_t cap, int64_t expected, const uint8_t *plain) {
    for (unsigned skip = 0; skip <= 1; skip++) {
        uint8_t *input_storage = (uint8_t *)malloc(len ? len : 1);
        uint8_t *output_storage = (uint8_t *)malloc(cap ? cap : 1);
        CHECK(input_storage && output_storage, "exact allocations");
        if (!input_storage || !output_storage) {
            free(input_storage);
            free(output_storage);
            return;
        }
        uint8_t *input = input_storage + (len == 0);
        uint8_t *output = output_storage + (cap == 0);
        if (len) memcpy(input, frame, len);
        if (cap) memset(output, 0xA5, cap);
        int64_t result = skip ? vv_decompress_flags(input, len, output, cap,
                                                    VV_DECOMPRESS_SKIP_CHECKSUM)
                              : vv_decompress(input, len, output, cap);
        if (result != expected) {
            fprintf(stderr, "%s: length=%zu capacity=%zu skip=%u got=%lld expected=%lld\n",
                    label, len, cap, skip, (long long)result, (long long)expected);
        }
        CHECK(result == expected, label);
        CHECK(!len || memcmp(input, frame, len) == 0, "input unchanged");
        if (result >= 0 && plain && result == expected)
            CHECK(!result || memcmp(output, plain, (size_t)result) == 0, label);
        free(input_storage);
        free(output_storage);
    }
}

/* These are single-frame fixtures: no strict prefix is a complete frame. */
static void complete_frame(const char *label, const uint8_t *frame, size_t len,
                           const uint8_t *plain, size_t size) {
    decode_case(label, frame, len, size, (int64_t)size, plain);
    if (size) decode_case("short output", frame, len, size - 1,
                          VV_ERR_OVERFLOW, NULL);
    for (size_t cut = 0; cut < len; cut++)
        decode_case("truncated frame", frame, cut, size, VV_ERR_CORRUPT, NULL);
}

static void test_complete_frames(void) {
    uint8_t frame[256];
    uint8_t plain[80];
    memset(plain, 'A', sizeof(plain));
    for (unsigned checksum = 0; checksum <= 1; checksum++) {
        for (size_t size = 0; size <= 17; size += 17) {
            size_t n = header(frame, (uint8_t)checksum, 16, size);
            n = block(frame, n, VV_BLOCK_RAW, 1, (uint32_t)size);
            memcpy(frame + n, plain, size);
            n += size;
            if (checksum) {
                vv_frame_footer_t ff;
                memset(&ff, 0, sizeof(ff));
                ff.checksum = vv_xxh64(plain, size, 0);
                ff.footer_magic = 0x56564E44u;
                memcpy(frame + n, &ff, sizeof(ff));
                n += sizeof(ff);
            }
            complete_frame("RAW exact", frame, n, plain, size);
        }
    }
    size_t n = header(frame, 0, 16, 17);
    n = block(frame, n, VV_BLOCK_RLE, 1, 17);
    frame[n++] = 'A';
    complete_frame("RLE exact", frame, n, plain, 17);

    /* Plain zero-size payloads remain legal; entropy needs a tag byte. */
    n = header(frame, 0, 16, 0);
    n = block(frame, n, VV_BLOCK_COMPRESSED, 1, 0);
    n = compressed_size(frame, n, 0);
    complete_frame("empty plain block", frame, n, plain, 0);

    for (uint8_t window = 16; window <= 17; window++) {
        size_t off_bytes = window == 16 ? 2u : 3u;
        n = header(frame, 0, window, 5);
        n = block(frame, n, VV_BLOCK_COMPRESSED, 1, 5);
        n = compressed_size(frame, n, (uint32_t)(2 + off_bytes));
        frame[n++] = 0x10;
        frame[n++] = 'A';
        frame[n++] = 1;
        frame[n++] = 0;
        if (off_bytes == 3) frame[n++] = 0;
        complete_frame("match ends exactly", frame, n, plain, 5);

        n = header(frame, 0, window, 80);
        n = block(frame, n, VV_BLOCK_COMPRESSED, 1, 80);
        n = compressed_size(frame, n, 82);
        frame[n++] = 0xF0;
        frame[n++] = 65;
        memcpy(frame + n, plain, 80);
        n += 80;
        complete_frame("extended literals end exactly", frame, n, plain, 80);
    }
}

static void test_malformed_lengths(void) {
    uint8_t frame[128];
    size_t n = header(frame, 0, 16, VV_MAX_BLOCK_SIZE);
    n = block(frame, n, VV_BLOCK_RAW, 1, VV_MAX_BLOCK_SIZE);
    decode_case("RAW length beyond input", frame, n, VV_MAX_BLOCK_SIZE,
                VV_ERR_CORRUPT, NULL);
    decode_case("output error precedes missing RAW payload", frame, n, 0,
                VV_ERR_OVERFLOW, NULL);
    n = block(frame, sizeof(vv_frame_header_t), VV_BLOCK_RAW, 1,
              VV_MAX_BLOCK_SIZE + 1);
    decode_case("block exceeds format bound", frame, n, 0, VV_ERR_OVERFLOW, NULL);

    const vv_block_type_t types[] = { VV_BLOCK_COMPRESSED, VV_BLOCK_ENTROPY };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        n = header(frame, 0, 16, 0);
        n = block(frame, n, types[i], 1, 0);
        size_t size_pos = n;
        n = compressed_size(frame, n, 0xFFFFFFu);
        frame[n++] = 'H';
        for (size_t cut = size_pos; cut <= n; cut++)
            decode_case("compressed size beyond exact input", frame, cut, 0,
                        VV_ERR_CORRUPT, NULL);
    }
    n = header(frame, 0, 16, 0);
    n = block(frame, n, VV_BLOCK_ENTROPY, 1, 0);
    n = compressed_size(frame, n, 0);
    decode_case("entropy needs a tag", frame, n, 0, VV_ERR_CORRUPT, NULL);

    static const uint8_t tags[] = { 'H', 'A', 'I', 'C' };
    for (size_t i = 0; i < sizeof(tags); i++) {
        for (uint32_t section = 0; section <= 4; section++) {
            n = header(frame, 0, 16, 0);
            n = block(frame, n, VV_BLOCK_ENTROPY, 1, 0);
            n = compressed_size(frame, n, 1 + section);
            frame[n++] = tags[i];
            memset(frame + n, 0, section);
            if (section == 4) frame[n + 2] = 1; /* absent one-byte section */
            n += section;
            decode_case("short entropy section", frame, n, 0, VV_ERR_CORRUPT, NULL);
        }
    }
}

static void test_output_remaining(void) {
    uint8_t frame[128];
    static const uint8_t plain[] = "ABCDEFGH";
    size_t n = header(frame, 0, 16, 8);
    n = block(frame, n, VV_BLOCK_RAW, 0, 4);
    memcpy(frame + n, plain, 4); n += 4;
    size_t first_end = n;
    n = block(frame, n, VV_BLOCK_RAW, 1, 4);
    memcpy(frame + n, plain + 4, 4); n += 4;
    complete_frame("two blocks exact", frame, n, plain, 8);
    decode_case("second block output error precedes input error", frame, n - 1,
                7, VV_ERR_OVERFLOW, NULL);

    /* The streaming counterpart retains ERROR state and per-call counters. */
    for (size_t cap = 7; cap <= 8; cap++) {
        vv_dstream_t *ds = vv_dstream_create();
        uint8_t *output = (uint8_t *)malloc(cap);
        CHECK(ds && output, "stream allocation");
        if (!ds || !output) { vv_dstream_destroy(ds); free(output); continue; }
        size_t consumed = 99, written = 99;
        int result = vv_dstream_decompress_chunk(ds, frame, first_end, output, cap,
                                                 &consumed, &written);
        CHECK(result == VV_OK && consumed == first_end && written == 4,
              "stream first block counters");
        if (cap == 8) {
            result = vv_dstream_decompress_chunk(ds, frame + first_end, n - first_end,
                                                 output, 3, &consumed, &written);
            CHECK(result == VV_ERR_OVERFLOW && consumed == 0 && written == 0,
                  "stream rejects capacity below prior output before subtraction");
            /* Retry the same bytes below: this early rejection must not append
             * input or change the state to ERROR. The allocation is still 8. */
        }
        result = vv_dstream_decompress_chunk(ds, frame + first_end, n - first_end,
                                             output, cap, &consumed, &written);
        CHECK(result == (cap == 8 ? 1 : VV_ERR_OVERFLOW), "stream output remaining");
        CHECK(consumed == n - first_end && written == (cap == 8 ? 8u : 0u),
              "stream second block counters");
        result = vv_dstream_decompress_chunk(ds, NULL, 0, output, cap,
                                             &consumed, &written);
        CHECK(result == (cap == 8 ? 1 : VV_ERR_CORRUPT), "stream terminal state");
        CHECK(consumed == 0 && written == (cap == 8 ? 8u : 0u), "stream terminal counters");
        if (cap == 8) CHECK(memcmp(output, plain, 8) == 0, "stream exact bytes");
        vv_dstream_destroy(ds);
        free(output);
    }

    n = header(frame, 0, 16, 4);
    n = block(frame, n, VV_BLOCK_RAW, 1, 4);
    memcpy(frame + n, plain, 4); n += 4;
    size_t second_start = n;
    n += header(frame + n, 0, 16, 4);
    for (size_t cut = second_start + 1; cut < n; cut++)
        decode_case("partial next frame header", frame, cut, 8, VV_ERR_CORRUPT, NULL);
    n = block(frame, n, VV_BLOCK_RAW, 1, 4);
    memcpy(frame + n, plain + 4, 4); n += 4;
    decode_case("two frames exact", frame, n, 8, 8, plain);
    decode_case("second frame output exhausted", frame, n, 7, VV_ERR_OVERFLOW, NULL);
}

static void test_hot_lookahead(void) {
    uint8_t frame[1200];
    for (unsigned phase = 0; phase < 2; phase++) {
        size_t history = phase ? 1025u : 0u;
        size_t n = header(frame, 0, 10, history + 80);
        if (history) {
            n = block(frame, n, VV_BLOCK_RAW, 0, (uint32_t)history);
            memset(frame + n, 'A', history); n += history;
        }
        n = block(frame, n, VV_BLOCK_COMPRESSED, 1, 80);
        n = compressed_size(frame, n, 50);
        frame[n++] = 0x10;
        frame[n++] = 'x';
        frame[n++] = 0xFF;
        frame[n++] = 0xFF;
        memset(frame + n, 0, 46); n += 46;
        /* >48 input and >72 output bytes select AVX2 lookahead. The
         * offset is rejected, but must not form a before-history prefetch
         * pointer first. Scalar builds validate the same error contract. */
        decode_case(phase ? "hot prefetch invalid history" : "warmup prefetch invalid history",
                    frame, n, history + 80, VV_ERR_CORRUPT, NULL);
    }
}

int main(void) {
    test_complete_frames();
    test_malformed_lengths();
    test_output_remaining();
    test_hot_lookahead();
    printf("test_decoder_bounds: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
