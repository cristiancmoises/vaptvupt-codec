/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "vaptvupt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t seeds[] = {
    UINT64_C(0), UINT64_C(1), UINT64_C(0x9e3779b185ebca87)
};

/* Values computed with upstream libxxhash 0.8.3, XXH64. The binary input
 * byte at index i is (i * 37 + 11) modulo 256. No reference implementation
 * is linked into this test. */
static const struct {
    size_t len;
    uint64_t hash[3];
} vectors[] = {
    {0, {UINT64_C(0xef46db3751d8e999), UINT64_C(0xd5afba1336a3be4b), UINT64_C(0x6ec6d05f61c7e7a7)}},
    {1, {UINT64_C(0xf592c0c7639c4cb6), UINT64_C(0x653ce83baffa4f91), UINT64_C(0x11c15bc64227a259)}},
    {3, {UINT64_C(0x22c08528601d4f27), UINT64_C(0x5ebfda8c9072a30d), UINT64_C(0x680fb1b072b12822)}},
    {4, {UINT64_C(0xfb1e5cf2f1ae4d95), UINT64_C(0x76c235e6330f623d), UINT64_C(0x1a24432ce8cca8ab)}},
    {7, {UINT64_C(0x5613ac510496c04e), UINT64_C(0xe75581b7bfa15ec7), UINT64_C(0xeb83e677abc7457f)}},
    {8, {UINT64_C(0x57cb2b7521f3e21a), UINT64_C(0xfbbee60ba1ff4594), UINT64_C(0x0eacb01d241d894d)}},
    {9, {UINT64_C(0xc3f1d09775257b66), UINT64_C(0xb07b63c711f0f9ba), UINT64_C(0x9b98a7a55393f326)}},
    {15, {UINT64_C(0x90a9714eb00e8d29), UINT64_C(0xa362247c52f76975), UINT64_C(0x6e0609ff46df0f7e)}},
    {16, {UINT64_C(0xc6843cef99721174), UINT64_C(0x8381704f10794f13), UINT64_C(0x23a8cf6ad53b5290)}},
    {31, {UINT64_C(0xe4a0e629e519a4ae), UINT64_C(0x30c494e3d8ea5c92), UINT64_C(0x5c59f004b6611b70)}},
    {32, {UINT64_C(0xcc6b8aaada790b2d), UINT64_C(0xe73accc75e541b60), UINT64_C(0x7b07232241d2ce0c)}},
    {33, {UINT64_C(0x35ec49850475a832), UINT64_C(0x0fd4b34807971da4), UINT64_C(0x7b94d2f19b051eff)}},
    {63, {UINT64_C(0xbf9f0ba3cf95b28a), UINT64_C(0x09abc95b9a7fa569), UINT64_C(0xa4bdf9e7d15f7ed0)}},
    {64, {UINT64_C(0x155ccce4bf32befc), UINT64_C(0x166dfdfdb0c6703e), UINT64_C(0xd10dfae7e9e3edec)}},
    {65, {UINT64_C(0xda5ee441a595cfb8), UINT64_C(0xba4c674ad6c934a1), UINT64_C(0xf9a52a50c1b7ebf1)}},
    {95, {UINT64_C(0xeb8fa2edc9550675), UINT64_C(0x14e9fd5f9786eb08), UINT64_C(0xb7edda411944622e)}},
    {96, {UINT64_C(0x7c874b795fa7ab0c), UINT64_C(0x72852dd1605eb34c), UINT64_C(0x1fc4cefcd63e5391)}},
    {97, {UINT64_C(0x665aae9ff64096d9), UINT64_C(0xfb0aa67069eb2bbd), UINT64_C(0xfcfc09ec0df746c8)}},
    {255, {UINT64_C(0x178ea4a7d2319abe), UINT64_C(0x6c102d1fbdaed12f), UINT64_C(0x83af98e226220a71)}},
    {256, {UINT64_C(0x43c92f09cb3e28cf), UINT64_C(0xef0337c0c37a2ab2), UINT64_C(0x81f1b18917fe0860)}},
    {257, {UINT64_C(0x5f0f93c2b91fa7db), UINT64_C(0x74a65f87d9558e26), UINT64_C(0x36c9ec886f37e9e7)}}
};

static size_t checks;

static int check_hash(uint64_t actual, uint64_t expected, const char *phase,
                      size_t len, size_t offset, size_t split, uint64_t seed)
{
    checks++;
    if (actual == expected) return 1;
    fprintf(stderr,
            "%s: len=%zu offset=%zu split=%zu seed=%016" PRIx64
            " got=%016" PRIx64 " expected=%016" PRIx64 "\n",
            phase, len, offset, split, seed, actual, expected);
    return 0;
}

static int check_streams(const uint8_t *data, size_t len, size_t offset,
                         uint64_t seed, uint64_t expected)
{
    vv_xxh64_state_t state;

    for (size_t split = 0; split <= len; split++) {
        /* Initialization must make reuse safe even with stale buffer bytes. */
        memset(&state, 0xa5, sizeof(state));
        vv_xxh64_init(&state, seed);
        vv_xxh64_update(&state, data, split);
        uint64_t prefix = vv_xxh64(data, split, seed);
        if (!check_hash(vv_xxh64_finalize(&state), prefix, "prefix",
                        len, offset, split, seed)) return 0;

        vv_xxh64_update(&state, NULL, 0);
        vv_xxh64_update(&state, data + len, 0);
        if (!check_hash(vv_xxh64_finalize(&state), prefix, "zero update",
                        len, offset, split, seed)) return 0;

        /* Finalizing a prefix must permit later updates to the same state. */
        vv_xxh64_update(&state, data + split, len - split);
        if (!check_hash(vv_xxh64_finalize(&state), expected, "split",
                        len, offset, split, seed)) return 0;
        if (!check_hash(vv_xxh64_finalize(&state), expected, "repeat finalize",
                        len, offset, split, seed)) return 0;
    }

    vv_xxh64_init(&state, seed);
    for (size_t i = 0; i < len; i++) {
        vv_xxh64_update(&state, data + i, 1);
        vv_xxh64_update(&state, NULL, 0);
    }
    return check_hash(vv_xxh64_finalize(&state), expected, "byte updates",
                      len, offset, 0, seed);
}

int main(void)
{
    for (size_t seed_index = 0; seed_index < 3; seed_index++) {
        vv_xxh64_state_t state;
        uint64_t seed = seeds[seed_index];
        uint64_t expected = vectors[0].hash[seed_index];
        if (!check_hash(vv_xxh64(NULL, 0, seed), expected, "empty",
                        0, 0, 0, seed)) return 1;
        vv_xxh64_init(&state, seed);
        vv_xxh64_update(&state, NULL, 0);
        if (!check_hash(vv_xxh64_finalize(&state), expected, "empty stream",
                        0, 0, 0, seed)) return 1;
    }

    for (size_t len = 1; len <= 257; len++) {
        for (size_t offset = 0; offset < 8; offset++) {
            /* No readable tail padding: data ends at the allocation boundary.
             * Offsets exercise every alignment of the 8-byte loads. */
            uint8_t *storage = malloc(len + offset);
            if (!storage) {
                fputs("FAIL: checksum test allocation\n", stderr);
                return 1;
            }
            uint8_t *data = storage + offset;
            for (size_t i = 0; i < len; i++)
                data[i] = (uint8_t)(i * 37 + 11);

            for (size_t seed_index = 0; seed_index < 3; seed_index++) {
                uint64_t seed = seeds[seed_index];
                uint64_t expected = vv_xxh64(data, len, seed);
                for (size_t i = 1; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
                    if (vectors[i].len != len) continue;
                    if (!check_hash(expected, vectors[i].hash[seed_index],
                                    "known vector", len, offset, 0, seed)) {
                        free(storage);
                        return 1;
                    }
                }
                if (!check_streams(data, len, offset, seed, expected)) {
                    free(storage);
                    return 1;
                }
            }
            free(storage);
        }
    }

    printf("PASS: %zu XXH64 checks (vectors, exact buffers, alignment, streaming)\n",
           checks);
    return 0;
}
