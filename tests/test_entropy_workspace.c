/* SPDX-License-Identifier: GPL-3.0-or-later
 * Caller-owned entropy tables and SEQ scratch-lifetime regression tests.
 * Define VV_TEST_ALLOC_WRAP and link --wrap=malloc/calloc/realloc/free to
 * prove workspace calls allocate nothing and SEQ decoding allocates once.
 */
#include "vv_ans.h"
#include "vv_huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 0; \
} } while (0)

#ifdef VV_TEST_ALLOC_WRAP
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
/* --wrap redirects calls after LTO, so the optimizer cannot see the wrapper
 * side effects at the original malloc/free call sites. Keep instrumentation
 * state observable across those linker-mediated calls. */
static volatile int tracking;
static volatile size_t allocs, frees, allowed;
static int reject_allocation(void) {
    return tracking && allocs++ >= allowed;
}
void *__wrap_malloc(size_t n) {
    return reject_allocation() ? NULL : __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t size) {
    return reject_allocation() ? NULL : __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t n) {
    return reject_allocation() ? NULL : __real_realloc(p, n);
}
void __wrap_free(void *p) {
    if (tracking && p) frees++;
    __real_free(p);
}
static void allocation_budget(size_t n) {
    allocs = frees = 0;
    allowed = n;
    tracking = 1;
}
static int allocation_result(size_t expected_allocs, size_t expected_frees) {
    tracking = 0;
    if (allocs != expected_allocs || frees != expected_frees)
        fprintf(stderr, "Allocation counts: got %zu allocs / %zu frees, expected %zu / %zu\n",
                allocs, frees, expected_allocs, expected_frees);
    return allocs == expected_allocs && frees == expected_frees;
}
#else
static void allocation_budget(size_t n) { (void)n; }
static int allocation_result(size_t a, size_t f) { (void)a; (void)f; return 1; }
#endif

typedef vv_error_t (*encode_fn)(const uint8_t *, size_t, uint8_t *, size_t, size_t *);
typedef vv_error_t (*decode_fn)(const uint8_t *, size_t, uint8_t *, size_t,
                                size_t, size_t *);
typedef vv_error_t (*workspace_fn)(const uint8_t *, size_t, uint8_t *, size_t,
                                   size_t, size_t *, void *, size_t);
typedef struct {
    encode_fn encode;
    decode_fn decode;
    workspace_fn decode_workspace;
    size_t (*workspace_size)(void);
    size_t (*workspace_alignment)(void);
} codec_t;
static const codec_t codecs[] = {
    {vva_encode4, vva_decode4, vva_decode4_with_workspace,
     vva_decode_workspace_size, vva_decode_workspace_alignment},
    {vva_encode, vva_decode, vva_decode_with_workspace,
     vva_decode_workspace_size, vva_decode_workspace_alignment},
    {vvh_encode, vvh_decode, vvh_decode_with_workspace,
     vvh_decode_workspace_size, vvh_decode_workspace_alignment},
    {vvh_encode4, vvh_decode4, vvh_decode4_with_workspace,
     vvh_decode_workspace_size, vvh_decode_workspace_alignment}
};

static int literal_workspace_tests(const codec_t *codec, int ans) {
    uint8_t data[4096], decoded[4096], reference[4096];
    size_t size = codec->workspace_size();
    size_t alignment = codec->workspace_alignment();
    size_t encoded_cap = vva_bound(sizeof(data));
    uint8_t *encoded = malloc(encoded_cap);
    uint8_t *workspace = malloc(size + 16);
    CHECK(encoded && workspace && size > 0 && alignment > 0);
    CHECK((uintptr_t)workspace % alignment == 0);

    /* Reuse one region for single-symbol, sparse and dense alphabets. */
    for (int phase = 0; phase < 3; phase++) {
        for (size_t i = 0; i < sizeof(data); i++)
            data[i] = phase == 0 ? 231 : (uint8_t)(17 + i % (phase == 1 ? 7 : 67));
        size_t encoded_len = 0, consumed = 0, reference_consumed = 0;
        CHECK(codec->encode(data, sizeof(data), encoded, encoded_cap, &encoded_len) == VV_OK);
        CHECK(codec->decode(encoded, encoded_len, reference, sizeof(reference),
                             sizeof(data), &reference_consumed) == VV_OK);
        memset(workspace, 0xA5, size + 16);

        CHECK(codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded),
                  sizeof(data), &consumed, workspace, size - 1) == VV_ERR_OVERFLOW);
        if (alignment > 1)
            CHECK(codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded),
                  sizeof(data), &consumed, workspace + 1, size) == VV_ERR_PARAM);
        CHECK(codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded),
                  sizeof(data), &consumed, NULL, size) == VV_ERR_PARAM);
        CHECK(codec->decode_workspace(NULL, encoded_len, decoded, sizeof(decoded),
                  sizeof(data), &consumed, workspace, size) == VV_ERR_PARAM);
        CHECK(codec->decode_workspace(encoded, encoded_len, NULL, sizeof(decoded),
                  sizeof(data), &consumed, workspace, size) == VV_ERR_PARAM);
        CHECK(codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded),
                  sizeof(data), NULL, workspace, size) == VV_ERR_PARAM);
        CHECK(codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded) - 1,
                  sizeof(data), &consumed, workspace, size) == VV_ERR_OVERFLOW);
        for (size_t i = 0; i < size + 16; i++) CHECK(workspace[i] == 0xA5);

        allocation_budget(0);
        vv_error_t err = codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded),
                              sizeof(data), &consumed, workspace, size);
        int no_allocations = allocation_result(0, 0);
        CHECK(no_allocations && err == VV_OK);
        CHECK(consumed == reference_consumed);
        CHECK(memcmp(decoded, reference, sizeof(data)) == 0);
        CHECK(memcmp(decoded, data, sizeof(data)) == 0);
        for (size_t i = size; i < size + 16; i++) CHECK(workspace[i] == 0xA5);

        if (phase > 0) {
            /* Fail after building the table, then reuse it successfully.
             * ANS gets an invalid initial state; Huffman loses code bits. */
            size_t hdr = encoded[0] == VVA_HDR_SPARSE ? 2 + 3 * (size_t)encoded[1] :
                         2 + 2 * ((size_t)encoded[1] + 1);
            uint8_t saved[2] = {0, 0};
            if (ans) {
                saved[0] = encoded[hdr]; saved[1] = encoded[hdr + 1];
                encoded[hdr] = encoded[hdr + 1] = 0xFF;
            }
            allocation_budget(0);
            err = codec->decode_workspace(encoded, encoded_len - (ans ? 0 : 1),
                          decoded, sizeof(decoded), sizeof(data), &consumed, workspace, size);
            no_allocations = allocation_result(0, 0);
            CHECK(no_allocations && err == VV_ERR_CORRUPT);
            if (ans) { encoded[hdr] = saved[0]; encoded[hdr + 1] = saved[1]; }
            CHECK(codec->decode_workspace(encoded, encoded_len, decoded, sizeof(decoded),
                      sizeof(data), &consumed, workspace, size) == VV_OK);
            CHECK(memcmp(decoded, data, sizeof(data)) == 0);
            for (size_t i = size; i < size + 16; i++) CHECK(workspace[i] == 0xA5);
        }
    }

    size_t consumed = 123;
    CHECK(codec->decode_workspace(NULL, 0, NULL, 0, 0, &consumed,
                                  workspace + 1, 0) == VV_OK);
    CHECK(consumed == 0);
    CHECK(codec->decode_workspace(NULL, 0, NULL, 0, 0, NULL, NULL, 0) == VV_ERR_PARAM);
    CHECK(codec->decode_workspace(NULL, 1, NULL, 0, 0, &consumed, NULL, 0) == VV_ERR_PARAM);
    CHECK(codec->decode_workspace(NULL, 0, NULL, 1, 0, &consumed, NULL, 0) == VV_ERR_PARAM);
    free(workspace);
    free(encoded);
    return 1;
}

static void put32(uint8_t *p, uint32_t value) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (i * 8));
}

static int sequence_workspace_tests(void) {
    uint8_t data[1024], decoded[1044];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i & 1);
    size_t cap = vva_bound(sizeof(data)) + 64;
    uint8_t *encoded = malloc(cap);
    CHECK(encoded != NULL);
    for (unsigned format = 1; format <= 4; format++) {
        size_t encoded_len = 0;
        CHECK(codecs[format - 1].encode(data, sizeof(data), encoded + 9,
                        cap - 64, &encoded_len) == VV_OK);
        put32(encoded, sizeof(data));
        encoded[4] = (uint8_t)format;
        put32(encoded + 5, (uint32_t)encoded_len);
        for (int matches = 0; matches <= 1; matches++) {
            uint8_t *p = encoded + 9 + encoded_len;
            put32(p, (uint32_t)matches); p += 4;
            const uint8_t symbols[3] = {0, 3, 27}; /* ML min, OF 1, LL 1024 */
            for (int table = 0; table < 3; table++) {
                if (matches || table == 2) {
                    *p++ = 2; *p++ = 0;
                    *p++ = VVA_HDR_SINGLE; *p++ = symbols[table];
                } else { *p++ = 0; *p++ = 0; }
            }
            memset(p, 0, 6); p += 6; /* three initial ANS states */
            put32(p, 2); p += 4;
            *p++ = 0; *p++ = 0;     /* ten LL extra bits, all zero */
            for (int version = 1; version <= 2; version++) {
                size_t expected = sizeof(data) + (matches ? (version == 1 ? 4 : 3) : 0);
                size_t written = 0;
                memset(decoded, 0xA5, sizeof(decoded));
                allocation_budget(1); /* reject any nested entropy allocation */
                vv_error_t err = version == 1
                    ? vva_decode_sequences(encoded, (size_t)(p - encoded), decoded,
                                           expected, &written, decoded)
                    : vva_decode_sequences_v2(encoded, (size_t)(p - encoded), decoded,
                                              expected, &written, decoded);
                int one_allocation = allocation_result(1, 1);
                if (!one_allocation || err != VV_OK || written != expected)
                    fprintf(stderr, "SEQ format %u, matches %d, version %d: err %d, written %zu, expected %zu\n",
                            format, matches, version, (int)err, written, expected);
                CHECK(one_allocation && err == VV_OK && written == expected);
                CHECK(memcmp(decoded, data, sizeof(data)) == 0);
                for (size_t i = sizeof(data); i < expected; i++) CHECK(decoded[i] == 1);
                for (size_t i = expected; i < sizeof(decoded); i++) CHECK(decoded[i] == 0xA5);
#ifdef VV_TEST_ALLOC_WRAP
                /* Also prove that the wrapper really intercepts this decode's
                 * arena allocation; a disabled hook must not silently pass. */
                written = 123;
                memset(decoded, 0xA5, sizeof(decoded));
                allocation_budget(0);
                err = version == 1
                    ? vva_decode_sequences(encoded, (size_t)(p - encoded), decoded,
                                           expected, &written, decoded)
                    : vva_decode_sequences_v2(encoded, (size_t)(p - encoded), decoded,
                                              expected, &written, decoded);
                int rejected = allocation_result(1, 0);
                CHECK(rejected && err == VV_ERR_NOMEM);
                for (size_t i = 0; i < sizeof(decoded); i++) CHECK(decoded[i] == 0xA5);
#endif
            }
        }
    }
    free(encoded);
    return 1;
}

int main(void) {
    for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++)
        if (!literal_workspace_tests(&codecs[i], i < 2)) return 1;
    if (!sequence_workspace_tests()) return 1;
    printf("Entropy workspace tests: %u checks passed", checks);
#ifdef VV_TEST_ALLOC_WRAP
    printf(" (allocation failure injection enabled)");
#endif
    printf("\n");
    return 0;
}
