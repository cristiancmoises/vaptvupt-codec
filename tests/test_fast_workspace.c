/* SPDX-License-Identifier: GPL-3.0-or-later
 * Caller-owned FAST context: legacy byte parity, reuse and checked ownership.
 * Define VV_TEST_ALLOC_WRAP and link --wrap=malloc/calloc/realloc/free for
 * zero-allocation checks. The counters and probe pointers remain observable
 * when the linker applies wrapping after LTO.
 */
#include "vaptvupt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_MAX 65536u
#define GUARD 64u
#define CANARY 0xA5

static unsigned checks, parity_cases, raw_cases, token_cases;
static char test_case[160] = "setup";
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL [%s] line %d: %s\n", test_case, __LINE__, #x); \
    return 0; \
} } while (0)

#ifdef VV_TEST_ALLOC_WRAP
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
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
    if (tracking) frees++;
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
        fprintf(stderr, "Allocation counts [%s]: %zu / %zu; expected %zu / %zu\n",
                test_case, allocs, frees, expected_allocs, expected_frees);
    return allocs == expected_allocs && frees == expected_frees;
}
#else
static void allocation_budget(size_t n) { (void)n; }
static int allocation_result(size_t a, size_t f) { (void)a; (void)f; return 1; }
#endif

typedef struct {
    uint8_t *allocation;
    uint8_t *data;
    size_t cap;
} guarded_t;

static int filled(const uint8_t *p, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; i++) if (p[i] != value) return 0;
    return 1;
}

static int guarded_alloc(guarded_t *g, size_t cap, size_t alignment) {
    if (!alignment || cap > SIZE_MAX - 2 * GUARD - alignment) return 0;
    g->allocation = malloc(cap + 2 * GUARD + alignment);
    if (!g->allocation) return 0;
    uintptr_t start = (uintptr_t)(g->allocation + GUARD);
    size_t adjustment = (alignment - start % alignment) % alignment;
    g->data = g->allocation + GUARD + adjustment;
    g->cap = cap;
    memset(g->data - GUARD, CANARY, cap + 2 * GUARD);
    return 1;
}

static int guarded_ok(const guarded_t *g) {
    return filled(g->data - GUARD, GUARD, CANARY) &&
           filled(g->data + g->cap, GUARD, CANARY);
}

static void make_input(uint8_t *p, size_t n, unsigned kind) {
    uint32_t state = 0x971AE385u;
    for (size_t i = 0; i < n; i++) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        if (kind == 0) p[i] = (uint8_t)(state >> 16); /* Usually RAW. */
        else if (kind == 1) p[i] = 0xE7;             /* Short overlap. */
        else if (kind == 2) p[i] = (uint8_t)(33 + i % 67);
        else p[i] = (uint8_t)(state >> 16);
    }
    /* Incompressible prefix followed by distant repeats exercises both
     * advertised offset widths and circular chains in smaller windows. */
    if (kind == 3 && n > 2048) {
        size_t period = n > 32768 ? 20003 : n / 3;
        for (size_t i = period; i < n; i++) p[i] = p[i - period];
    }
}

static void fast_options(vv_options_t *o, uint8_t window, unsigned variant) {
    vv_default_options(o);
    o->mode = VV_MODE_ULTRA_FAST;
    o->window_log = window;
    switch (variant) {
    case 1: o->checksum = 0; break;
    case 2: o->depth_override = 1; o->accel = 1; o->no_rep = 1; break;
    case 3: o->depth_override = 4096; o->accel = 64; break;
    case 4: o->depth_override = 4097; o->accel = 65; o->no_rep = 1; break;
    case 5: o->format_v2 = 1; o->compat_v246_5_decoder = 1; break;
    case 6: o->checksum = -1; o->no_rep = -1; break;
    case 7:
        o->depth_override = 7; o->accel = 3; o->no_rep = 1;
        o->format_v2 = 1;
        break;
    default: break;
    }
}

static int hook_tests(void) {
#ifdef VV_TEST_ALLOC_WRAP
    /* Volatile indirect calls cannot be optimized away as unused allocator
     * pairs; every linker wrapper is checked rather than assumed active. */
    void *(*volatile call_malloc)(size_t) = malloc;
    void *(*volatile call_calloc)(size_t, size_t) = calloc;
    void *(*volatile call_realloc)(void *, size_t) = realloc;
    void (*volatile call_free)(void *) = free;
    strcpy(test_case, "allocator interception");
    allocation_budget(0);
    void *p = call_malloc(32);
    int intercepted = allocation_result(1, 0);
    CHECK(intercepted && p == NULL);
    allocation_budget(0);
    p = call_calloc(8, 4);
    intercepted = allocation_result(1, 0);
    CHECK(intercepted && p == NULL);
    p = call_malloc(32);
    CHECK(p != NULL);
    allocation_budget(0);
    void *replacement = call_realloc(p, 64);
    intercepted = allocation_result(1, 0);
    CHECK(intercepted && replacement == NULL);
    allocation_budget(0);
    call_free(p);
    intercepted = allocation_result(0, 1);
    CHECK(intercepted);

    /* A real legacy codec call must also hit allocation failure. This
     * distinguishes observable test probes from successful codec wrapping. */
    vv_options_t opts;
    fast_options(&opts, 16, 0);
    uint8_t source[32] = {0}, output[512];
    allocation_budget(0);
    int64_t result = vv_compress(source, sizeof(source), output, sizeof(output), &opts);
    tracking = 0;
    CHECK(allocs > 0 && result == VV_ERR_NOMEM);
#endif
    return 1;
}

static int invalid_init(guarded_t *storage, size_t max_input,
                        const vv_options_t *opts, int expected,
                        size_t supplied_cap, size_t skew, int null_storage,
                        int null_output) {
    memset(storage->data, CANARY, storage->cap);
    vv_fast_context_t *ctx = (vv_fast_context_t *)(void *)storage->data;
    allocation_budget(0);
    int result = vv_fast_context_init(null_storage ? NULL : storage->data + skew,
                      supplied_cap, max_input, opts, null_output ? NULL : &ctx);
    int no_allocations = allocation_result(0, 0);
    CHECK(no_allocations && result == expected);
    CHECK(null_output || ctx == NULL);
    CHECK(filled(storage->data, storage->cap, CANARY));
    CHECK(guarded_ok(storage));
    return 1;
}

static int init_tests(void) {
    strcpy(test_case, "context init validation");
    size_t alignment = vv_fast_context_alignment();
    CHECK(alignment > 0);
    CHECK(vv_fast_context_size(0) == 0);
    CHECK(vv_fast_context_size(PAGE_MAX + 1) == 0);
    CHECK(vv_fast_context_size(SIZE_MAX) == 0);
    /* Resource ceilings, not fixed layout or ABI sizes. Allow metadata to
     * evolve without silently restoring the full-width page matcher. */
    CHECK(vv_fast_context_size(4096) < 600u * 1024u);
    CHECK(vv_fast_context_size(PAGE_MAX) < 750u * 1024u);
    static const size_t maxima[] = {1, 4096, 16384, PAGE_MAX};
    size_t previous_size = 0;
    for (size_t k = 0; k < sizeof(maxima) / sizeof(maxima[0]); k++) {
        size_t maximum = maxima[k], size = vv_fast_context_size(maximum);
        CHECK(size > previous_size);
        previous_size = size;
        guarded_t storage;
        CHECK(guarded_alloc(&storage, size, alignment));
        CHECK((uintptr_t)storage.data % alignment == 0);
        vv_options_t opts;
        fast_options(&opts, 16, 0);
        CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_OVERFLOW, size - 1, 0, 0, 0));
        CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 0, 1, 0));
        CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 0, 0, 1));
        CHECK(invalid_init(&storage, maximum, NULL, VV_ERR_PARAM, size, 0, 0, 0));
        if (alignment > 1)
            CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 1, 0, 0));
        CHECK(invalid_init(&storage, 0, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        CHECK(invalid_init(&storage, PAGE_MAX + 1, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        CHECK(invalid_init(&storage, SIZE_MAX, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        static const int bad_modes[] = {-1, VV_MODE_BALANCED, VV_MODE_EXTREME, 3};
        for (size_t i = 0; i < sizeof(bad_modes) / sizeof(bad_modes[0]); i++) {
            opts.mode = (vv_mode_t)bad_modes[i];
            CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        }
        fast_options(&opts, 9, 0);
        CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        opts.window_log = 25;
        CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        for (unsigned filter = 0; filter < 3; filter++) {
            fast_options(&opts, 16, 0);
            if (filter == 0) opts.filter_x86 = 1;
            if (filter == 1) opts.filter_arm64 = 1;
            if (filter == 2) opts.filter_auto = 1;
            CHECK(invalid_init(&storage, maximum, &opts, VV_ERR_PARAM, size, 0, 0, 0));
        }
        fast_options(&opts, 0, 0);
        vv_fast_context_t *ctx = NULL;
        allocation_budget(0);
        int result = vv_fast_context_init(storage.data, size, maximum, &opts, &ctx);
        int no_allocations = allocation_result(0, 0);
        CHECK(no_allocations && result == VV_OK && ctx != NULL);
        CHECK((uintptr_t)ctx >= (uintptr_t)storage.data);
        CHECK((uintptr_t)ctx < (uintptr_t)storage.data + size);
        CHECK(guarded_ok(&storage));
        free(storage.allocation);
    }
    return 1;
}

static int parity(vv_fast_context_t *ctx, const vv_options_t *opts,
                  const uint8_t *src, size_t n, guarded_t *output,
                  guarded_t *reference, guarded_t *decoded) {
    size_t bound = vv_compress_bound(n);
    memset(output->data, CANARY, output->cap);
    memset(reference->data, CANARY, reference->cap);
    memset(decoded->data, CANARY, decoded->cap);
    const uint8_t empty = 0;
    int64_t legacy = vv_compress(src ? src : &empty, n, reference->data, bound, opts);
    CHECK(legacy > 0 && (uint64_t)legacy <= bound);
    allocation_budget(0);
    int64_t actual = vv_fast_context_compress(ctx, src, n, output->data, bound);
    int no_allocations = allocation_result(0, 0);
    CHECK(no_allocations && actual == legacy);
    CHECK(memcmp(output->data, reference->data, (size_t)legacy) == 0);
    uint32_t header = vv_read32(output->data + sizeof(vv_frame_header_t));
    CHECK(vv_bh_last(header) && vv_bh_size(header) == n);
    CHECK(vv_bh_type(header) == VV_BLOCK_RAW || vv_bh_type(header) == VV_BLOCK_COMPRESSED);
    if (vv_bh_type(header) == VV_BLOCK_RAW) raw_cases++;
    else token_cases++;
    CHECK(filled(output->data + actual, output->cap - (size_t)actual, CANARY));
    CHECK(guarded_ok(output) && guarded_ok(reference));
    allocation_budget(0);
    int64_t restored = vv_decompress(output->data, (size_t)actual, decoded->data, n);
    no_allocations = allocation_result(0, 0);
    CHECK(no_allocations && restored == (int64_t)n);
    CHECK(n == 0 || memcmp(src, decoded->data, n) == 0);
    CHECK(filled(decoded->data + n, decoded->cap - n, CANARY));
    CHECK(guarded_ok(decoded));
    parity_cases++;
    return 1;
}

static int rejected_compress(vv_fast_context_t *ctx, const uint8_t *src,
                             size_t n, guarded_t *output, size_t cap,
                             int null_dst, int expected) {
    memset(output->data, CANARY, output->cap);
    allocation_budget(0);
    int64_t result = vv_fast_context_compress(ctx, src, n,
                                             null_dst ? NULL : output->data, cap);
    int no_allocations = allocation_result(0, 0);
    CHECK(no_allocations && result == expected);
    CHECK(filled(output->data, output->cap, CANARY));
    CHECK(guarded_ok(output));
    return 1;
}

/* Inspect match positions in the existing plain token grammar. Round-trip
 * decoding is checked by parity(); this walker establishes that a fixture
 * actually uses high dictionary positions, rather than merely being large. */
static int high_references(const guarded_t *frame, size_t raw_size,
                           unsigned *high_matches, size_t *token_size) {
    size_t start = sizeof(vv_frame_header_t);
    CHECK(frame->cap >= start + 7);
    uint32_t header = vv_read32(frame->data + start);
    CHECK(vv_bh_type(header) == VV_BLOCK_COMPRESSED);
    CHECK(vv_bh_last(header) && vv_bh_size(header) == raw_size);
    const uint8_t *size_bytes = frame->data + start + 4;
    size_t compressed = (size_t)size_bytes[0] |
                        ((size_t)size_bytes[1] << 8) |
                        ((size_t)size_bytes[2] << 16);
    CHECK(compressed <= frame->cap - start - 7);
    vv_frame_header_t fh;
    memcpy(&fh, frame->data, sizeof(fh));
    size_t offset_bytes = fh.window_log > 16 ? 3 : 2;
    const uint8_t *tokens = frame->data + start + 7;
    size_t cursor = 0, produced = 0;
    *high_matches = 0;
    while (cursor < compressed) {
        uint8_t token = tokens[cursor++];
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t extension;
            do {
                CHECK(cursor < compressed);
                extension = tokens[cursor++];
                CHECK(extension <= raw_size - literals);
                literals += extension;
            } while (extension == 255);
        }
        CHECK(literals <= compressed - cursor && literals <= raw_size - produced);
        cursor += literals;
        produced += literals;
        if (cursor == compressed) break;
        CHECK(offset_bytes <= compressed - cursor);
        size_t offset = (size_t)tokens[cursor] | ((size_t)tokens[cursor + 1] << 8);
        if (offset_bytes == 3) offset |= (size_t)tokens[cursor + 2] << 16;
        cursor += offset_bytes;
        CHECK(offset != 0 && offset <= produced);
        if (produced - offset > 32767) (*high_matches)++;
        size_t match = (token & 15u) + VV_MIN_MATCH;
        if ((token & 15u) == 15) {
            uint8_t extension;
            do {
                CHECK(cursor < compressed);
                extension = tokens[cursor++];
                CHECK(extension <= raw_size - match);
                match += extension;
            } while (extension == 255);
        }
        CHECK(match <= raw_size - produced);
        produced += match;
    }
    CHECK(cursor == compressed && produced == raw_size);
    *token_size = compressed;
    return 1;
}

static int high_position_tests(void) {
    guarded_t source, output, reference, decoded, storage;
    size_t frame_cap = vv_compress_bound(PAGE_MAX);
    size_t context_size = vv_fast_context_size(PAGE_MAX);
    CHECK(guarded_alloc(&source, PAGE_MAX, 1));
    CHECK(guarded_alloc(&output, frame_cap, 1));
    CHECK(guarded_alloc(&reference, frame_cap, 1));
    CHECK(guarded_alloc(&decoded, PAGE_MAX, 1));
    CHECK(guarded_alloc(&storage, context_size, vv_fast_context_alignment()));
    /* Unique record headers stop matches from swallowing the page. Common
     * prefixes followed by three different tails create chains whose older
     * candidates can match farther than their newest candidate. */
    make_input(source.data, PAGE_MAX, 0);
    for (size_t record = 0; record < PAGE_MAX / 64; record++) {
        uint8_t *p = source.data + record * 64;
        p[0] = (uint8_t)record;
        p[1] = (uint8_t)(record >> 8);
        for (size_t i = 8; i < 16; i++) p[i] = (uint8_t)(17 + i);
        for (size_t i = 16; i < 64; i++)
            p[i] = (uint8_t)(64 + i + 53 * (record % 3));
    }
    static const uint8_t windows[] = {10, 16, 24};
    static const uint32_t depths[] = {1, 4, 4096};
    static const size_t lengths[] = {
        PAGE_MAX, 4096, 65531, 65532, 65533, 65534, 65535, PAGE_MAX
    };
    for (size_t w = 0; w < sizeof(windows) / sizeof(windows[0]); w++) {
        size_t shallow_size = 0, deep_size = 0;
        for (size_t d = 0; d < sizeof(depths) / sizeof(depths[0]); d++) {
            vv_options_t opts;
            fast_options(&opts, windows[w], 2);
            opts.depth_override = depths[d];
            vv_fast_context_t *ctx = NULL;
            memset(storage.data, CANARY, storage.cap);
            allocation_budget(0);
            int result = vv_fast_context_init(storage.data, storage.cap, PAGE_MAX, &opts, &ctx);
            int no_allocations = allocation_result(0, 0);
            CHECK(no_allocations && result == VV_OK && ctx != NULL);
            for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
                size_t n = lengths[l];
                snprintf(test_case, sizeof(test_case),
                         "high positions wlog=%u depth=%u step=%zu n=%zu",
                         windows[w], depths[d], l, n);
                CHECK(parity(ctx, &opts, source.data, n, &output, &reference, &decoded));
                unsigned high_matches;
                size_t token_size;
                CHECK(high_references(&output, n, &high_matches, &token_size));
                CHECK(n <= 32768 || high_matches > 100);
                if (l == 0 && d == 0) shallow_size = token_size;
                if (l == 0 && d == 1) deep_size = token_size;
                CHECK(guarded_ok(&storage) && guarded_ok(&source));
            }
        }
        /* Ensure the fixture distinguishes deeper chain selection from the
         * newest-root-only search, beyond merely decoding successfully. */
        CHECK(shallow_size != deep_size);
    }
    free(source.allocation);
    free(output.allocation);
    free(reference.allocation);
    free(decoded.allocation);
    free(storage.allocation);
    return 1;
}

static int compression_tests(void) {
    guarded_t source, output, reference, decoded;
    size_t frame_cap = vv_compress_bound(PAGE_MAX);
    CHECK(guarded_alloc(&source, PAGE_MAX + 1, 1));
    CHECK(guarded_alloc(&output, frame_cap, 1));
    CHECK(guarded_alloc(&reference, frame_cap, 1));
    CHECK(guarded_alloc(&decoded, PAGE_MAX, 1));
    size_t alignment = vv_fast_context_alignment();
    static const size_t maxima[] = {1, 4096, 16384, PAGE_MAX};
    static const size_t lengths[] = {0, 1, 3, 15, 255, 4095, 4096, 4097, 16384, PAGE_MAX};
    static const uint8_t windows[] = {0, 10, 16, 17, 24};
    for (size_t m = 0; m < sizeof(maxima) / sizeof(maxima[0]); m++) {
        size_t maximum = maxima[m], size = vv_fast_context_size(maximum);
        guarded_t storage;
        CHECK(guarded_alloc(&storage, size, alignment));
        for (size_t w = 0; w < sizeof(windows) / sizeof(windows[0]); w++) {
            for (unsigned variant = 0; variant < 8; variant++) {
                snprintf(test_case, sizeof(test_case), "init max=%zu wlog=%u opts=%u",
                         maximum, windows[w], variant);
                vv_options_t opts;
                fast_options(&opts, windows[w], variant);
                vv_options_t saved = opts;
                vv_fast_context_t *ctx = NULL;
                allocation_budget(0);
                int result = vv_fast_context_init(storage.data, size, maximum, &opts, &ctx);
                int no_allocations = allocation_result(0, 0);
                CHECK(no_allocations && result == VV_OK && ctx != NULL);
                /* Init borrows no options pointer: later caller mutation is
                 * deliberately invalid and must not change this context. */
                opts.mode = VV_MODE_EXTREME;
                opts.window_log = 9;
                opts.filter_x86 = 1;
                opts.checksum = !saved.checksum;
                CHECK(rejected_compress(ctx, source.data, maximum + 1, &output,
                                          frame_cap, 0, VV_ERR_PARAM));
                CHECK(rejected_compress(NULL, source.data, 1, &output,
                                          frame_cap, 0, VV_ERR_PARAM));
                CHECK(rejected_compress(ctx, NULL, 1, &output,
                                          frame_cap, 0, VV_ERR_PARAM));
                CHECK(rejected_compress(ctx, source.data, 1, &output,
                                          frame_cap, 1, VV_ERR_PARAM));
                for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
                    size_t n = lengths[l];
                    if (n > maximum) continue;
                    for (unsigned kind = 0; kind < 4; kind++) {
                        snprintf(test_case, sizeof(test_case),
                                 "max=%zu wlog=%u opts=%u n=%zu pattern=%u",
                                 maximum, windows[w], variant, n, kind);
                        make_input(source.data, n, kind);
                        size_t bound = vv_compress_bound(n);
                        CHECK(rejected_compress(ctx, source.data, n, &output,
                                                  bound - 1, 0, VV_ERR_OVERFLOW));
                        CHECK(parity(ctx, &saved, n == 0 ? NULL : source.data, n,
                                     &output, &reference, &decoded));
                        if (n) {
                            /* Focused implementation-layout regression: the
                             * token scratch is the final storage section. */
                            size_t scratch = maximum + maximum / 255 + 1024;
                            CHECK(size >= scratch);
                            CHECK(filled(storage.data + size - scratch, scratch, 0));
                        }
                        CHECK(guarded_ok(&storage) && guarded_ok(&source));
                    }
                }
                if (maximum == PAGE_MAX) {
                    /* Explicit long->small transitions without re-init:
                     * sparse-map reset must make old large-input roots and
                     * chain entries unreachable for each new small frame. */
                    static const size_t alternating[] = {
                        PAGE_MAX, 4096, 16384, 1, 4097, 0, 4096
                    };
                    for (size_t i = 0; i < sizeof(alternating) / sizeof(alternating[0]); i++) {
                        size_t n = alternating[i];
                        snprintf(test_case, sizeof(test_case),
                                 "alternating max=%zu wlog=%u opts=%u step=%zu n=%zu",
                                 maximum, windows[w], variant, i, n);
                        make_input(source.data, n, (unsigned)((i + 3) % 4));
                        CHECK(rejected_compress(ctx, source.data, n, &output,
                                  vv_compress_bound(n) - 1, 0, VV_ERR_OVERFLOW));
                        CHECK(parity(ctx, &saved, n == 0 ? NULL : source.data, n,
                                     &output, &reference, &decoded));
                        if (n) {
                            size_t scratch = maximum + maximum / 255 + 1024;
                            CHECK(filled(storage.data + size - scratch, scratch, 0));
                        }
                        CHECK(guarded_ok(&storage) && guarded_ok(&source));
                    }
                }
            }
        }
        free(storage.allocation);
    }
    free(source.allocation);
    free(output.allocation);
    free(reference.allocation);
    free(decoded.allocation);
    CHECK(raw_cases > 0 && token_cases > 0);
    return 1;
}

int main(void) {
    if (!hook_tests() || !init_tests() || !compression_tests() ||
        !high_position_tests()) return 1;
    printf("FAST context tests: %u checks, %u legacy byte-parity cases passed",
           checks, parity_cases);
#ifdef VV_TEST_ALLOC_WRAP
    printf(" (allocation failure injection enabled)");
#endif
    printf("\n");
    return 0;
}
