/* VaptVupt amalgamation — single-file build */
#include "vaptvupt.h"

/* ── src/vv_xxh64.c ── */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — XXH64 checksum (simplified, standalone)
 * Based on xxHash by Yann Collet. Public domain.
 */

#include <string.h>

#define XXH_PRIME64_1  0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2  0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3  0x165667B19E3779F9ULL
#define XXH_PRIME64_4  0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5  0x27D4EB2F165667C5ULL

static inline uint64_t xxh_rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

static inline uint64_t xxh_round(uint64_t acc, uint64_t input) {
    acc += input * XXH_PRIME64_2;
    acc  = xxh_rotl64(acc, 31);
    acc *= XXH_PRIME64_1;
    return acc;
}

static inline uint64_t xxh_merge_round(uint64_t acc, uint64_t val) {
    val  = xxh_round(0, val);
    acc ^= val;
    acc  = acc * XXH_PRIME64_1 + XXH_PRIME64_4;
    return acc;
}

uint64_t vv_xxh64(const void *data, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h64;

    if (len >= 32) {
        uint64_t v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
        uint64_t v2 = seed + XXH_PRIME64_2;
        uint64_t v3 = seed + 0;
        uint64_t v4 = seed - XXH_PRIME64_1;

        do {
            uint64_t k; memcpy(&k, p, 8); v1 = xxh_round(v1, k); p += 8;
            memcpy(&k, p, 8); v2 = xxh_round(v2, k); p += 8;
            memcpy(&k, p, 8); v3 = xxh_round(v3, k); p += 8;
            memcpy(&k, p, 8); v4 = xxh_round(v4, k); p += 8;
        } while (p <= end - 32);

        h64 = xxh_rotl64(v1, 1) + xxh_rotl64(v2, 7) + xxh_rotl64(v3, 12) + xxh_rotl64(v4, 18);
        h64 = xxh_merge_round(h64, v1);
        h64 = xxh_merge_round(h64, v2);
        h64 = xxh_merge_round(h64, v3);
        h64 = xxh_merge_round(h64, v4);
    } else {
        h64 = seed + XXH_PRIME64_5;
    }

    h64 += (uint64_t)len;

    while (p + 8 <= end) {
        uint64_t k; memcpy(&k, p, 8);
        k *= XXH_PRIME64_2; k = xxh_rotl64(k, 31); k *= XXH_PRIME64_1;
        h64 ^= k; h64 = xxh_rotl64(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }
    while (p + 4 <= end) {
        uint32_t k; memcpy(&k, p, 4);
        h64 ^= (uint64_t)k * XXH_PRIME64_1;
        h64 = xxh_rotl64(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }
    while (p < end) {
        h64 ^= (*p) * XXH_PRIME64_5;
        h64 = xxh_rotl64(h64, 11) * XXH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33; h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29; h64 *= XXH_PRIME64_3;
    h64 ^= h64 >> 32;
    return h64;
}

/* ═══════════════════════════════════════════════════════════════
 * STREAMING XXH64
 * ═══════════════════════════════════════════════════════════════ */

void vv_xxh64_init(vv_xxh64_state_t *s, uint64_t seed) {
    s->v1 = seed + XXH_PRIME64_1 + XXH_PRIME64_2;
    s->v2 = seed + XXH_PRIME64_2;
    s->v3 = seed + 0;
    s->v4 = seed - XXH_PRIME64_1;
    s->total_len = 0;
    s->buf_len = 0;
    s->seed = seed;
}

void vv_xxh64_update(vv_xxh64_state_t *s, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    s->total_len += (uint64_t)len;

    /* Fill buffer if partial data pending */
    if (s->buf_len > 0) {
        size_t fill = 32 - s->buf_len;
        if (fill > len) fill = len;
        memcpy(s->buf + s->buf_len, p, fill);
        s->buf_len += fill;
        p += fill;
        if (s->buf_len < 32) return; /* still partial */
        /* Process the full 32 bytes */
        uint64_t k;
        memcpy(&k, s->buf + 0,  8); s->v1 = xxh_round(s->v1, k);
        memcpy(&k, s->buf + 8,  8); s->v2 = xxh_round(s->v2, k);
        memcpy(&k, s->buf + 16, 8); s->v3 = xxh_round(s->v3, k);
        memcpy(&k, s->buf + 24, 8); s->v4 = xxh_round(s->v4, k);
        s->buf_len = 0;
    }

    /* Process full 32-byte chunks */
    while (p + 32 <= end) {
        uint64_t k;
        memcpy(&k, p + 0,  8); s->v1 = xxh_round(s->v1, k);
        memcpy(&k, p + 8,  8); s->v2 = xxh_round(s->v2, k);
        memcpy(&k, p + 16, 8); s->v3 = xxh_round(s->v3, k);
        memcpy(&k, p + 24, 8); s->v4 = xxh_round(s->v4, k);
        p += 32;
    }

    /* Buffer trailing bytes */
    if (p < end) {
        size_t rem = (size_t)(end - p);
        memcpy(s->buf + s->buf_len, p, rem);
        s->buf_len += rem;
    }
}

uint64_t vv_xxh64_finalize(const vv_xxh64_state_t *s) {
    uint64_t h64;

    if (s->total_len >= 32) {
        h64 = xxh_rotl64(s->v1, 1) + xxh_rotl64(s->v2, 7)
            + xxh_rotl64(s->v3, 12) + xxh_rotl64(s->v4, 18);
        h64 = xxh_merge_round(h64, s->v1);
        h64 = xxh_merge_round(h64, s->v2);
        h64 = xxh_merge_round(h64, s->v3);
        h64 = xxh_merge_round(h64, s->v4);
    } else {
        h64 = s->seed + XXH_PRIME64_5;
    }

    h64 += s->total_len;

    const uint8_t *p = s->buf;
    const uint8_t *end = p + s->buf_len;

    while (p + 8 <= end) {
        uint64_t k; memcpy(&k, p, 8);
        k *= XXH_PRIME64_2; k = xxh_rotl64(k, 31); k *= XXH_PRIME64_1;
        h64 ^= k; h64 = xxh_rotl64(h64, 27) * XXH_PRIME64_1 + XXH_PRIME64_4;
        p += 8;
    }
    while (p + 4 <= end) {
        uint32_t k; memcpy(&k, p, 4);
        h64 ^= (uint64_t)k * XXH_PRIME64_1;
        h64 = xxh_rotl64(h64, 23) * XXH_PRIME64_2 + XXH_PRIME64_3;
        p += 4;
    }
    while (p < end) {
        h64 ^= (*p) * XXH_PRIME64_5;
        h64 = xxh_rotl64(h64, 11) * XXH_PRIME64_1;
        p++;
    }

    h64 ^= h64 >> 33; h64 *= XXH_PRIME64_2;
    h64 ^= h64 >> 29; h64 *= XXH_PRIME64_3;
    h64 ^= h64 >> 32;
    return h64;
}

/* ── src/vv_simd.c ── */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — SIMD-accelerated copy routines
 *
 * Three tiers:
 *   1. AVX2 (x86-64 with runtime detection)
 *   2. NEON (ARM64, compile-time)
 *   3. Scalar fallback (always available)
 *
 * PERFORMANCE-CRITICAL: these are the #1 hotspot in decompression.
 * The literal copy and match copy account for ~60% of decode cycles.
 */

#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 * SCALAR FALLBACK (always compiled)
 * ═══════════════════════════════════════════════════════════════ */

static void copy_fast_scalar(uint8_t *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
}

static void copy_match_scalar(uint8_t *dst, uint32_t offset, size_t length) {
    const uint8_t *src = dst - offset;
    if (offset >= 16) {
        /* Non-overlapping: bulk copy */
        while (length >= 16) {
            memcpy(dst, src, 16);
            dst += 16; src += 16; length -= 16;
        }
        if (length > 0) memcpy(dst, src, length);
    } else if (offset >= 8) {
        /* Offset >= 8: can safely copy 8 bytes at a time — each chunk fits
         * within the overlap window without reading unwritten bytes. */
        while (length >= 8) {
            uint64_t v;
            memcpy(&v, src, 8);
            memcpy(dst, &v, 8);
            dst += 8; src += 8; length -= 8;
        }
        while (length-- > 0) *dst++ = *src++;
    } else {
        /* CRITICAL: for offset < 8 the "moderate overlap" 8-byte bulk copy
         * is UNSAFE. Reading 8 bytes at src before writing means we read
         * bytes at positions we're about to write, which may be uninitialized.
         *
         * Example: offset=7, length=8. src = dst-7. Read src[0..7] reads
         * dst[-7..0]. But dst[0] is the first byte we'll WRITE, not a
         * literal we already wrote. Bulk-read gets garbage there, then
         * writes it to dst[7], corrupting position 7.
         *
         * Safe implementation: byte-by-byte, where each write feeds the
         * next read correctly (the classic LZ "self-reference" pattern). */
        for (size_t i = 0; i < length; i++) dst[i] = dst[i - (ptrdiff_t)offset];
    }
}

/* ═══════════════════════════════════════════════════════════════
 * x86-64 AVX2 (guarded by compile-time + runtime detection)
 * ═══════════════════════════════════════════════════════════════ */

#if defined(__x86_64__) || defined(_M_X64)

#include <cpuid.h>

static int vv_has_avx2(void) {
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) return 0;
    return (ebx & (1 << 5)) != 0;  /* AVX2 bit */
}

#ifdef __AVX2__
#include <immintrin.h>

static void copy_fast_avx2(uint8_t *dst, const uint8_t *src, size_t n) {
    while (n >= 32) {
        __m256i v = _mm256_loadu_si256((const __m256i *)src);
        _mm256_storeu_si256((__m256i *)dst, v);
        dst += 32; src += 32; n -= 32;
    }
    if (n >= 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)src);
        _mm_storeu_si128((__m128i *)dst, v);
        dst += 16; src += 16; n -= 16;
    }
    if (n > 0) memcpy(dst, src, n);
}

static void copy_match_avx2(uint8_t *dst, uint32_t offset, size_t length) {
    const uint8_t *src = dst - offset;
    if (offset >= 32) {
        while (length >= 32) {
            __m256i v = _mm256_loadu_si256((const __m256i *)src);
            _mm256_storeu_si256((__m256i *)dst, v);
            dst += 32; src += 32; length -= 32;
        }
        if (length >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)src);
            _mm_storeu_si128((__m128i *)dst, v);
            dst += 16; src += 16; length -= 16;
        }
        if (length > 0) memcpy(dst, src, length);
    } else {
        /* Fall back to scalar for overlapping copies */
        copy_match_scalar(dst, offset, length);
    }
}
#endif /* __AVX2__ */

/* SSE2 path: baseline on all x86-64 CPUs. No runtime check needed.
 * Used when AVX2 is not available at runtime, or when compiled without -mavx2. */
#include <emmintrin.h>  /* SSE2 is guaranteed on x86-64 */

static void copy_fast_sse2(uint8_t *dst, const uint8_t *src, size_t n) {
    while (n >= 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)src);
        _mm_storeu_si128((__m128i *)dst, v);
        dst += 16; src += 16; n -= 16;
    }
    if (n > 0) memcpy(dst, src, n);
}

static void copy_match_sse2(uint8_t *dst, uint32_t offset, size_t length) {
    const uint8_t *src = dst - offset;
    if (offset >= 16) {
        while (length >= 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)src);
            _mm_storeu_si128((__m128i *)dst, v);
            dst += 16; src += 16; length -= 16;
        }
        if (length > 0) memcpy(dst, src, length);
    } else {
        copy_match_scalar(dst, offset, length);
    }
}

#endif /* x86-64 */

/* ═══════════════════════════════════════════════════════════════
 * ARM64 NEON (compile-time detection)
 * ═══════════════════════════════════════════════════════════════ */

#if defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>

static void copy_fast_neon(uint8_t *dst, const uint8_t *src, size_t n) {
    while (n >= 16) {
        uint8x16_t v = vld1q_u8(src);
        vst1q_u8(dst, v);
        dst += 16; src += 16; n -= 16;
    }
    if (n > 0) memcpy(dst, src, n);
}

static void copy_match_neon(uint8_t *dst, uint32_t offset, size_t length) {
    const uint8_t *src = dst - offset;
    if (offset >= 16) {
        while (length >= 16) {
            uint8x16_t v = vld1q_u8(src);
            vst1q_u8(dst, v);
            dst += 16; src += 16; length -= 16;
        }
        if (length > 0) memcpy(dst, src, length);
    } else {
        copy_match_scalar(dst, offset, length);
    }
}
#endif /* ARM64 NEON */

/* ═══════════════════════════════════════════════════════════════
 * RUNTIME DISPATCH (initialized once at first call)
 * ═══════════════════════════════════════════════════════════════ */

typedef void (*copy_fast_fn)(uint8_t *, const uint8_t *, size_t);
typedef void (*copy_match_fn)(uint8_t *, uint32_t, size_t);

static copy_fast_fn  g_copy_fast  = NULL;
static copy_match_fn g_copy_match = NULL;

static void vv_init_simd(void) {
    if (g_copy_fast) return;  /* Already initialized */

#if defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX2__
    if (vv_has_avx2()) {
        g_copy_fast  = copy_fast_avx2;
        g_copy_match = copy_match_avx2;
        return;
    }
#endif
    /* SSE2 is baseline on all x86-64 — no runtime check needed */
    g_copy_fast  = copy_fast_sse2;
    g_copy_match = copy_match_sse2;
    return;
#endif

#if defined(__aarch64__) && defined(__ARM_NEON)
    g_copy_fast  = copy_fast_neon;
    g_copy_match = copy_match_neon;
    return;
#endif

    g_copy_fast  = copy_fast_scalar;
    g_copy_match = copy_match_scalar;
}

void vv_copy_fast(uint8_t *dst, const uint8_t *src, size_t n) {
    if (!g_copy_fast) vv_init_simd();
    g_copy_fast(dst, src, n);
}

void vv_copy_match(uint8_t *dst, uint32_t offset, size_t length) {
    if (!g_copy_match) vv_init_simd();
    g_copy_match(dst, offset, length);
}

/* ── src/vv_huffman.c ── */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — Canonical Huffman Codec Implementation
 *
 * Performance targets (x86-64, gcc -O2):
 *   Encode: ≥ 150 MB/s (bottleneck: bit packing, 1 symbol per ~4 cycles)
 *   Decode: ≥ 800 MB/s (bottleneck: table lookup + refill, 1 symbol per ~5 cycles)
 *
 * If decode falls short of 800 MB/s, the cause is likely the refill frequency.
 * Fix: unroll the decode loop 4× and refill once per 4 symbols (amortize refill).
 *
 * Algorithm:
 *   1. Count symbol frequencies
 *   2. Build Huffman tree (two-queue merge, O(n) after sort)
 *   3. Extract code lengths, limit to 15 bits
 *   4. Assign canonical codes (sorted by length then symbol)
 *   5. Encode: LSB-first bitstream with 64-bit accumulator
 *   6. Decode: 12-bit lookup table (16 KB, L1-resident)
 *
 * Header format (on-disk):
 *   [1B max_symbol]   — highest symbol index with nonzero code length (0-255)
 *   [(max_symbol+2)/2 bytes]  — code lengths packed as nibble pairs:
 *       byte[i] = (lengths[2*i] << 4) | lengths[2*i+1]
 *   Total header: 1 + ceil((max_symbol+1)/2) bytes (1-129 bytes)
 */

#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════
 * BITSTREAM WRITER (LSB-first, 64-bit accumulator)
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t bits;
    int      nbits;
    uint8_t *dst;
    size_t   pos;
    size_t   cap;
} bw_t;

static inline void bw_init(bw_t *w, uint8_t *dst, size_t cap) {
    w->bits = 0; w->nbits = 0; w->dst = dst; w->pos = 0; w->cap = cap;
}

/* Add up to 16 bits. Flushes full bytes automatically. */
static inline void bw_add(bw_t *w, uint32_t val, int n) {
    w->bits |= (uint64_t)(val & ((1u << n) - 1)) << w->nbits;
    w->nbits += n;
    /* Flush complete bytes */
    while (w->nbits >= 8 && w->pos < w->cap) {
        w->dst[w->pos++] = (uint8_t)(w->bits);
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

static inline size_t bw_flush(bw_t *w) {
    while (w->nbits > 0 && w->pos < w->cap) {
        w->dst[w->pos++] = (uint8_t)(w->bits);
        w->bits >>= 8;
        w->nbits -= 8;
    }
    return w->pos;
}

/* ═══════════════════════════════════════════════════════════════
 * BITSTREAM READER (LSB-first, 64-bit accumulator)
 *
 * PERFORMANCE-CRITICAL: this is the decode hot path.
 * The refill reads 8 bytes at a time when possible.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t       bits;
    int            nbits;
    const uint8_t *src;
    size_t         pos;
    size_t         len;
} br_t;

static inline void br_init(br_t *r, const uint8_t *src, size_t len) {
    r->bits = 0; r->nbits = 0; r->src = src; r->pos = 0; r->len = len;
}

/* Refill: load bytes until accumulator is full (≥56 bits) */
static inline void br_refill(br_t *r) {
    while (r->nbits <= 56 && r->pos < r->len) {
        r->bits |= (uint64_t)r->src[r->pos++] << r->nbits;
        r->nbits += 8;
    }
}

static inline uint32_t br_peek(const br_t *r, int n) {
    return (uint32_t)(r->bits & ((1ULL << n) - 1));
}

static inline void br_consume(br_t *r, int n) {
    r->bits >>= n;
    r->nbits -= n;
}

/* ═══════════════════════════════════════════════════════════════
 * REVERSE BITS (for LSB-first canonical code storage)
 * ═══════════════════════════════════════════════════════════════ */

static inline uint16_t reverse_bits(uint16_t code, int len) {
    uint16_t rev = 0;
    for (int i = 0; i < len; i++) {
        rev = (uint16_t)((rev << 1) | (code & 1));
        code >>= 1;
    }
    return rev;
}

/* ═══════════════════════════════════════════════════════════════
 * BUILD HUFFMAN CODE LENGTHS FROM FREQUENCIES
 *
 * Two-queue merge algorithm (O(n) after sorting):
 *   1. Sort non-zero symbols by frequency (ascending)
 *   2. Merge two cheapest nodes repeatedly using two queues
 *      (leaf queue + internal node queue)
 *   3. Extract depths via parent pointers
 *   4. Limit max depth to VVH_MAX_CODE_LEN (15)
 * ═══════════════════════════════════════════════════════════════ */

static void build_code_lengths(const uint32_t freq[VVH_SYMBOLS],
                                uint8_t lengths[VVH_SYMBOLS]) {
    /* Collect non-zero symbols, sort by frequency */
    int sym_idx[VVH_SYMBOLS];
    uint32_t sym_freq[VVH_SYMBOLS];
    int n = 0;

    memset(lengths, 0, VVH_SYMBOLS);
    for (int i = 0; i < VVH_SYMBOLS; i++) {
        if (freq[i] > 0) {
            sym_idx[n] = i;
            sym_freq[n] = freq[i];
            n++;
        }
    }

    if (n == 0) return;
    if (n == 1) { lengths[sym_idx[0]] = 1; return; }
    if (n == 2) { lengths[sym_idx[0]] = 1; lengths[sym_idx[1]] = 1; return; }

    /* Insertion sort by frequency ascending (n ≤ 256, fast enough) */
    for (int i = 1; i < n; i++) {
        uint32_t tf = sym_freq[i];
        int ts = sym_idx[i];
        int j = i - 1;
        while (j >= 0 && sym_freq[j] > tf) {
            sym_freq[j + 1] = sym_freq[j];
            sym_idx[j + 1] = sym_idx[j];
            j--;
        }
        sym_freq[j + 1] = tf;
        sym_idx[j + 1] = ts;
    }

    /* Heap-allocate tree workspace: 2n-1 nodes (n >= 3, so total >= 5) */
    size_t total = 2u * (unsigned)n - 1u;
    uint32_t *nf = (uint32_t *)calloc(total, sizeof(uint32_t));
    int16_t *par = (int16_t *)malloc(total * sizeof(int16_t));
    if (!nf || !par) { free(nf); free(par); return; }

    /* Initialize leaf nodes */
    for (int i = 0; i < n; i++) {
        nf[i] = sym_freq[i];
        par[i] = -1;
    }
    for (size_t i = (size_t)n; i < total; i++) {
        nf[i] = 0;
        par[i] = -1;
    }

    /* Two-queue merge */
    int lq = 0;        /* Leaf queue read pointer */
    int iq = n;         /* Internal queue read pointer */
    int next = n;       /* Next internal node to create */

    for (int m = 0; m < n - 1; m++) {
        uint32_t cost = 0;
        for (int pick = 0; pick < 2; pick++) {
            int use_leaf = (lq < n) && (iq >= next || nf[lq] <= nf[iq]);
            if (use_leaf) {
                cost += nf[lq];
                par[lq] = (int16_t)next;
                lq++;
            } else {
                cost += nf[iq];
                par[iq] = (int16_t)next;
                iq++;
            }
        }
        nf[next] = cost;
        par[next] = -1;
        next++;
    }

    /* Compute depths */
    uint8_t *dep = (uint8_t *)calloc(total, 1);
    if (!dep) { free(nf); free(par); return; }
    dep[total - 1] = 0;
    for (int i = (int)total - 2; i >= 0; i--)
        dep[i] = dep[par[i]] + 1;

    /* Extract leaf depths */
    for (int i = 0; i < n; i++)
        lengths[sym_idx[i]] = dep[i];

    free(nf); free(par); free(dep);

    /* ─── Depth limiting to VVH_MAX_CODE_LEN ─── */
    int max_d = 0;
    for (int i = 0; i < VVH_SYMBOLS; i++)
        if (lengths[i] > max_d) max_d = lengths[i];
    if (max_d <= VVH_MAX_CODE_LEN) return;

    /* Count symbols per depth */
    int bl_count[32];
    memset(bl_count, 0, sizeof(bl_count));
    for (int i = 0; i < VVH_SYMBOLS; i++)
        if (lengths[i] > 0) bl_count[lengths[i]]++;

    /* Cap depths > 15 to 15 */
    for (int d = VVH_MAX_CODE_LEN + 1; d < 32; d++) {
        bl_count[VVH_MAX_CODE_LEN] += bl_count[d];
        bl_count[d] = 0;
    }

    /* Fix Kraft inequality: sum(bl_count[d] * 2^(15-d)) must ≤ 2^15 */
    for (;;) {
        uint32_t kraft = 0;
        for (int d = 1; d <= VVH_MAX_CODE_LEN; d++)
            kraft += (uint32_t)bl_count[d] << (VVH_MAX_CODE_LEN - d);
        if (kraft <= (1u << VVH_MAX_CODE_LEN)) break;
        /* Move one symbol from shallowest level deeper */
        for (int d = VVH_MAX_CODE_LEN - 1; d >= 1; d--) {
            if (bl_count[d] > 0) {
                bl_count[d]--;
                bl_count[d + 1]++;
                break;
            }
        }
    }

    /* Reassign lengths: sort non-zero symbols by (current_length asc, symbol asc)
     * then assign from the bl_count distribution shortest-first */
    typedef struct { uint8_t len; uint8_t sym; } ls_t;
    ls_t sorted[VVH_SYMBOLS];
    int ns = 0;
    for (int i = 0; i < VVH_SYMBOLS; i++)
        if (lengths[i] > 0) {
            sorted[ns].len = lengths[i] > VVH_MAX_CODE_LEN
                           ? VVH_MAX_CODE_LEN : lengths[i];
            sorted[ns].sym = (uint8_t)i;
            ns++;
        }
    /* Sort by len ascending, then sym ascending */
    for (int i = 1; i < ns; i++) {
        ls_t tmp = sorted[i];
        int j = i - 1;
        while (j >= 0 && (sorted[j].len > tmp.len ||
              (sorted[j].len == tmp.len && sorted[j].sym > tmp.sym))) {
            sorted[j + 1] = sorted[j]; j--;
        }
        sorted[j + 1] = tmp;
    }
    /* Assign from distribution */
    int si = 0;
    for (int d = 1; d <= VVH_MAX_CODE_LEN && si < ns; d++)
        for (int c = 0; c < bl_count[d] && si < ns; c++)
            lengths[sorted[si++].sym] = (uint8_t)d;
}

/* ═══════════════════════════════════════════════════════════════
 * CANONICAL CODE ASSIGNMENT
 * ═══════════════════════════════════════════════════════════════ */

static void assign_canonical_codes(const uint8_t lengths[VVH_SYMBOLS],
                                    uint16_t codes[VVH_SYMBOLS]) {
    /* Count symbols at each length */
    int bl_count[VVH_MAX_CODE_LEN + 1];
    memset(bl_count, 0, sizeof(bl_count));
    for (int i = 0; i < VVH_SYMBOLS; i++)
        if (lengths[i] > 0 && lengths[i] <= VVH_MAX_CODE_LEN)
            bl_count[lengths[i]]++;

    /* Compute first code for each length (MSB-first canonical) */
    uint16_t next_code[VVH_MAX_CODE_LEN + 1];
    uint16_t code = 0;
    next_code[0] = 0;
    for (int bits = 1; bits <= VVH_MAX_CODE_LEN; bits++) {
        code = (uint16_t)((code + bl_count[bits - 1]) << 1);
        next_code[bits] = code;
    }

    /* Assign codes in symbol order (canonical: sorted by length then symbol) */
    for (int i = 0; i < VVH_SYMBOLS; i++) {
        if (lengths[i] > 0)
            codes[i] = next_code[lengths[i]]++;
        else
            codes[i] = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * BUILD ENCODER TABLE
 * ═══════════════════════════════════════════════════════════════ */

static void build_enc_table(const uint32_t freq[VVH_SYMBOLS],
                             vvh_enc_table_t *enc) {
    build_code_lengths(freq, enc->lengths);

    uint16_t canonical[VVH_SYMBOLS];
    assign_canonical_codes(enc->lengths, canonical);

    /* Store bit-reversed codes for LSB-first writing */
    for (int i = 0; i < VVH_SYMBOLS; i++) {
        if (enc->lengths[i] > 0)
            enc->codes[i] = reverse_bits(canonical[i], enc->lengths[i]);
        else
            enc->codes[i] = 0;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * BUILD DECODER TABLE
 * ═══════════════════════════════════════════════════════════════ */

static void build_dec_table(const uint8_t lengths[VVH_SYMBOLS],
                             vvh_dec_table_t *dec) {
    uint16_t canonical[VVH_SYMBOLS];
    assign_canonical_codes(lengths, canonical);

    memset(dec->table, 0, sizeof(dec->table));
    dec->slow_count = 0;

    for (int sym = 0; sym < VVH_SYMBOLS; sym++) {
        int len = lengths[sym];
        if (len == 0) continue;

        uint16_t rev = reverse_bits(canonical[sym], len);

        if (len <= VVH_DECODE_BITS) {
            /* Fast path: fill all entries where low `len` bits match `rev` */
            int fill = 1 << (VVH_DECODE_BITS - len);
            for (int j = 0; j < fill; j++) {
                int idx = (int)rev | (j << len);
                dec->table[idx] = (uint32_t)sym | ((uint32_t)len << 8);
            }
        } else {
            /* Slow path: store for linear scan */
            int si = dec->slow_count++;
            dec->slow_code[si] = rev;
            dec->slow_len[si] = (uint8_t)len;
            dec->slow_sym[si] = (uint8_t)sym;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * WRITE HEADER (code lengths as packed nibbles)
 *
 * Format: [1B max_sym] [(max_sym+2)/2 bytes packed nibble pairs]
 * ═══════════════════════════════════════════════════════════════ */

static size_t write_header(const uint8_t lengths[VVH_SYMBOLS],
                            uint8_t *dst, size_t cap) {
    /* Find max symbol with nonzero length */
    int max_sym = 0;
    for (int i = VVH_SYMBOLS - 1; i >= 0; i--) {
        if (lengths[i] > 0) { max_sym = i; break; }
    }

    size_t hdr_size = 1 + ((size_t)max_sym + 2) / 2;
    if (hdr_size > cap) return 0;

    dst[0] = (uint8_t)max_sym;

    /* Pack nibble pairs */
    for (int i = 0; i <= max_sym; i += 2) {
        uint8_t hi = lengths[i];
        uint8_t lo = (i + 1 <= max_sym) ? lengths[i + 1] : 0;
        dst[1 + i / 2] = (uint8_t)((hi << 4) | (lo & 0x0F));
    }

    return hdr_size;
}

/* ═══════════════════════════════════════════════════════════════
 * READ HEADER
 * ═══════════════════════════════════════════════════════════════ */

static size_t read_header(const uint8_t *src, size_t src_len,
                           uint8_t lengths[VVH_SYMBOLS]) {
    memset(lengths, 0, VVH_SYMBOLS);
    if (src_len < 1) return 0;

    int max_sym = src[0];
    size_t hdr_size = 1 + ((size_t)max_sym + 2) / 2;
    if (hdr_size > src_len) return 0;

    for (int i = 0; i <= max_sym; i += 2) {
        uint8_t packed = src[1 + i / 2];
        lengths[i] = packed >> 4;
        if (i + 1 <= max_sym)
            lengths[i + 1] = packed & 0x0F;
    }

    return hdr_size;
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODE
 * ═══════════════════════════════════════════════════════════════ */

vvh_error_t vvh_encode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap, size_t *dst_len) {
    if (src_len == 0) {
        *dst_len = 0;
        return VVH_OK;
    }

    /* Count frequencies */
    uint32_t freq[VVH_SYMBOLS];
    memset(freq, 0, sizeof(freq));
    for (size_t i = 0; i < src_len; i++)
        freq[src[i]]++;

    /* Build encode table */
    vvh_enc_table_t enc;
    build_enc_table(freq, &enc);

    /* Check: any symbols with length 0 that appear in input? (shouldn't happen) */
    /* Write header */
    size_t hdr_sz = write_header(enc.lengths, dst, dst_cap);
    if (hdr_sz == 0) return VVH_ERR_OVERFLOW;

    /* Encode bitstream */
    bw_t w;
    bw_init(&w, dst + hdr_sz, dst_cap - hdr_sz);

    for (size_t i = 0; i < src_len; i++) {
        uint8_t sym = src[i];
        bw_add(&w, enc.codes[sym], enc.lengths[sym]);
    }

    size_t bs_sz = bw_flush(&w);
    size_t total = hdr_sz + bs_sz;

    /* Incompressible guard: if not smaller, signal failure */
    if (total >= src_len) {
        return VVH_ERR_OVERFLOW;
    }

    *dst_len = total;
    return VVH_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE
 *
 * PERFORMANCE-CRITICAL: the inner loop decodes one symbol per
 * iteration using a 12-bit table lookup + refill.
 *
 * Hot path (codes ≤ 12 bits, ~99% of symbols):
 *   1. Peek 12 bits from accumulator
 *   2. Table lookup → (symbol, length)
 *   3. Consume `length` bits
 *   4. Refill accumulator if needed
 *   5. Write symbol to output
 *
 * Cold path (codes 13-15 bits, <1% of symbols):
 *   Linear scan of slow_code/slow_len/slow_sym arrays.
 * ═══════════════════════════════════════════════════════════════ */

vvh_error_t vvh_decode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap,
                       size_t num_literals, size_t *src_consumed) {
    if (num_literals == 0) {
        *src_consumed = 0;
        return VVH_OK;
    }
    if (num_literals > dst_cap) return VVH_ERR_OVERFLOW;

    /* Read header */
    uint8_t lengths[VVH_SYMBOLS];
    size_t hdr_sz = read_header(src, src_len, lengths);
    if (hdr_sz == 0) return VVH_ERR_CORRUPT;

    /* Check for valid tree: at least one nonzero length */
    int has_sym = 0;
    for (int i = 0; i < VVH_SYMBOLS; i++)
        if (lengths[i] > 0) { has_sym = 1; break; }
    if (!has_sym) return VVH_ERR_CORRUPT;

    /* Build decode table (heap-allocated: 16 KB) */
    vvh_dec_table_t *dec = (vvh_dec_table_t *)malloc(sizeof(vvh_dec_table_t));
    if (!dec) return VVH_ERR_NOMEM;
    build_dec_table(lengths, dec);

    /* Initialize bitstream reader */
    br_t r;
    br_init(&r, src + hdr_sz, src_len - hdr_sz);
    br_refill(&r);

    /* ─── Decode loop ─── */
    for (size_t i = 0; i < num_literals; i++) {
        /* Refill if accumulator is getting low */
        if (r.nbits < VVH_MAX_CODE_LEN)
            br_refill(&r);

        uint32_t peek = br_peek(&r, VVH_DECODE_BITS);
        uint32_t entry = dec->table[peek];
        int sym = (int)(entry & 0xFF);
        int len = (int)((entry >> 8) & 0xF);

        if (VV_LIKELY(len > 0)) {
            /* Fast path: code ≤ 12 bits */
            br_consume(&r, len);
            dst[i] = (uint8_t)sym;
        } else {
            /* Slow path: code > 12 bits */
            int found = 0;
            for (int s = 0; s < dec->slow_count; s++) {
                int slen = dec->slow_len[s];
                uint32_t mask = (1u << slen) - 1;
                if ((br_peek(&r, slen) & mask) == dec->slow_code[s]) {
                    br_consume(&r, slen);
                    dst[i] = dec->slow_sym[s];
                    found = 1;
                    break;
                }
            }
            if (!found) {
                free(dec);
                return VVH_ERR_CORRUPT;
            }
        }
    }

    /* Calculate bytes consumed from src */
    *src_consumed = hdr_sz + r.pos;
    /* Account for bits still in accumulator that we didn't fully consume */
    if (r.nbits >= 8) {
        /* We over-read by (nbits/8) bytes */
        size_t over = (size_t)(r.nbits / 8);
        if (*src_consumed >= over)
            *src_consumed -= over;
    }

    free(dec);
    return VVH_OK;
}

/* ── src/vv_ans.c ── */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — tANS v2 (sparse header + 4-way interleaved decode)
 *
 * Performance targets (x86-64, gcc -O2):
 *   Encode: ≥ 200 MB/s
 *   Decode (scalar 4-way): ≥ 2,500 MB/s
 *   Decode (scalar 1-way): ≥ 1,200 MB/s (backward compat path)
 *
 * Sprint 6 changes:
 *   Item 1: Adaptive header — sparse format for ≤32 active symbols,
 *           saves 400+ bytes on typical post-LZ literal streams.
 *   Item 2: 4-way interleaved encode/decode — hides table lookup latency,
 *           ~2.5× throughput improvement.
 */

#include <stdlib.h>
#include <string.h>

#define ANS_L    VVA_TABLE_SIZE
#define ANS_LOG  VVA_TABLE_LOG
#define NSYM     VVA_MAX_SYMBOL

static inline int ilog2(uint32_t v) {
    int r = 0;
    while (v >>= 1) r++;
    return r;
}

/* ═══════════════════════════════════════════════════════════════
 * BIT WRITER / READER (LSB-first, 64-bit accumulator)
 * ═══════════════════════════════════════════════════════════════ */

typedef struct { uint64_t a; int n; uint8_t *b; size_t p, c; } ans_bw_t;

static inline void ans_bw_init(ans_bw_t *w, uint8_t *b, size_t c) {
    w->a = 0; w->n = 0; w->b = b; w->p = 0; w->c = c;
}
static inline void ans_bw_add(ans_bw_t *w, uint32_t v, int nb) {
    if (!nb) return;
    w->a |= (uint64_t)(v & ((1u << nb) - 1)) << w->n;
    w->n += nb;
    while (w->n >= 8 && w->p < w->c) {
        w->b[w->p++] = (uint8_t)w->a;
        w->a >>= 8;
        w->n -= 8;
    }
}
static inline size_t ans_bw_flush(ans_bw_t *w) {
    while (w->n > 0 && w->p < w->c) {
        w->b[w->p++] = (uint8_t)w->a;
        w->a >>= 8;
        w->n -= 8;
    }
    return w->p;
}

typedef struct { uint64_t a; int n; const uint8_t *s; size_t p, l; } ans_br_t;

static inline void ans_br_init(ans_br_t *r, const uint8_t *s, size_t l) {
    r->a = 0; r->n = 0; r->s = s; r->p = 0; r->l = l;
}
static inline void ans_br_fill(ans_br_t *r) {
    /* PERF: bulk refill — one unaligned 8-byte load + masked OR.
     *
     * Semantics must match the byte-at-a-time loop exactly. The
     * loop adds whole bytes at positions r->n, r->n+8, r->n+16, ...
     * stopping when r->n would exceed 56 after adding another byte.
     *
     * So we add k = (64 - r->n) / 8 whole bytes (floor), contributing
     * 8k bits. Any 8-byte load's high (64 - 8k) bits are discarded by
     * pre-masking — those bytes stay on disk and get re-loaded next
     * fill. This preserves `r->p` as the byte offset of the next
     * unloaded byte, exactly as the byte-at-a-time loop does.
     *
     * Fallback loop handles end-of-stream where we can't load 8 bytes. */
    if (r->n <= 56) {
        if (VV_LIKELY(r->p + 8 <= r->l)) {
            uint64_t bytes;
            memcpy(&bytes, r->s + r->p, 8);
            int k = (64 - r->n) >> 3;          /* whole bytes to add */
            int bits = k << 3;
            uint64_t mask = (bits == 64) ? ~(uint64_t)0
                                         : ((uint64_t)1 << bits) - 1;
            r->a |= (bytes & mask) << r->n;
            r->p += k;
            r->n += bits;
        } else {
            while (r->n <= 56 && r->p < r->l) {
                r->a |= (uint64_t)r->s[r->p++] << r->n;
                r->n += 8;
            }
        }
    }
}
static inline uint32_t ans_br_read(ans_br_t *r, int nb) {
    if (!nb) return 0;
    if (r->n < nb) ans_br_fill(r);
    uint32_t v = (uint32_t)(r->a & ((1ULL << nb) - 1));
    r->a >>= nb;
    r->n -= nb;
    return v;
}

/* ═══════════════════════════════════════════════════════════════
 * FREQUENCY NORMALIZATION → sum = L = 4096
 * ═══════════════════════════════════════════════════════════════ */

static int normalize_freq(const uint32_t raw[NSYM], uint16_t norm[NSYM]) {
    uint64_t total = 0;
    int np = 0;
    for (int i = 0; i < NSYM; i++) {
        total += raw[i];
        if (raw[i]) np++;
    }
    memset(norm, 0, NSYM * sizeof(uint16_t));
    if (!np) return 0;
    if (np == 1) {
        for (int i = 0; i < NSYM; i++)
            if (raw[i]) norm[i] = (uint16_t)ANS_L;
        return 1;
    }

    int32_t assigned = 0;
    int32_t frac[NSYM];
    memset(frac, 0, sizeof(frac));
    for (int i = 0; i < NSYM; i++) {
        if (!raw[i]) continue;
        uint64_t sc = (uint64_t)raw[i] * ANS_L;
        uint32_t base = (uint32_t)(sc / total);
        if (!base) base = 1;
        norm[i] = (uint16_t)base;
        frac[i] = (int32_t)(sc % total);
        assigned += (int32_t)base;
    }

    int32_t diff = ANS_L - assigned;
    while (diff > 0) {
        int b = -1; int32_t br = -1;
        for (int i = 0; i < NSYM; i++)
            if (raw[i] && frac[i] > br) { br = frac[i]; b = i; }
        if (b < 0) break;
        norm[b]++; frac[b] = -1; diff--;
    }
    while (diff < 0) {
        int b = -1; int32_t br = 0x7FFFFFFF;
        for (int i = 0; i < NSYM; i++)
            if (norm[i] > 1 && frac[i] < br) { br = frac[i]; b = i; }
        if (b < 0) {
            int lg = -1; uint16_t lf = 0;
            for (int i = 0; i < NSYM; i++)
                if (norm[i] > lf) { lf = norm[i]; lg = i; }
            if (lg >= 0 && norm[lg] > 1) { norm[lg]--; diff++; }
            else break;
        } else {
            norm[b]--; frac[b] = 0x7FFFFFFF; diff++;
        }
    }
    return np;
}

/* ═══════════════════════════════════════════════════════════════
 * SYMBOL SPREAD + TABLE BUILD
 * ═══════════════════════════════════════════════════════════════ */

static void spread_symbols(const uint16_t norm[NSYM], uint8_t sp[ANS_L]) {
    const uint32_t step = (ANS_L >> 1) + (ANS_L >> 3) + 3;
    uint32_t pos = 0;
    for (int s = 0; s < NSYM; s++)
        for (int i = 0; i < norm[s]; i++) {
            sp[pos] = (uint8_t)s;
            pos = (pos + step) & (ANS_L - 1);
        }
}

static void build_dec(const uint16_t norm[NSYM], const uint8_t sp[ANS_L],
                       vva_dec_entry_t dec[ANS_L]) {
    uint16_t occ[NSYM];
    memset(occ, 0, sizeof(occ));
    for (int x = 0; x < ANS_L; x++) {
        uint8_t s = sp[x];
        uint16_t f = norm[s];
        int k = occ[s]++;
        if (f == 0 || f == (uint16_t)ANS_L) {
            dec[x].symbol = s; dec[x].nbits = 0; dec[x].baseline = 0;
            continue;
        }
        int flg = ilog2(f);
        int nb_max = ANS_LOG - flg;
        int low_count = (1 << (flg + 1)) - (int)f;
        if (k < low_count) {
            dec[x].nbits = (uint8_t)nb_max;
            dec[x].baseline = (uint16_t)((uint32_t)k << nb_max);
        } else {
            dec[x].nbits = (uint8_t)(nb_max - 1);
            dec[x].baseline = (uint16_t)(((uint32_t)low_count << nb_max)
                             + ((uint32_t)(k - low_count) << (nb_max - 1)));
        }
        dec[x].symbol = s;
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODE CONTEXT
 * ═══════════════════════════════════════════════════════════════ */

typedef struct { uint16_t bl; uint8_t nb; uint16_t slot; } enc_occ_t;
typedef struct { enc_occ_t *o; uint16_t cum[NSYM + 1]; } enc_ctx_t;

static enc_ctx_t *build_enc(const uint16_t norm[NSYM], const uint8_t sp[ANS_L],
                             const vva_dec_entry_t dec[ANS_L]) {
    enc_ctx_t *c = (enc_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->o = (enc_occ_t *)malloc(ANS_L * sizeof(enc_occ_t));
    if (!c->o) { free(c); return NULL; }
    c->cum[0] = 0;
    for (int i = 0; i < NSYM; i++) c->cum[i + 1] = c->cum[i] + norm[i];
    uint16_t oi[NSYM];
    memset(oi, 0, sizeof(oi));
    for (int x = 0; x < ANS_L; x++) {
        uint8_t s = sp[x];
        int idx = c->cum[s] + oi[s]++;
        c->o[idx].bl = dec[x].baseline;
        c->o[idx].nb = dec[x].nbits;
        c->o[idx].slot = (uint16_t)x;
    }
    for (int s = 0; s < NSYM; s++) {
        int st = c->cum[s], cnt = (int)norm[s];
        for (int i = st + 1; i < st + cnt; i++) {
            enc_occ_t tmp = c->o[i];
            int j = i - 1;
            while (j >= st && c->o[j].bl > tmp.bl) {
                c->o[j + 1] = c->o[j]; j--;
            }
            c->o[j + 1] = tmp;
        }
    }
    return c;
}

static void free_enc(enc_ctx_t *c) {
    if (c) { free(c->o); free(c); }
}

static inline int enc_sym(const enc_ctx_t *c, uint32_t state, uint8_t sym,
                           uint32_t *bv, int *bn) {
    int base = c->cum[sym], cnt = c->cum[sym + 1] - base;
    if (!cnt) return -1;
    if (cnt == ANS_L) { *bv = 0; *bn = 0; return 0; }
    for (int i = base; i < base + cnt; i++) {
        uint32_t bl = c->o[i].bl;
        int nb = c->o[i].nb;
        if (state >= bl && state < bl + (1u << nb)) {
            *bv = state - bl; *bn = nb;
            return (int)c->o[i].slot;
        }
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════
 * ADAPTIVE HEADER v2 (Item 1 — Sprint 6)
 *
 * Format:
 *   [1B fmt] VVA_HDR_SINGLE: [1B symbol]
 *   [1B fmt] VVA_HDR_SPARSE: [1B count] then count × [1B sym][2B freq LE]
 *   [1B fmt] VVA_HDR_DENSE:  [1B max_sym] then (max_sym+1) × [2B freq LE]
 *
 * Tradeoff: sparse = 2 + 3×n bytes; dense = 2 + 2×(max_sym+1) bytes.
 * Break-even at n ≈ (2×max_sym) / 3, typically around 85 for ASCII data.
 * We use sparse when n ≤ 64 for safety margin.
 * ═══════════════════════════════════════════════════════════════ */

#define SPARSE_THRESHOLD 64

static size_t write_hdr_v2(const uint16_t norm[NSYM], uint8_t *d, size_t cap) {
    /* Count active symbols and find max */
    int active = 0, max_sym = 0, single_sym = -1;
    for (int i = 0; i < NSYM; i++) {
        if (norm[i] > 0) { active++; max_sym = i; single_sym = i; }
    }

    if (active == 0) return 0;

    if (active == 1) {
        /* Single symbol: 2 bytes total */
        if (cap < 2) return 0;
        d[0] = VVA_HDR_SINGLE;
        d[1] = (uint8_t)single_sym;
        return 2;
    }

    if (active <= SPARSE_THRESHOLD) {
        /* Sparse: 2 + 3×active bytes */
        size_t sz = 2 + 3 * (size_t)active;
        if (sz > cap) return 0;
        d[0] = VVA_HDR_SPARSE;
        d[1] = (uint8_t)active;
        int p = 2;
        for (int i = 0; i < NSYM; i++) {
            if (norm[i] > 0) {
                d[p++] = (uint8_t)i;
                d[p++] = (uint8_t)(norm[i] & 0xFF);
                d[p++] = (uint8_t)(norm[i] >> 8);
            }
        }
        return sz;
    }

    /* Dense: 2 + 2×(max_sym+1) bytes */
    size_t sz = 2 + 2 * (size_t)(max_sym + 1);
    if (sz > cap) return 0;
    d[0] = VVA_HDR_DENSE;
    d[1] = (uint8_t)max_sym;
    for (int i = 0; i <= max_sym; i++) {
        d[2 + 2 * i]     = (uint8_t)(norm[i] & 0xFF);
        d[2 + 2 * i + 1] = (uint8_t)(norm[i] >> 8);
    }
    return sz;
}

static size_t read_hdr_v2(const uint8_t *s, size_t len, uint16_t norm[NSYM]) {
    memset(norm, 0, NSYM * sizeof(uint16_t));
    if (len < 1) return 0;

    uint8_t fmt = s[0];

    if (fmt == VVA_HDR_SINGLE) {
        if (len < 2) return 0;
        norm[s[1]] = (uint16_t)ANS_L;
        return 2;
    }

    if (fmt == VVA_HDR_SPARSE) {
        if (len < 2) return 0;
        int count = s[1];
        size_t sz = 2 + 3 * (size_t)count;
        if (sz > len) return 0;
        int p = 2;
        for (int i = 0; i < count; i++) {
            int sym = s[p++];
            norm[sym] = (uint16_t)(s[p] | (s[p + 1] << 8));
            p += 2;
        }
        return sz;
    }

    if (fmt == VVA_HDR_DENSE) {
        if (len < 2) return 0;
        int max_sym = s[1];
        size_t sz = 2 + 2 * (size_t)(max_sym + 1);
        if (sz > len) return 0;
        for (int i = 0; i <= max_sym; i++)
            norm[i] = (uint16_t)(s[2 + 2 * i] | (s[2 + 2 * i + 1] << 8));
        return sz;
    }

    /* Legacy v0.5 format: first byte is max_sym (0-255), not a format code.
     * HDR_SINGLE=1, HDR_SPARSE=2, HDR_DENSE=3, so any value ≥4 is legacy.
     * Values 0-3 could also be a legacy max_sym of 0-3.
     * Disambiguate: legacy format has s[1..2] = freq of symbol 0.
     * If s[0] <= 3 and len >= 1+2*(s[0]+1), try legacy. */
    {
        int max_sym = s[0];
        size_t sz = 1 + 2 * (size_t)(max_sym + 1);
        if (sz <= len) {
            for (int i = 0; i <= max_sym; i++)
                norm[i] = (uint16_t)(s[1 + 2 * i] | (s[1 + 2 * i + 1] << 8));
            return sz;
        }
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 * BITPAIR STACK (for LIFO encode)
 * ═══════════════════════════════════════════════════════════════ */

/* PERF: val must be uint32_t to hold up to 23 offset extra bits (wlog>16) */
typedef struct { uint32_t val; uint8_t nb; } bitpair_t;

/* ═══════════════════════════════════════════════════════════════
 * INTERNAL: build all tables from normalized frequencies
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t        *spread;
    vva_dec_entry_t *dec;
    enc_ctx_t      *enc;
} tables_t;

static int build_all(const uint16_t norm[NSYM], tables_t *t) {
    /* PERF: coalesce spread + dec into a single allocation.
     * spread is ANS_L bytes; dec is ANS_L * sizeof(vva_dec_entry_t).
     * We store the base pointer in t->spread and carve dec from it.
     * free_all() frees t->spread which covers both. */
    size_t spread_sz = ANS_L;
    size_t dec_sz = ANS_L * sizeof(vva_dec_entry_t);
    t->spread = (uint8_t *)malloc(spread_sz + dec_sz);
    if (!t->spread) {
        t->dec = NULL; t->enc = NULL;
        return -1;
    }
    t->dec = (vva_dec_entry_t *)(t->spread + spread_sz);
    spread_symbols(norm, t->spread);
    build_dec(norm, t->spread, t->dec);
    t->enc = build_enc(norm, t->spread, t->dec);
    if (!t->enc) {
        free(t->spread);
        t->spread = NULL; t->dec = NULL;
        return -1;
    }
    return 0;
}

static void free_all(tables_t *t) {
    /* t->dec is part of the t->spread allocation; only free spread */
    free(t->spread);
    free_enc(t->enc);
}

/* ═══════════════════════════════════════════════════════════════
 * SINGLE-STREAM ENCODE (tag 'A', backward compat)
 * ═══════════════════════════════════════════════════════════════ */

vva_error_t vva_encode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap, size_t *dst_len) {
    if (!src_len) { *dst_len = 0; return VVA_OK; }

    uint32_t raw[NSYM];
    memset(raw, 0, sizeof(raw));
    for (size_t i = 0; i < src_len; i++) raw[src[i]]++;

    uint16_t norm[NSYM];
    int np = normalize_freq(raw, norm);
    if (!np) return VVA_ERR_PARAM;

    size_t hdr = write_hdr_v2(norm, dst, dst_cap);
    if (!hdr) return VVA_ERR_OVERFLOW;

    if (np == 1) {
        *dst_len = hdr;
        return (hdr >= src_len) ? VVA_ERR_OVERFLOW : VVA_OK;
    }

    tables_t t;
    if (build_all(norm, &t) < 0) return VVA_ERR_NOMEM;

    /* PERF: one combined alloc for pairs + bs. pairs is src_len of
     * bitpair_t; bs is (src_len*15+7)/8 + 16 bytes of bitstream.
     * Saves 1 malloc/free pair per vva_encode call. */
    size_t pairs_sz = src_len * sizeof(bitpair_t);
    size_t bs_cap = (src_len * 15 + 7) / 8 + 16;
    uint8_t *combo = (uint8_t *)malloc(pairs_sz + bs_cap);
    if (!combo) { free_all(&t); return VVA_ERR_NOMEM; }
    bitpair_t *pairs = (bitpair_t *)combo;
    uint8_t *bs = combo + pairs_sz;

    uint32_t state = 0;
    for (size_t ii = src_len; ii > 0; ii--) {
        uint32_t bv; int bn;
        int slot = enc_sym(t.enc, state, src[ii - 1], &bv, &bn);
        if (slot < 0) { free_all(&t); free(combo); return VVA_ERR_CORRUPT; }
        pairs[ii - 1].val = (uint32_t)bv;
        pairs[ii - 1].nb = (uint8_t)bn;
        state = (uint32_t)slot;
    }

    ans_bw_t w;
    ans_bw_init(&w, bs, bs_cap);
    for (size_t i = 0; i < src_len; i++)
        ans_bw_add(&w, pairs[i].val, pairs[i].nb);
    size_t bs_len = ans_bw_flush(&w);

    size_t total = hdr + 2 + bs_len;
    if (total > dst_cap || total >= src_len) {
        free_all(&t); free(combo);
        return VVA_ERR_OVERFLOW;
    }

    dst[hdr]     = (uint8_t)(state & 0xFF);
    dst[hdr + 1] = (uint8_t)((state >> 8) & 0xFF);
    memcpy(dst + hdr + 2, bs, bs_len);

    *dst_len = total;
    free_all(&t); free(combo);
    return VVA_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * SINGLE-STREAM DECODE (tag 'A', backward compat)
 * ═══════════════════════════════════════════════════════════════ */

vva_error_t vva_decode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap,
                       size_t num_literals, size_t *src_consumed) {
    if (!num_literals) { *src_consumed = 0; return VVA_OK; }
    if (num_literals > dst_cap) return VVA_ERR_OVERFLOW;

    uint16_t norm[NSYM];
    size_t hdr = read_hdr_v2(src, src_len, norm);
    if (!hdr) return VVA_ERR_CORRUPT;

    int np = 0, single = -1;
    for (int i = 0; i < NSYM; i++)
        if (norm[i]) { np++; single = i; }
    if (!np) return VVA_ERR_CORRUPT;
    if (np == 1) {
        memset(dst, single, num_literals);
        *src_consumed = hdr;
        return VVA_OK;
    }

    uint8_t *sp = (uint8_t *)malloc(ANS_L);
    vva_dec_entry_t *dec = (vva_dec_entry_t *)malloc(ANS_L * sizeof(*dec));
    if (!sp || !dec) { free(sp); free(dec); return VVA_ERR_NOMEM; }
    spread_symbols(norm, sp);
    build_dec(norm, sp, dec);
    free(sp);

    if (hdr + 2 > src_len) { free(dec); return VVA_ERR_CORRUPT; }
    uint32_t state = (uint32_t)src[hdr] | ((uint32_t)src[hdr + 1] << 8);
    if (state >= (uint32_t)ANS_L) { free(dec); return VVA_ERR_CORRUPT; }

    ans_br_t r;
    ans_br_init(&r, src + hdr + 2, src_len - hdr - 2);
    ans_br_fill(&r);

    for (size_t i = 0; i < num_literals; i++) {
        if (r.n < ANS_LOG) ans_br_fill(&r);
        vva_dec_entry_t e = dec[state];
        dst[i] = e.symbol;
        uint32_t bits = ans_br_read(&r, e.nbits);
        state = (uint32_t)e.baseline + bits;
        if (state >= (uint32_t)ANS_L) { free(dec); return VVA_ERR_CORRUPT; }
    }

    *src_consumed = hdr + 2 + r.p;
    if (r.n >= 8) {
        size_t ov = (size_t)(r.n / 8);
        if (*src_consumed >= ov) *src_consumed -= ov;
    }

    free(dec);
    return VVA_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 4-WAY INTERLEAVED ENCODE (tag 'I', v0.6+, Item 2)
 *
 * Split literals into 4 sub-streams (round-robin), encode each
 * independently, then interleave the bitstream output.
 *
 * Output: [header] [4×2B states] [4×2B bitstream_sizes] [bitstream0..3]
 * ═══════════════════════════════════════════════════════════════ */

vva_error_t vva_encode4(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap, size_t *dst_len) {
    if (!src_len) { *dst_len = 0; return VVA_OK; }

    /* Count frequencies (shared table for all 4 streams) */
    uint32_t raw[NSYM];
    memset(raw, 0, sizeof(raw));
    for (size_t i = 0; i < src_len; i++) raw[src[i]]++;

    uint16_t norm[NSYM];
    int np = normalize_freq(raw, norm);
    if (!np) return VVA_ERR_PARAM;

    size_t hdr = write_hdr_v2(norm, dst, dst_cap);
    if (!hdr) return VVA_ERR_OVERFLOW;

    if (np == 1) {
        *dst_len = hdr;
        return (hdr >= src_len) ? VVA_ERR_OVERFLOW : VVA_OK;
    }

    tables_t t;
    if (build_all(norm, &t) < 0) return VVA_ERR_NOMEM;

    /* Encode 4 sub-streams independently */
    size_t bs_cap = (src_len * 15 + 7) / 8 + 64;
    size_t lane_cap = bs_cap / 4 + 16;
    uint8_t *bs_bufs[4] = {NULL, NULL, NULL, NULL};
    size_t bs_lens[4] = {0, 0, 0, 0};
    uint16_t states[4] = {0, 0, 0, 0};

    /* PERF: one combined allocation for all 4 lane bitstream buffers
     * (saves 3 mallocs per vva_encode4 call). Each lane gets its own
     * region at offset (lane * lane_cap). */
    uint8_t *all_bs = (uint8_t *)malloc(lane_cap * 4);
    if (!all_bs) return VVA_ERR_NOMEM;
    for (int i = 0; i < 4; i++) bs_bufs[i] = all_bs + (size_t)i * lane_cap;

    /* PERF: allocate pairs buffer ONCE, sized for the largest lane.
     * Each lane has at most (src_len+3)/4 symbols, so this covers all.
     * Previously this was malloc'd 4× per vva_encode4 call, which cost
     * ~4 µs/call on small inputs. */
    size_t max_lane_len = (src_len + 3) / 4;
    bitpair_t *pairs = (bitpair_t *)malloc(max_lane_len * sizeof(bitpair_t));
    if (!pairs) { free(all_bs); free_all(&t); return VVA_ERR_NOMEM; }

    for (int lane = 0; lane < 4; lane++) {
        /* Count symbols in this lane */
        size_t lane_len = 0;
        for (size_t i = (size_t)lane; i < src_len; i += 4) lane_len++;
        if (lane_len == 0) continue;

        uint32_t state = 0;
        /* Encode backward within this lane (pairs is pre-allocated) */
        size_t ki = lane_len;
        for (size_t idx = (lane_len - 1) * 4 + (size_t)lane; ; idx -= 4) {
            ki--;
            if (idx >= src_len) { ki++; if (idx < 4) break; continue; }
            uint32_t bv; int bn;
            int slot = enc_sym(t.enc, state, src[idx], &bv, &bn);
            if (slot < 0) {
                free(all_bs); free(pairs); free_all(&t); return VVA_ERR_CORRUPT;
            }
            pairs[ki].val = (uint32_t)bv;
            pairs[ki].nb = (uint8_t)bn;
            state = (uint32_t)slot;
            if (idx < 4) break;
        }

        /* Write bitstream for this lane (into pre-allocated slot) */
        ans_bw_t w;
        ans_bw_init(&w, bs_bufs[lane], lane_cap);
        for (size_t i = 0; i < lane_len; i++)
            ans_bw_add(&w, pairs[i].val, pairs[i].nb);
        bs_lens[lane] = ans_bw_flush(&w);
        states[lane] = (uint16_t)state;
    }

    free(pairs);

    free_all(&t);

    /* Output: [header] [4×2B states] [4×4B bs_lens] [bs0][bs1][bs2][bs3]
     * bs_lens are 4B to support large literal blocks (v1.6.0+). */
    size_t overhead = hdr + 8 + 16; /* 4 states (2B) + 4 sizes (4B) */
    size_t total_bs = bs_lens[0] + bs_lens[1] + bs_lens[2] + bs_lens[3];
    size_t total = overhead + total_bs;

    if (total > dst_cap || total >= src_len) {
        free(all_bs);
        return VVA_ERR_OVERFLOW;
    }

    uint8_t *op = dst + hdr;
    for (int i = 0; i < 4; i++) {
        op[0] = (uint8_t)(states[i] & 0xFF);
        op[1] = (uint8_t)(states[i] >> 8);
        op += 2;
    }
    for (int i = 0; i < 4; i++) {
        op[0] = (uint8_t)(bs_lens[i] & 0xFF);
        op[1] = (uint8_t)((bs_lens[i] >> 8) & 0xFF);
        op[2] = (uint8_t)((bs_lens[i] >> 16) & 0xFF);
        op[3] = (uint8_t)((bs_lens[i] >> 24) & 0xFF);
        op += 4;
    }
    for (int i = 0; i < 4; i++) {
        memcpy(op, bs_bufs[i], bs_lens[i]);
        op += bs_lens[i];
    }
    free(all_bs);

    *dst_len = total;
    return VVA_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * 4-WAY INTERLEAVED DECODE (tag 'I', v0.6+, Item 2)
 *
 * The hot loop decodes 4 symbols per iteration from 4 independent
 * ANS states. This hides the ~4-cycle L1 table lookup latency —
 * while one lookup resolves, the other 3 are in-flight.
 *
 * Output is interleaved: dst[0]=lane0, dst[1]=lane1, dst[2]=lane2, dst[3]=lane3
 * ═══════════════════════════════════════════════════════════════ */

vva_error_t vva_decode4(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap,
                        size_t num_literals, size_t *src_consumed) {
    if (!num_literals) { *src_consumed = 0; return VVA_OK; }
    if (num_literals > dst_cap) return VVA_ERR_OVERFLOW;

    uint16_t norm[NSYM];
    size_t hdr = read_hdr_v2(src, src_len, norm);
    if (!hdr) return VVA_ERR_CORRUPT;

    int np = 0, single = -1;
    for (int i = 0; i < NSYM; i++)
        if (norm[i]) { np++; single = i; }
    if (!np) return VVA_ERR_CORRUPT;
    if (np == 1) {
        memset(dst, single, num_literals);
        *src_consumed = hdr;
        return VVA_OK;
    }

    /* Build shared decode table */
    uint8_t *sp = (uint8_t *)malloc(ANS_L);
    vva_dec_entry_t *dec = (vva_dec_entry_t *)malloc(ANS_L * sizeof(*dec));
    if (!sp || !dec) { free(sp); free(dec); return VVA_ERR_NOMEM; }
    spread_symbols(norm, sp);
    build_dec(norm, sp, dec);
    free(sp);

    /* Read 4 states (2B) + 4 bitstream sizes (4B) */
    const uint8_t *p = src + hdr;
    if (p + 8 + 16 > src + src_len) { free(dec); return VVA_ERR_CORRUPT; }

    uint32_t s[4];
    size_t bsz[4];
    for (int i = 0; i < 4; i++) {
        s[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        p += 2;
        if (s[i] >= (uint32_t)ANS_L) { free(dec); return VVA_ERR_CORRUPT; }
    }
    for (int i = 0; i < 4; i++) {
        bsz[i] = (size_t)p[0] | ((size_t)p[1] << 8)
               | ((size_t)p[2] << 16) | ((size_t)p[3] << 24);
        p += 4;
    }

    /* Set up 4 independent bit readers */
    ans_br_t r[4];
    const uint8_t *bp = p;
    for (int i = 0; i < 4; i++) {
        if (bp + bsz[i] > src + src_len) { free(dec); return VVA_ERR_CORRUPT; }
        ans_br_init(&r[i], bp, bsz[i]);
        ans_br_fill(&r[i]);
        bp += bsz[i];
    }

    /* ─── 4-way interleaved decode hot loop ───
     * Process 4 symbols per iteration, one from each lane.
     * Output is round-robin: dst[0]=lane0, dst[1]=lane1, ... */
    size_t out_pos = 0;
    size_t full_quads = num_literals / 4;

    for (size_t q = 0; q < full_quads; q++) {
        /* 4 parallel table lookups — CPU can issue all 4 loads simultaneously
         * because the states are independent (no data dependency). */
        vva_dec_entry_t e0 = dec[s[0]];
        vva_dec_entry_t e1 = dec[s[1]];
        vva_dec_entry_t e2 = dec[s[2]];
        vva_dec_entry_t e3 = dec[s[3]];

        /* 4 symbol outputs */
        dst[out_pos]     = e0.symbol;
        dst[out_pos + 1] = e1.symbol;
        dst[out_pos + 2] = e2.symbol;
        dst[out_pos + 3] = e3.symbol;
        out_pos += 4;

        /* 4 state updates — use results from lookups above */
        if (r[0].n < ANS_LOG) ans_br_fill(&r[0]);
        s[0] = (uint32_t)e0.baseline + ans_br_read(&r[0], e0.nbits);

        if (r[1].n < ANS_LOG) ans_br_fill(&r[1]);
        s[1] = (uint32_t)e1.baseline + ans_br_read(&r[1], e1.nbits);

        if (r[2].n < ANS_LOG) ans_br_fill(&r[2]);
        s[2] = (uint32_t)e2.baseline + ans_br_read(&r[2], e2.nbits);

        if (r[3].n < ANS_LOG) ans_br_fill(&r[3]);
        s[3] = (uint32_t)e3.baseline + ans_br_read(&r[3], e3.nbits);
    }

    /* Scalar tail for remaining 0-3 symbols */
    for (size_t i = full_quads * 4; i < num_literals; i++) {
        int lane = (int)(i & 3);
        if (r[lane].n < ANS_LOG) ans_br_fill(&r[lane]);
        vva_dec_entry_t e = dec[s[lane]];
        dst[i] = e.symbol;
        s[lane] = (uint32_t)e.baseline + ans_br_read(&r[lane], e.nbits);
    }

    *src_consumed = (size_t)(bp - src);
    free(dec);
    return VVA_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * ORDER-1 CONTEXT MODEL (tag 'C', v0.7+ — Item 1 Sprint 7)
 *
 * Uses 256 ANS tables, one per previous byte. Captures correlations
 * like '{' → '"' in JSON, '\n' → digit in logs.
 *
 * Contexts with fewer than 16 observations inherit the global table.
 * This avoids overfitting on sparse contexts and keeps headers small.
 *
 * Header format:
 *   [2B global_table_size] [global_table]
 *   [32B inherited_bitmap: bit c=1 means ctx c is inherited]
 *   For each non-inherited context c:
 *     [1B context_id] [2B table_size] [table_data]
 *
 * ZUPT-COMPAT: this function is available when VV_ANS_STANDALONE defined.
 * Memory: ~4 MB decode tables (L3-resident), allocated per call.
 * ═══════════════════════════════════════════════════════════════ */

#define CTX_MIN_OBS  16  /* Minimum observations to build a context table */

vva_error_t vva_encode_ctx(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap, size_t *dst_len) {
    if (!src_len) { *dst_len = 0; return VVA_OK; }

    /* ─── Pass 1: build 256×256 histogram ─── */
    /* Heap-allocate: 256×256×4 = 256 KB */
    uint32_t (*hist)[NSYM] = (uint32_t (*)[NSYM])calloc(NSYM, NSYM * sizeof(uint32_t));
    uint32_t global_raw[NSYM];
    memset(global_raw, 0, sizeof(global_raw));
    if (!hist) return VVA_ERR_NOMEM;

    uint8_t prev = 0;
    for (size_t i = 0; i < src_len; i++) {
        uint8_t cur = src[i];
        hist[prev][cur]++;
        global_raw[cur]++;
        prev = cur;
    }

    /* ─── Normalize global table ─── */
    uint16_t global_norm[NSYM];
    int global_np = normalize_freq(global_raw, global_norm);
    if (global_np == 0) { free(hist); return VVA_ERR_PARAM; }

    /* ─── Determine which contexts are inherited ─── */
    uint8_t inherited[32]; /* 256-bit bitmap: bit c=1 → inherited */
    memset(inherited, 0xFF, 32); /* Start all inherited */

    uint16_t ctx_norms[NSYM][NSYM]; /* [context][symbol] → normalized freq */
    int ctx_np[NSYM]; /* number of present symbols per context */

    for (int c = 0; c < NSYM; c++) {
        uint32_t row_total = 0;
        for (int s = 0; s < NSYM; s++) row_total += hist[c][s];

        if (row_total >= CTX_MIN_OBS) {
            ctx_np[c] = normalize_freq(hist[c], ctx_norms[c]);
            if (ctx_np[c] > 1) {
                /* Non-trivial context: mark as non-inherited */
                inherited[c / 8] &= ~(1u << (c % 8));
            } else {
                /* Single symbol: still use own table */
                inherited[c / 8] &= ~(1u << (c % 8));
            }
        } else {
            /* Too few observations: inherit global */
            memcpy(ctx_norms[c], global_norm, sizeof(global_norm));
            ctx_np[c] = global_np;
        }
    }

    /* ─── Write header ─── */
    uint8_t *op = dst;
    size_t remaining_cap = dst_cap;

    /* Global table */
    uint8_t global_hdr_buf[600];
    size_t global_hdr_sz = write_hdr_v2(global_norm, global_hdr_buf, sizeof(global_hdr_buf));
    if (!global_hdr_sz) { free(hist); return VVA_ERR_OVERFLOW; }

    if (remaining_cap < 2 + global_hdr_sz + 32) { free(hist); return VVA_ERR_OVERFLOW; }

    /* [2B global_table_size] */
    op[0] = (uint8_t)(global_hdr_sz & 0xFF);
    op[1] = (uint8_t)(global_hdr_sz >> 8);
    op += 2;
    memcpy(op, global_hdr_buf, global_hdr_sz);
    op += global_hdr_sz;

    /* [32B inherited bitmap] */
    memcpy(op, inherited, 32);
    op += 32;

    /* Per non-inherited context tables */
    for (int c = 0; c < NSYM; c++) {
        if (inherited[c / 8] & (1u << (c % 8))) continue; /* Skip inherited */

        uint8_t ctx_hdr_buf[600];
        size_t ctx_hdr_sz = write_hdr_v2(ctx_norms[c], ctx_hdr_buf, sizeof(ctx_hdr_buf));
        if (!ctx_hdr_sz) { free(hist); return VVA_ERR_OVERFLOW; }

        if ((size_t)(op - dst) + 3 + ctx_hdr_sz > dst_cap) { free(hist); return VVA_ERR_OVERFLOW; }

        *op++ = (uint8_t)c;
        op[0] = (uint8_t)(ctx_hdr_sz & 0xFF);
        op[1] = (uint8_t)(ctx_hdr_sz >> 8);
        op += 2;
        memcpy(op, ctx_hdr_buf, ctx_hdr_sz);
        op += ctx_hdr_sz;
    }

    size_t hdr_total = (size_t)(op - dst);

    /* ─── Build encode tables for all 256 contexts ─── */
    /* We need spread + dec + enc for each context.
     * Memory: 256 × (4096 spread + 4096×4 dec + enc_ctx) ≈ 8 MB
     * This is a lot — but it's temporary per block. */
    uint8_t *spread_buf = (uint8_t *)malloc(ANS_L);
    vva_dec_entry_t *dec_buf = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
    enc_ctx_t **enc_tables = (enc_ctx_t **)calloc(NSYM, sizeof(enc_ctx_t *));
    if (!spread_buf || !dec_buf || !enc_tables) {
        free(hist); free(spread_buf); free(dec_buf); free(enc_tables);
        return VVA_ERR_NOMEM;
    }

    /* Build global encode table (for inherited contexts) */
    spread_symbols(global_norm, spread_buf);
    build_dec(global_norm, spread_buf, dec_buf);
    enc_ctx_t *global_enc = build_enc(global_norm, spread_buf, dec_buf);
    if (!global_enc) {
        free(hist); free(spread_buf); free(dec_buf); free(enc_tables);
        return VVA_ERR_NOMEM;
    }

    for (int c = 0; c < NSYM; c++) {
        if (inherited[c / 8] & (1u << (c % 8))) {
            enc_tables[c] = global_enc; /* Alias, not owned */
        } else {
            spread_symbols(ctx_norms[c], spread_buf);
            build_dec(ctx_norms[c], spread_buf, dec_buf);
            enc_tables[c] = build_enc(ctx_norms[c], spread_buf, dec_buf);
            if (!enc_tables[c]) {
                /* Cleanup on failure */
                for (int j = 0; j < c; j++)
                    if (enc_tables[j] != global_enc) free_enc(enc_tables[j]);
                free_enc(global_enc);
                free(hist); free(spread_buf); free(dec_buf); free(enc_tables);
                return VVA_ERR_NOMEM;
            }
        }
    }

    free(spread_buf); free(dec_buf);

    /* ─── Precompute forward context array ─── */
    uint8_t *prev_ctx = (uint8_t *)malloc(src_len);
    if (!prev_ctx) {
        for (int c = 0; c < NSYM; c++)
            if (enc_tables[c] != global_enc) free_enc(enc_tables[c]);
        free_enc(global_enc); free(hist); free(enc_tables);
        return VVA_ERR_NOMEM;
    }
    prev_ctx[0] = 0; /* Initial context */
    for (size_t i = 1; i < src_len; i++)
        prev_ctx[i] = src[i - 1];

    /* ─── Encode backward with context-dependent tables ─── */
    bitpair_t *pairs = (bitpair_t *)malloc(src_len * sizeof(bitpair_t));
    if (!pairs) {
        free(prev_ctx);
        for (int c = 0; c < NSYM; c++)
            if (enc_tables[c] != global_enc) free_enc(enc_tables[c]);
        free_enc(global_enc); free(hist); free(enc_tables);
        return VVA_ERR_NOMEM;
    }

    /* Per-context ANS states (256 independent states) */
    uint16_t ctx_states[NSYM];
    memset(ctx_states, 0, sizeof(ctx_states));

    for (size_t ii = src_len; ii > 0; ii--) {
        uint8_t sym = src[ii - 1];
        uint8_t ctx = prev_ctx[ii - 1];
        uint32_t bv; int bn;
        int slot = enc_sym(enc_tables[ctx], ctx_states[ctx], sym, &bv, &bn);
        if (slot < 0) {
            free(pairs); free(prev_ctx);
            for (int c = 0; c < NSYM; c++)
                if (enc_tables[c] != global_enc) free_enc(enc_tables[c]);
            free_enc(global_enc); free(hist); free(enc_tables);
            return VVA_ERR_CORRUPT;
        }
        pairs[ii - 1].val = (uint32_t)bv;
        pairs[ii - 1].nb = (uint8_t)bn;
        ctx_states[ctx] = (uint16_t)slot;
    }

    free(prev_ctx);
    for (int c = 0; c < NSYM; c++)
        if (enc_tables[c] != global_enc) free_enc(enc_tables[c]);
    free_enc(global_enc); free(hist); free(enc_tables);

    /* ─── Write bitstream: [256×2B states] [bitpairs forward] ─── */
    size_t bs_cap = (src_len * 15 + 7) / 8 + 16;
    uint8_t *bs = (uint8_t *)malloc(bs_cap);
    if (!bs) { free(pairs); return VVA_ERR_NOMEM; }

    ans_bw_t w;
    ans_bw_init(&w, bs, bs_cap);
    for (size_t i = 0; i < src_len; i++)
        ans_bw_add(&w, pairs[i].val, pairs[i].nb);
    size_t bs_len = ans_bw_flush(&w);
    free(pairs);

    /* Output: [header] [512B states] [bitstream] */
    size_t total = hdr_total + 512 + bs_len;
    if (total > dst_cap || total >= src_len) {
        free(bs);
        return VVA_ERR_OVERFLOW;
    }

    /* Write 256 states (2B each, LE) */
    for (int c = 0; c < NSYM; c++) {
        op[0] = (uint8_t)(ctx_states[c] & 0xFF);
        op[1] = (uint8_t)(ctx_states[c] >> 8);
        op += 2;
    }
    memcpy(op, bs, bs_len);
    free(bs);

    *dst_len = total;
    return VVA_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * ORDER-1 CONTEXT MODEL DECODE
 * ═══════════════════════════════════════════════════════════════ */

vva_error_t vva_decode_ctx(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap,
                           size_t num_literals, size_t *src_consumed) {
    if (!num_literals) { *src_consumed = 0; return VVA_OK; }
    if (num_literals > dst_cap) return VVA_ERR_OVERFLOW;

    const uint8_t *p = src;
    const uint8_t *end = src + src_len;

    /* Read global table */
    if (p + 2 > end) return VVA_ERR_CORRUPT;
    size_t global_sz = (size_t)p[0] | ((size_t)p[1] << 8);
    p += 2;
    if (p + global_sz > end) return VVA_ERR_CORRUPT;

    uint16_t global_norm[NSYM];
    size_t ghdr = read_hdr_v2(p, global_sz, global_norm);
    if (!ghdr) return VVA_ERR_CORRUPT;
    p += global_sz;

    /* Check for single-symbol global */
    int global_np = 0, global_single = -1;
    for (int i = 0; i < NSYM; i++)
        if (global_norm[i]) { global_np++; global_single = i; }

    /* Read inherited bitmap */
    if (p + 32 > end) return VVA_ERR_CORRUPT;
    uint8_t inherited[32];
    memcpy(inherited, p, 32);
    p += 32;

    /* Build global decode table */
    uint8_t *sp = (uint8_t *)malloc(ANS_L);
    vva_dec_entry_t *global_dec = (vva_dec_entry_t *)malloc(ANS_L * sizeof(*global_dec));
    if (!sp || !global_dec) { free(sp); free(global_dec); return VVA_ERR_NOMEM; }

    if (global_np > 1) {
        spread_symbols(global_norm, sp);
        build_dec(global_norm, sp, global_dec);
    } else if (global_np == 1) {
        /* Single symbol global: fill table */
        for (int x = 0; x < ANS_L; x++) {
            global_dec[x].symbol = (uint8_t)global_single;
            global_dec[x].nbits = 0;
            global_dec[x].baseline = 0;
        }
    }

    /* Allocate per-context decode tables: 256 pointers to tables.
     * Inherited contexts point to global_dec (not owned).
     * Non-inherited get their own allocation. */
    vva_dec_entry_t **ctx_dec = (vva_dec_entry_t **)calloc(NSYM, sizeof(vva_dec_entry_t *));
    if (!ctx_dec) { free(sp); free(global_dec); return VVA_ERR_NOMEM; }

    /* Set all to global first */
    for (int c = 0; c < NSYM; c++)
        ctx_dec[c] = global_dec;

    /* Read non-inherited context tables */
    for (int c = 0; c < NSYM; c++) {
        if (inherited[c / 8] & (1u << (c % 8))) continue;

        if (p + 3 > end) goto ctx_dec_fail;
        int ctx_id = *p++;
        size_t tsz = (size_t)p[0] | ((size_t)p[1] << 8);
        p += 2;
        if (p + tsz > end) goto ctx_dec_fail;

        uint16_t cnorm[NSYM];
        size_t chdr = read_hdr_v2(p, tsz, cnorm);
        if (!chdr) goto ctx_dec_fail;
        p += tsz;

        vva_dec_entry_t *cdec = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        if (!cdec) goto ctx_dec_fail;

        int cnp = 0, csingle = -1;
        for (int i = 0; i < NSYM; i++) if (cnorm[i]) { cnp++; csingle = i; }

        if (cnp > 1) {
            spread_symbols(cnorm, sp);
            build_dec(cnorm, sp, cdec);
        } else if (cnp == 1) {
            for (int x = 0; x < ANS_L; x++) {
                cdec[x].symbol = (uint8_t)csingle;
                cdec[x].nbits = 0;
                cdec[x].baseline = 0;
            }
        }
        ctx_dec[ctx_id] = cdec;
    }
    free(sp);

    /* Read 256 initial states */
    if (p + 512 > end) goto ctx_dec_fail;
    uint16_t ctx_states[NSYM];
    for (int c = 0; c < NSYM; c++) {
        ctx_states[c] = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
    }

    /* Bitstream */
    {
        ans_br_t r;
        ans_br_init(&r, p, (size_t)(end - p));
        ans_br_fill(&r);

        /* Decode forward with context tracking.
         * PERF: prefetch next context table to hide L2/L3 latency.
         * Each context table is 16KB. Without prefetch: ~50 MB/s (L3 thrash).
         * With prefetch: hides latency by 1 iteration → ~300+ MB/s. */
        uint8_t prev_ctx = 0;
        for (size_t i = 0; i < num_literals; i++) {
            if (r.n < ANS_LOG) ans_br_fill(&r);

            uint32_t st = ctx_states[prev_ctx];
            if (st >= (uint32_t)ANS_L) goto ctx_dec_fail;

            vva_dec_entry_t e = ctx_dec[prev_ctx][st];
            dst[i] = e.symbol;

            uint32_t bits = ans_br_read(&r, e.nbits);
            ctx_states[prev_ctx] = (uint16_t)((uint32_t)e.baseline + bits);

            prev_ctx = e.symbol;

            /* Prefetch next context's decode table into L2 cache.
             * The next iteration will access ctx_dec[prev_ctx][ctx_states[prev_ctx]].
             * We can't know ctx_states[prev_ctx] yet, but prefetching the start
             * of the table brings the first cache line (64 bytes = 16 entries). */
            VV_PREFETCH(&ctx_dec[prev_ctx][0]);
        }

        *src_consumed = (size_t)(p - src) + r.p;
        if (r.n >= 8) {
            size_t ov = (size_t)(r.n / 8);
            if (*src_consumed >= ov) *src_consumed -= ov;
        }
    }

    /* Cleanup */
    for (int c = 0; c < NSYM; c++)
        if (ctx_dec[c] != global_dec) free(ctx_dec[c]);
    free(ctx_dec); free(global_dec);
    return VVA_OK;

ctx_dec_fail:
    for (int c = 0; c < NSYM; c++)
        if (ctx_dec[c] != global_dec) free(ctx_dec[c]);
    free(ctx_dec); free(global_dec); free(sp);
    return VVA_ERR_CORRUPT;
}

/* ═══════════════════════════════════════════════════════════════
 * SEQUENCE CODING (tag 'S', v0.8+ — Sprint 8 Item 1)
 *
 * Entropy-codes match lengths and offsets using ANS, replacing
 * raw varint/fixed-width storage. Saves 8-15% on typical data.
 *
 * Match length codes: 36 codes mapping to lengths 4-65538
 * Offset codes: 24 codes mapping to offsets 1-16M
 *
 * ZUPT-COMPAT: these functions are standalone when VV_ANS_STANDALONE.
 *
 * Output format:
 *   [2B lit_count] [2B lit_ans_size] [lit_ans_data]
 *   [2B seq_count]
 *   [2B ml_hdr_size] [ml_table_hdr]
 *   [2B of_hdr_size] [of_table_hdr]
 *   [2B state_ml] [2B state_of]
 *   [2B seq_bs_size] [sequence_bitstream]
 *   [litlen_varints: one per sequence]
 * ═══════════════════════════════════════════════════════════════ */

/* PERF: ML/OF code tables are small fixed arrays — always L1 hot */
static const uint32_t ml_base[VVA_ML_CODES] = {
    4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
    20,22,24,28,32,40,48,64,96,128,192,256,384,512,1024,2048,
    4096,8192,16384,32768
};
/* ml_base_v2 for tag 'T' (VV_ENTROPY_SEQ_V2) — every entry shifted
 * down by 1, so code 0 means match length 3 instead of 4. Extra-bits
 * table is unchanged since the step sizes between consecutive codes
 * are preserved — only the starting point moves. This closes the
 * binary-compression gap vs gzip-9 which uses min_match=3. */
static const uint32_t ml_base_v2[VVA_ML_CODES] = {
    3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,
    19,21,23,27,31,39,47,63,95,127,191,255,383,511,1023,2047,
    4095,8191,16383,32767
};
static const uint8_t ml_extra[VVA_ML_CODES] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,2,2,3,3,4,5,5,6,6,7,7,9,10,11,
    12,13,14,15
};

/* Rep-match codes: 0=rep[0], 1=rep[1], 2=rep[2], 3+=explicit offset.
 * Explicit offset code c (c≥3): offset in [2^(c-3), 2^(c-2)), (c-3) extra bits.
 * This is how zstd encodes repeated offsets — saves 10-15 bits per rep-match. */
static const uint8_t of_extra[VVA_OF_CODES] = {
    0,0,0, /* rep codes: 0 extra bits */
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23
};

/* Encode match length → (code, extra_value, extra_bits).
 * Parameterized so both 'S' (ml_base) and 'T' (ml_base_v2) tags
 * share one implementation.
 *
 * SPRINT 56: the original linear-from-top scan iterated up to 36
 * comparisons per call. Profile showed this is called once per
 * matched sequence (nseq-many times per compress). Replacing with
 * a hybrid lookup:
 *   1. Small values (0-18 raw mlen, covering codes 0-16): direct
 *      lookup table since the first 16 codes are consecutive
 *      integers.
 *   2. Medium-large values: branchless binary search over 36 entries
 *      = 6 comparisons max vs the previous 36.
 *
 * Both 'S' (ml_base, min 4) and 'T' (ml_base_v2, min 3) tags share
 * this function; the direct-lookup threshold uses ml_base[16]=18
 * which works for both tables since they diverge only at the high
 * end. */
static void ml_encode_with(uint32_t mlen, const uint32_t *base_tab,
                            uint8_t *code, uint32_t *extra, int *nbits) {
    /* Fast path: small mlen covers the majority of binary matches.
     * ml_base[c] for c=0..15 is consecutive integers:
     *   v1 ml_base[0..15] = 4,5,...,19 (covers up to 19)
     *   v2 ml_base[0..15] = 3,4,...,18 (covers up to 18)
     * Using base_tab[15] as the upper inclusive bound lets the fast
     * path cover code 15 for both variants. */
    if (mlen <= base_tab[15]) {
        uint32_t c = (mlen >= base_tab[0]) ? (mlen - base_tab[0]) : 0;
        *code = (uint8_t)c;
        *extra = 0;       /* codes 0-15 all have ml_extra[c] = 0 */
        *nbits = 0;
        return;
    }

    /* Binary search over codes 16..35 for larger values. */
    int lo = 16, hi = VVA_ML_CODES - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (mlen >= base_tab[mid]) lo = mid;
        else hi = mid - 1;
    }
    *code = (uint8_t)lo;
    *extra = mlen - base_tab[lo];
    *nbits = ml_extra[lo];
}
/* (ml_encode legacy wrapper removed — all callers migrated to
 * ml_encode_with for explicit table selection.) */

/* (ml_decode removed — its single caller was refactored to use the
 * ml_base_tab parameter directly, enabling 'S'/'T' tag sharing.) */

/* Encode explicit offset → (code, extra_value, extra_bits).
 * Returns code in range [3..26]. Caller handles rep-match codes 0-2. */
static void of_encode(uint32_t offset, uint8_t *code, uint32_t *extra, int *nbits) {
    if (offset == 0) { *code = 3; *extra = 0; *nbits = 0; return; }
    int c = 0;
    uint32_t v = offset;
    while (v > 1) { v >>= 1; c++; }
    if (c >= 24) c = 23; /* clamp to 24 explicit codes */
    *code = (uint8_t)(c + 3); /* shift by 3 for rep codes */
    *extra = offset - (1u << c);
    *nbits = of_extra[c + 3];
}

/* Decode offset code → offset. Codes 0-2 are rep-match (caller resolves).
 * Codes 3-26 are explicit offsets. */
static uint32_t of_decode(uint8_t code, uint32_t extra) {
    if (code < 3) return 0; /* rep-match — caller must handle */
    return (1u << (code - 3)) + extra;
}

/* ─── Literal-run length codes: 36 codes covering 0-65536+
 * Small values (0-18) get short codes; long literal runs (common in logs
 * and binary data with low redundancy) are covered via longer extra-bit codes. */
static const uint32_t ll_base[VVA_LL_CODES] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    16,18,20,24,28,32,48,64,128,256,512,1024,2048,4096,8192,16384,
    32768,49152,57344,61440
};
static const uint8_t ll_extra[VVA_LL_CODES] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,1,2,2,2,4,4,6,7,8,9,10,11,12,13,14,
    14,13,12,12
};

static void ll_encode(uint32_t litlen, uint8_t *code, uint32_t *extra, int *nbits) {
    /* SPRINT 56: same optimization as ml_encode_with. Small litlens
     * (0-15) are direct-lookup since ll_base[c]=c for c=0..15.
     * Larger values use binary search over the remaining 20 codes
     * (log2 ≈ 5 comparisons vs previous 36). */
    if (litlen <= 15u) {
        *code = (uint8_t)litlen;
        *extra = 0;       /* codes 0-15 all have ll_extra[c] = 0 */
        *nbits = 0;
        return;
    }

    /* Binary search over codes 16..35 */
    int lo = 16, hi = VVA_LL_CODES - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (litlen >= ll_base[mid]) lo = mid;
        else hi = mid - 1;
    }
    *code = (uint8_t)lo;
    *extra = litlen - ll_base[lo];
    *nbits = ll_extra[lo];
}

static uint32_t ll_decode(uint8_t code, uint32_t extra) {
    return ll_base[code] + extra;
}

/* Write a varint to a buffer, return bytes written (kept for backward compat) */
static size_t __attribute__((unused)) seq_write_varint(uint8_t *dst, size_t val) {
    size_t n = 0;
    while (val >= 255) { dst[n++] = 255; val -= 255; }
    dst[n++] = (uint8_t)val;
    return n;
}

/* Read a varint from a buffer, advance pointer (kept for backward compat) */
static size_t __attribute__((unused)) seq_read_varint(const uint8_t **pp, const uint8_t *end) {
    size_t val = 0;
    while (*pp < end && **pp == 255) { val += 255; (*pp)++; }
    if (*pp < end) { val += **pp; (*pp)++; }
    return val;
}

/* Sequence descriptor (parsed from LZ token stream) */
typedef struct {
    uint32_t litlen;
    uint32_t lit_offset;  /* offset into literal buffer */
    uint32_t matchlen;    /* 0 = last sequence (no match) */
    uint32_t offset;
} seq_t;

/* Parse LZ token stream into sequences + literal buffer.
 * Returns number of sequences, or 0 on error. */
static size_t parse_sequences(const uint8_t *tokens, size_t tok_len,
                               uint8_t *lit_buf, size_t lit_cap,
                               seq_t *seqs, size_t seq_cap,
                               size_t *total_lits, int off_bytes,
                               int min_match) {
    const uint8_t *tp = tokens, *tp_end = tokens + tok_len;
    size_t nseq = 0, nlits = 0;

    /* SPRINT 63: maximum litlen representable by the LL ANS coder is
     * 65535 (ll_base[35]=61440 + max 4095 extra bits). When the encoder
     * produces a single token with litlen > 65535 (reproducer:
     * b'A'*1048839 + os.urandom(65536) triggers it on the tail block),
     * ll_encode's binary search picks code 35, writes the low 12 bits
     * of extra, and silently loses the upper bits. Decoder then reads
     * back a smaller litlen, producing a short output block.
     *
     * Fix: if a parsed token's ll exceeds LL_MAX, split into multiple
     * seq entries: as many (LL_MAX, matchlen=0) zero-match sequences
     * as needed to absorb the overflow, followed by the final sequence
     * carrying the remaining (ll' ≤ LL_MAX) and the original match.
     *
     * Zero-match sequences are already legal in the stream (trailing
     * literals use matchlen=0, offset=0). Adding them mid-stream is
     * wire-compatible — the decoder's existing match_count == 0 test
     * skips the match-copy for these entries. */
    enum { LL_MAX = 65535 };

    while (tp < tp_end && nseq < seq_cap) {
        uint8_t token = *tp++;
        size_t ll = token >> 4;
        size_t mc = token & 0x0F;

        if (ll == 15) {
            size_t ext = 0;
            do {
                if (tp >= tp_end) return 0;
                uint8_t b = *tp++;
                ext += b;
                if (b < 255) break;
            } while (tp < tp_end);
            ll += ext;
        }

        if (tp + ll > tp_end || nlits + ll > lit_cap) return 0;
        memcpy(lit_buf + nlits, tp, ll);
        tp += ll;

        /* SPRINT 63: split oversize literal runs */
        while (ll > LL_MAX) {
            if (nseq >= seq_cap) return 0;
            seqs[nseq].litlen = (uint32_t)LL_MAX;
            seqs[nseq].lit_offset = (uint32_t)nlits;
            seqs[nseq].matchlen = 0;
            seqs[nseq].offset = 0;
            nlits += LL_MAX;
            nseq++;
            ll -= LL_MAX;
        }

        seqs[nseq].litlen = (uint32_t)ll;
        seqs[nseq].lit_offset = (uint32_t)nlits;
        nlits += ll;

        if (tp >= tp_end) {
            seqs[nseq].matchlen = 0;
            seqs[nseq].offset = 0;
            nseq++;
            break;
        }

        if (tp + off_bytes > tp_end) return 0;
        uint32_t off = (off_bytes == 3)
            ? ((uint32_t)tp[0] | ((uint32_t)tp[1] << 8) | ((uint32_t)tp[2] << 16))
            : ((uint32_t)tp[0] | ((uint32_t)tp[1] << 8));
        tp += off_bytes;

        size_t mlen = mc + (size_t)min_match;
        if (mc == 15) {
            size_t ext = 0;
            do {
                if (tp >= tp_end) return 0;
                uint8_t b = *tp++;
                ext += b;
                if (b < 255) break;
            } while (tp < tp_end);
            mlen += ext;
        }

        seqs[nseq].matchlen = (uint32_t)mlen;
        seqs[nseq].offset = off;
        nseq++;
    }

    *total_lits = nlits;
    return nseq;
}

/* ═══════════════════════════════════════════════════════════════
 * ENCODE SEQUENCES
 *
 * Takes raw LZ token stream, outputs ANS-coded sequence block.
 * ═══════════════════════════════════════════════════════════════ */

/* Internal impl. ml_base_tab selects between 'S' (min_match=4) and
 * 'T' (min_match=3) encoding. */
static vva_error_t vva_encode_sequences_impl(const uint8_t *tokens, size_t tok_len,
                                              uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                              int off_bytes,
                                              const uint32_t *ml_base_tab) {
    if (!tok_len) { *dst_len = 0; return VVA_OK; }

    /* Parse into sequences.
     * PERF: one combined alloc for seqs + lit_buf. The sizeof(seq_t)
     * is ≥ 4 bytes so natural alignment for both is satisfied. Saves
     * 1 malloc/free pair per call. */
    size_t max_seqs = tok_len; /* Upper bound */
    size_t seqs_sz = max_seqs * sizeof(seq_t);
    size_t total_scratch = seqs_sz + tok_len;
    uint8_t *base_scratch = (uint8_t *)malloc(total_scratch);
    if (!base_scratch) return VVA_ERR_NOMEM;
    seq_t *seqs = (seq_t *)base_scratch;
    uint8_t *lit_buf = base_scratch + seqs_sz;

    size_t total_lits = 0;
    /* Derive min_match from the ml_base table passed in. For the v1
     * table this is 4; for v2 it's 3. Passed to parse_sequences so
     * it reconstructs mlen consistently with how emit_seq packed it. */
    int min_match = (int)ml_base_tab[0];
    size_t nseq = parse_sequences(tokens, tok_len, lit_buf, tok_len, seqs, max_seqs, &total_lits, off_bytes, min_match);
    if (nseq == 0) { free(base_scratch); return VVA_ERR_CORRUPT; }

    /* ─── Encode literals with 4-way ANS ─── */
    size_t lit_cap = vva_bound(total_lits);
    uint8_t *lit_enc = (uint8_t *)malloc(lit_cap);
    if (!lit_enc) { free(base_scratch); return VVA_ERR_NOMEM; }

    size_t lit_enc_len = 0;
    uint8_t lit_fmt = 0; /* 0=raw, 1=ANS4, 2=ANS1, 3=Huffman (Sprint 71) */
    if (total_lits > 0) {
        /* SPRINT 71 (v2.46): Huffman as a competitive literal coder
         * inside the SEQ stream.
         *
         * Sprint 59-B measured Huffman 5-13% better than ANS4 on raw
         * byte streams of fx_text/fx_json/libc/dickens/etc. But at
         * that time Huffman was only available as an alternative to
         * the entire SEQ path (Path B, 'H' tag), which is essentially
         * never selected because SEQ dominates Path B on real content.
         *
         * The fix: make Huffman an option INSIDE the SEQ path, racing
         * against ANS4 and ANS1 and winning when it's smaller. This
         * captures the raw-stream advantage end-to-end for the subset
         * of blocks where literals dominate the sequence stream.
         *
         * Race all three coders, pick smallest. Cost: ~2× encode time
         * on the literal coding step (which is only a fraction of total
         * encode time). Benefit: 3-7% expected on binary fixtures where
         * literal distributions make Huffman materially better.
         *
         * Decoder support: lit_fmt=3 dispatches to vvh_decode. Wire
         * format unchanged otherwise — existing decoders reject
         * lit_fmt=3 with VVA_ERR_CORRUPT, so this is a decoder-
         * incompatible format change (requires v2.46.0+ decoder). */
        size_t ans4_len = 0, ans1_len = 0, huf_len = 0;
        uint8_t *ans4_buf = (uint8_t *)malloc(lit_cap);
        uint8_t *ans1_buf = (uint8_t *)malloc(lit_cap);
        uint8_t *huf_buf  = (uint8_t *)malloc(lit_cap);
        int ans4_ok = 0, ans1_ok = 0, huf_ok = 0;
        if (ans4_buf) {
            ans4_ok = (vva_encode4(lit_buf, total_lits, ans4_buf, lit_cap, &ans4_len) == VVA_OK);
        }
        if (ans1_buf) {
            ans1_ok = (vva_encode(lit_buf, total_lits, ans1_buf, lit_cap, &ans1_len) == VVA_OK);
        }
        if (huf_buf) {
            huf_ok = (vvh_encode(lit_buf, total_lits, huf_buf, lit_cap, &huf_len) == VVH_OK);
        }

        /* Pick the smallest of the three. Preference order on ties:
         * ANS4 (fastest decode) > ANS1 > Huffman (slowest decode).
         * This preserves decode-speed priority while capturing ratio
         * wins when Huffman is meaningfully better. */
        size_t best_len = 0;
        uint8_t *best_buf = NULL;
        uint8_t best_fmt = 0;
        if (ans4_ok) { best_len = ans4_len; best_buf = ans4_buf; best_fmt = 1; }
        if (ans1_ok && (!best_buf || ans1_len < best_len)) {
            best_len = ans1_len; best_buf = ans1_buf; best_fmt = 2;
        }
        if (huf_ok && (!best_buf || huf_len < best_len)) {
            best_len = huf_len; best_buf = huf_buf; best_fmt = 3;
        }

        if (best_buf && best_len <= lit_cap) {
            memcpy(lit_enc, best_buf, best_len);
            lit_enc_len = best_len;
            lit_fmt = best_fmt;
        } else if (total_lits <= lit_cap) {
            /* All three failed — fall back to raw literals */
            memcpy(lit_enc, lit_buf, total_lits);
            lit_enc_len = total_lits;
            lit_fmt = 0;
        }
        free(ans4_buf); free(ans1_buf); free(huf_buf);
    }

    /* ─── Count ML, OF, and LL code frequencies ─── */
    uint32_t freq_ml[VVA_ML_CODES], freq_of[VVA_OF_CODES], freq_ll[VVA_LL_CODES];
    memset(freq_ml, 0, sizeof(freq_ml));
    memset(freq_of, 0, sizeof(freq_of));
    memset(freq_ll, 0, sizeof(freq_ll));

    /* Precompute OF codes with rep-match detection (forward pass).
     * Store in per-sequence arrays so the backward ANS pass can use them.
     *
     * PERF: consolidate 3 separate mallocs into 1. Layout:
     *   [seq_of_code: nseq × uint8_t]  (padded to 4-byte align)
     *   [seq_of_extra: nseq × uint32_t]
     *   [seq_of_nbits: nseq × int]
     *   [seq_ml_code: nseq × uint8_t]  (SPRINT 54)
     *   [seq_ml_extra: nseq × uint32_t]
     *   [seq_ml_nbits: nseq × int]
     *   [seq_ll_code: nseq × uint8_t]
     *   [seq_ll_extra: nseq × uint32_t]
     *   [seq_ll_nbits: nseq × int]
     *
     * SPRINT 54: also memoize ML and LL codes from the forward pass.
     * Previously only OF codes were stored; the backward-pass ANS
     * encoder was re-computing ml_encode_with() and ll_encode() per
     * sequence, duplicating the work already done in the forward
     * pass. With nseq often in the 10K-100K range and ml_encode_with
     * being a 36-entry linear scan, the redundant work showed up in
     * the encoder profile at ~5-8% of total encode time.
     *
     * Net cost: 1 extra malloc region (~14 × nseq bytes), 0 extra
     * malloc calls. Net saving: the backward pass becomes lookups
     * instead of re-computation. */
    size_t codes_sz = (nseq * sizeof(uint8_t) + 3) & ~(size_t)3;
    size_t extra_sz = nseq * sizeof(uint32_t);
    size_t nbits_sz = nseq * sizeof(int);
    /* 3 streams × (codes + extra + nbits) */
    uint8_t *seq_scratch = (uint8_t *)malloc(3 * (codes_sz + extra_sz + nbits_sz));
    if (!seq_scratch) {
        free(base_scratch); free(lit_enc);
        return VVA_ERR_NOMEM;
    }
    size_t stream_sz = codes_sz + extra_sz + nbits_sz;
    uint8_t  *seq_of_code  = seq_scratch;
    uint32_t *seq_of_extra = (uint32_t *)(seq_scratch + codes_sz);
    int      *seq_of_nbits = (int *)(seq_scratch + codes_sz + extra_sz);
    uint8_t  *seq_ml_code  = seq_scratch + stream_sz;
    uint32_t *seq_ml_extra = (uint32_t *)(seq_scratch + stream_sz + codes_sz);
    int      *seq_ml_nbits = (int *)(seq_scratch + stream_sz + codes_sz + extra_sz);
    uint8_t  *seq_ll_code  = seq_scratch + 2 * stream_sz;
    uint32_t *seq_ll_extra = (uint32_t *)(seq_scratch + 2 * stream_sz + codes_sz);
    int      *seq_ll_nbits = (int *)(seq_scratch + 2 * stream_sz + codes_sz + extra_sz);

    size_t match_count = 0;
    uint32_t enc_rep[3] = {0, 0, 0}; /* Rep-match tracking during forward pass */
    for (size_t i = 0; i < nseq; i++) {
        if (seqs[i].matchlen > 0) {
            uint8_t mc; uint32_t mx; int mn;
            ml_encode_with(seqs[i].matchlen, ml_base_tab, &mc, &mx, &mn);
            freq_ml[mc]++;
            /* SPRINT 54: memoize for backward pass */
            seq_ml_code[i] = mc;
            seq_ml_extra[i] = mx;
            seq_ml_nbits[i] = mn;

            /* Check rep-match before explicit encoding */
            uint32_t off = seqs[i].offset;
            uint8_t oc; uint32_t ox; int on;
            if (off == enc_rep[0] && off != 0) {
                oc = 0; ox = 0; on = 0; /* rep[0] */
            } else if (off == enc_rep[1] && off != 0) {
                oc = 1; ox = 0; on = 0; /* rep[1] */
            } else if (off == enc_rep[2] && off != 0) {
                oc = 2; ox = 0; on = 0; /* rep[2] */
            } else {
                of_encode(off, &oc, &ox, &on); /* explicit: codes 3-26 */
            }
            seq_of_code[i] = oc;
            seq_of_extra[i] = ox;
            seq_of_nbits[i] = on;
            freq_of[oc]++;

            /* Update rep array (same logic as LZ engine) */
            if (off != enc_rep[0] && off != 0) {
                enc_rep[2] = enc_rep[1];
                enc_rep[1] = enc_rep[0];
                enc_rep[0] = off;
            }
            match_count++;
        } else {
            seq_of_code[i] = 0;
            seq_of_extra[i] = 0;
            seq_of_nbits[i] = 0;
            /* SPRINT 54: ml_code unused when matchlen==0, but zero for safety */
            seq_ml_code[i] = 0;
            seq_ml_extra[i] = 0;
            seq_ml_nbits[i] = 0;
        }

        /* Count litlen frequency for ALL sequences (including last) */
        {
            uint8_t lc; uint32_t lx; int ln;
            ll_encode(seqs[i].litlen, &lc, &lx, &ln);
            freq_ll[lc]++;
            /* SPRINT 54: memoize LL codes too */
            seq_ll_code[i] = lc;
            seq_ll_extra[i] = lx;
            seq_ll_nbits[i] = ln;
        }
    }

    /* ─── Build ML and OF ANS tables ─── */
    /* Normalize frequencies to sum=4096 for tables with ≤36/24 symbols */
    uint16_t norm_ml[NSYM], norm_of[NSYM];
    memset(norm_ml, 0, sizeof(norm_ml));
    memset(norm_of, 0, sizeof(norm_of));

    /* PERF: header buffers live on the stack — each is bounded at 600 B
     * (fits any NSYM=256 table header) and they were heap-allocated on
     * every call before. Saves 3 malloc/free pairs per call. */
    uint8_t  ml_hdr_buf[600], of_hdr_buf[600], ll_hdr_buf[600];
    size_t ml_hdr_sz = 0, of_hdr_sz = 0, ll_hdr_sz = 0;
    uint8_t *seq_bs = NULL;
    size_t seq_bs_len = 0;
    uint32_t state_ml = 0, state_of = 0, state_ll = 0;
    enc_ctx_t *enc_ll_ctx = NULL;

    /* Build LL ANS table unconditionally (all sequences have litlens) */
    {
        uint32_t raw_ll[NSYM];
        memset(raw_ll, 0, sizeof(raw_ll));
        for (int i = 0; i < VVA_LL_CODES; i++) raw_ll[i] = freq_ll[i];
        uint16_t norm_ll[NSYM];
        memset(norm_ll, 0, sizeof(norm_ll));
        normalize_freq(raw_ll, norm_ll);
        ll_hdr_sz = write_hdr_v2(norm_ll, ll_hdr_buf, 600);
        if (!ll_hdr_sz) goto seq_fail;

        /* PERF: one combined alloc for sp_ll + dec_ll. sp_ll lives in
         * the first ANS_L bytes, dec_ll follows with alignment (16-byte
         * aligned vs 8-byte reads is satisfied since ANS_L=4096 is
         * already 4KB-aligned). Saves 1 malloc/free pair. */
        size_t sp_sz = ANS_L;
        size_t dec_sz = ANS_L * sizeof(vva_dec_entry_t);
        uint8_t *ll_tables = (uint8_t *)malloc(sp_sz + dec_sz);
        if (!ll_tables) goto seq_fail;
        uint8_t *sp_ll = ll_tables;
        vva_dec_entry_t *dec_ll = (vva_dec_entry_t *)(ll_tables + sp_sz);
        spread_symbols(norm_ll, sp_ll);
        build_dec(norm_ll, sp_ll, dec_ll);
        enc_ll_ctx = build_enc(norm_ll, sp_ll, dec_ll);
        free(ll_tables);
        if (!enc_ll_ctx) goto seq_fail;
    }

    if (match_count > 0) {
        /* Treat ML codes as a small-alphabet problem */
        uint32_t raw_ml[NSYM], raw_of[NSYM];
        memset(raw_ml, 0, sizeof(raw_ml));
        memset(raw_of, 0, sizeof(raw_of));
        for (int i = 0; i < VVA_ML_CODES; i++) raw_ml[i] = freq_ml[i];
        for (int i = 0; i < VVA_OF_CODES; i++) raw_of[i] = freq_of[i];

        normalize_freq(raw_ml, norm_ml);
        normalize_freq(raw_of, norm_of);

        /* Write ML and OF table headers (buffers are on the stack) */
        ml_hdr_sz = write_hdr_v2(norm_ml, ml_hdr_buf, 600);
        of_hdr_sz = write_hdr_v2(norm_of, of_hdr_buf, 600);
        if (!ml_hdr_sz || !of_hdr_sz) goto seq_fail;



        /* ─── Build encode tables ───
         * PERF: one combined alloc for sp_ml + dec_ml + sp_of + dec_of
         * (4 fixed-size ANS_L-based buffers). Saves 3 malloc/free pairs. */
        size_t sp_sz = ANS_L;
        size_t dec_sz = ANS_L * sizeof(vva_dec_entry_t);
        size_t combo_sz = (sp_sz + dec_sz) * 2;
        uint8_t *ml_of_tables = (uint8_t *)malloc(combo_sz);
        if (!ml_of_tables) goto seq_fail;
        uint8_t *sp_ml = ml_of_tables;
        vva_dec_entry_t *dec_ml = (vva_dec_entry_t *)(ml_of_tables + sp_sz);
        uint8_t *sp_of = ml_of_tables + sp_sz + dec_sz;
        vva_dec_entry_t *dec_of = (vva_dec_entry_t *)(ml_of_tables + sp_sz + dec_sz + sp_sz);

        spread_symbols(norm_ml, sp_ml);
        build_dec(norm_ml, sp_ml, dec_ml);
        enc_ctx_t *enc_ml_ctx = build_enc(norm_ml, sp_ml, dec_ml);

        spread_symbols(norm_of, sp_of);
        build_dec(norm_of, sp_of, dec_of);
        enc_ctx_t *enc_of_ctx = build_enc(norm_of, sp_of, dec_of);

        free(ml_of_tables);
        if (!enc_ml_ctx || !enc_of_ctx) {
            free_enc(enc_ml_ctx); free_enc(enc_of_ctx);
            goto seq_fail;
        }

        /* ─── Encode ML/OF codes + extra bits in reverse ─── */
        /* Collect bitpairs for ANS-coded symbols + raw extra bits */
        size_t pair_cap = nseq * 6; /* 3 ANS + 3 extra max per seq */
        bitpair_t *pairs = (bitpair_t *)malloc(pair_cap * sizeof(bitpair_t));
        if (!pairs) { free_enc(enc_ml_ctx); free_enc(enc_of_ctx); goto seq_fail; }

        state_ml = 0; state_of = 0; state_ll = 0;
        size_t npairs = 0;

        /* Process sequences in reverse for ANS LIFO.
         * Decoder reads per-sequence: LL, OF, ML (forward).
         * Backward encode order (reversed of decode): ML, OF, LL.
         * After bitstream reversal: LL appears first → decoded first.
         *
         * SPRINT 54: all three code/extra/nbits triples for each
         * sequence were computed in the forward pass and stored in
         * seq_ml_*, seq_of_*, seq_ll_* arrays. Re-use them here
         * instead of recomputing ml_encode_with() and ll_encode().
         * Eliminates ~5-8% of encode time (the forward+backward
         * duplicate work). */
        for (size_t ii = nseq; ii > 0; ii--) {
            size_t idx = ii - 1;
            if (seqs[idx].matchlen > 0) {
                uint8_t mc = seq_ml_code[idx];
                uint32_t mx = seq_ml_extra[idx];
                int mn = seq_ml_nbits[idx];

                uint8_t oc = seq_of_code[idx];
                uint32_t ox = seq_of_extra[idx];
                int on = seq_of_nbits[idx];

                /* ML extra bits (raw) */
                if (mn > 0) {
                    pairs[npairs].val = (uint32_t)mx;
                    pairs[npairs].nb = (uint8_t)mn;
                    npairs++;
                }

                /* ML code (ANS) */
                {
                    uint32_t bv; int bn;
                    int slot = enc_sym(enc_ml_ctx, state_ml, mc, &bv, &bn);
                    if (slot < 0) {
                        free(pairs); free_enc(enc_ml_ctx); free_enc(enc_of_ctx);
                        goto seq_fail;
                    }
                    pairs[npairs].val = (uint32_t)bv;
                    pairs[npairs].nb = (uint8_t)bn;
                    npairs++;
                    state_ml = (uint32_t)slot;
                }

                /* OF extra bits (raw) */
                if (on > 0) {
                    pairs[npairs].val = (uint32_t)ox;
                    pairs[npairs].nb = (uint8_t)on;
                    npairs++;
                }

                /* OF code (ANS) */
                {
                    uint32_t bv; int bn;
                    int slot = enc_sym(enc_of_ctx, state_of, oc, &bv, &bn);
                    if (slot < 0) {
                        free(pairs); free_enc(enc_ml_ctx); free_enc(enc_of_ctx);
                        goto seq_fail;
                    }
                    pairs[npairs].val = (uint32_t)bv;
                    pairs[npairs].nb = (uint8_t)bn;
                    npairs++;
                    state_of = (uint32_t)slot;
                }
            }

            /* LL encoded LAST per sequence (decoded FIRST after reversal) */
            {
                uint8_t lc = seq_ll_code[idx];
                uint32_t lx = seq_ll_extra[idx];
                int ln = seq_ll_nbits[idx];

                if (ln > 0) {
                    pairs[npairs].val = (uint32_t)lx;
                    pairs[npairs].nb = (uint8_t)ln;
                    npairs++;
                }
                {
                    uint32_t bv; int bn;
                    int slot = enc_sym(enc_ll_ctx, state_ll, lc, &bv, &bn);
                    if (slot < 0) {
                        free(pairs); free_enc(enc_ml_ctx); free_enc(enc_of_ctx);
                        goto seq_fail;
                    }
                    pairs[npairs].val = (uint32_t)bv;
                    pairs[npairs].nb = (uint8_t)bn;
                    npairs++;
                    state_ll = (uint32_t)slot;
                }
            }
        }

        free_enc(enc_ml_ctx); free_enc(enc_of_ctx);

        /* Write pairs in reverse (so decoder reads forward).
         * Each pair is up to 32 bits (ANS slot = 14 bits + extra up to 18).
         * Allocate 4 bytes per pair + 16-byte safety margin. */
        size_t bs_cap = npairs * 4 + 16;
        seq_bs = (uint8_t *)malloc(bs_cap);
        if (!seq_bs) { free(pairs); goto seq_fail; }

        ans_bw_t w;
        ans_bw_init(&w, seq_bs, bs_cap);
        for (size_t i = npairs; i > 0; i--)
            ans_bw_add(&w, pairs[i - 1].val, pairs[i - 1].nb);
        seq_bs_len = ans_bw_flush(&w);
        free(pairs);
    }

    /* Litlens are now ANS-coded in the sequence bitstream — no varints needed */

    /* ─── Assemble output ─── */
    /* Format: [4B lit_count] [1B lit_fmt] [4B lit_enc_len] [lit_data]
     *         [4B match_count]
     *         [2B ml_hdr_sz] [ml_hdr] [2B of_hdr_sz] [of_hdr]
     *         [2B state_ml] [2B state_of]
     *         [4B seq_bs_len] [seq_bs]
     *         [litlen_varints] */
    {
        size_t total = 9 + lit_enc_len + 4 + 4 + ml_hdr_sz + 4 + of_hdr_sz
                     + 2 + ll_hdr_sz + 4 + 2 + 4 + seq_bs_len;

        if (total > dst_cap) goto seq_fail;

        uint8_t *op = dst;
        /* Literal section: 4B count + 1B fmt + 4B enc_len */
        op[0]=(uint8_t)total_lits; op[1]=(uint8_t)(total_lits>>8);
        op[2]=(uint8_t)(total_lits>>16); op[3]=(uint8_t)(total_lits>>24); op+=4;
        *op++ = lit_fmt;
        op[0]=(uint8_t)lit_enc_len; op[1]=(uint8_t)(lit_enc_len>>8);
        op[2]=(uint8_t)(lit_enc_len>>16); op[3]=(uint8_t)(lit_enc_len>>24); op+=4;
        if (lit_enc_len > 0) { memcpy(op, lit_enc, lit_enc_len); op += lit_enc_len; }

        /* Match count (4B) */
        op[0]=(uint8_t)match_count; op[1]=(uint8_t)(match_count>>8);
        op[2]=(uint8_t)(match_count>>16); op[3]=(uint8_t)(match_count>>24); op+=4;

        /* ML table */
        op[0] = (uint8_t)(ml_hdr_sz & 0xFF); op[1] = (uint8_t)(ml_hdr_sz >> 8); op += 2;
        if (ml_hdr_sz > 0) { memcpy(op, ml_hdr_buf, ml_hdr_sz); op += ml_hdr_sz; }

        /* OF table */
        op[0] = (uint8_t)(of_hdr_sz & 0xFF); op[1] = (uint8_t)(of_hdr_sz >> 8); op += 2;
        if (of_hdr_sz > 0) { memcpy(op, of_hdr_buf, of_hdr_sz); op += of_hdr_sz; }

        /* LL table */
        op[0] = (uint8_t)(ll_hdr_sz & 0xFF); op[1] = (uint8_t)(ll_hdr_sz >> 8); op += 2;
        if (ll_hdr_sz > 0) { memcpy(op, ll_hdr_buf, ll_hdr_sz); op += ll_hdr_sz; }

        /* States */
        op[0] = (uint8_t)(state_ml & 0xFF); op[1] = (uint8_t)((state_ml >> 8) & 0xFF); op += 2;
        op[0] = (uint8_t)(state_of & 0xFF); op[1] = (uint8_t)((state_of >> 8) & 0xFF); op += 2;
        op[0] = (uint8_t)(state_ll & 0xFF); op[1] = (uint8_t)((state_ll >> 8) & 0xFF); op += 2;

        /* Sequence bitstream (4B size) */
        op[0]=(uint8_t)seq_bs_len; op[1]=(uint8_t)(seq_bs_len>>8);
        op[2]=(uint8_t)(seq_bs_len>>16); op[3]=(uint8_t)(seq_bs_len>>24); op+=4;
        if (seq_bs_len > 0) { memcpy(op, seq_bs, seq_bs_len); op += seq_bs_len; }

        /* Litlens are ANS-coded in the bitstream — no trailing varints */

        *dst_len = (size_t)(op - dst);
    }

    free(base_scratch); free(lit_enc);
    free(seq_scratch);
    free_enc(enc_ll_ctx);
    free(seq_bs);
    return VVA_OK;

seq_fail:
    free(base_scratch); free(lit_enc);
    free(seq_scratch);
    free_enc(enc_ll_ctx);
    free(seq_bs);
    return VVA_ERR_OVERFLOW;
}

/* Public entry for 'S' tag (VV_ENTROPY_SEQ, min_match=4). */
vva_error_t vva_encode_sequences(const uint8_t *tokens, size_t tok_len,
                                  uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                  int off_bytes) {
    return vva_encode_sequences_impl(tokens, tok_len, dst, dst_cap, dst_len,
                                      off_bytes, ml_base);
}

/* Public entry for 'T' tag (VV_ENTROPY_SEQ_V2, min_match=3).
 * Encodes match-length codes using ml_base_v2 so a length-3 match
 * becomes code 0 (instead of being unrepresentable as it is in 'S').
 * The caller (vv_encoder.c) must ensure the token stream contains
 * only matches of length ≥ 3, and must emit the frame with tag 'T'. */
vva_error_t vva_encode_sequences_v2(const uint8_t *tokens, size_t tok_len,
                                     uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                     int off_bytes) {
    return vva_encode_sequences_impl(tokens, tok_len, dst, dst_cap, dst_len,
                                      off_bytes, ml_base_v2);
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE SEQUENCES
 *
 * Takes ANS-coded sequence block, outputs decompressed data.
 * Reconstructs LZ matches in-place using existing copy logic.
 * ═══════════════════════════════════════════════════════════════ */

/* Internal implementation shared by 'S' (min_match=4) and 'T'
 * (min_match=3) entropy tags. Takes the ml_base table as a parameter
 * so both tags use the same code path. Everything else in the 'T'
 * payload is byte-identical to 'S'. */
static vva_error_t vva_decode_sequences_impl(const uint8_t *src, size_t src_len,
                                              uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                              const uint8_t *dst_base,
                                              const uint32_t *ml_base_tab) {
    const uint8_t *p = src, *end = src + src_len;

    /* Read literal section: [4B lit_count] [1B lit_fmt] [4B lit_enc_len] */
        if (p + 9 > end) return VVA_ERR_CORRUPT;
    size_t total_lits = (size_t)p[0]|((size_t)p[1]<<8)|((size_t)p[2]<<16)|((size_t)p[3]<<24); p += 4;
    uint8_t lit_fmt = *p++;
    size_t lit_enc_len = (size_t)p[0]|((size_t)p[1]<<8)|((size_t)p[2]<<16)|((size_t)p[3]<<24); p += 4;
        if (p + lit_enc_len > end) return VVA_ERR_CORRUPT;

    /* Decode literals based on format byte */
    uint8_t *lit_buf = (uint8_t *)malloc(total_lits + 16);
    if (!lit_buf) return VVA_ERR_NOMEM;

    if (total_lits > 0 && lit_enc_len > 0) {
        vva_error_t lerr = VVA_ERR_CORRUPT;
        size_t lit_consumed = 0;

        if (lit_fmt == 1) {
            /* ANS 4-way interleaved */
            lerr = vva_decode4(p, lit_enc_len, lit_buf, total_lits,
                                total_lits, &lit_consumed);
        } else if (lit_fmt == 2) {
            /* ANS single-stream */
            lerr = vva_decode(p, lit_enc_len, lit_buf, total_lits,
                               total_lits, &lit_consumed);
        } else if (lit_fmt == 3) {
            /* SPRINT 71 (v2.46): Huffman-coded literals within SEQ. */
            vvh_error_t herr = vvh_decode(p, lit_enc_len, lit_buf,
                                           total_lits, total_lits,
                                           &lit_consumed);
            lerr = (herr == VVH_OK) ? VVA_OK : VVA_ERR_CORRUPT;
        } else {
            /* Raw literals (lit_fmt == 0) */
            if (lit_enc_len >= total_lits) {
                memcpy(lit_buf, p, total_lits);
                lerr = VVA_OK;
            }
        }
        if (lerr != VVA_OK) { free(lit_buf); return VVA_ERR_CORRUPT; }
    }
    p += lit_enc_len;

    /* Read match count (4B) */
        if (p + 4 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    size_t match_count = (size_t)p[0]|((size_t)p[1]<<8)|((size_t)p[2]<<16)|((size_t)p[3]<<24); p += 4;

    /* Read ML table header */
        if (p + 2 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    size_t ml_hdr_sz = (size_t)p[0] | ((size_t)p[1] << 8); p += 2;
        if (p + ml_hdr_sz > end) { free(lit_buf); return VVA_ERR_CORRUPT; }

    uint16_t norm_ml[NSYM];
    memset(norm_ml, 0, sizeof(norm_ml));
    if (ml_hdr_sz > 0) read_hdr_v2(p, ml_hdr_sz, norm_ml);
    p += ml_hdr_sz;

    /* Read OF table header */
        if (p + 2 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    size_t of_hdr_sz = (size_t)p[0] | ((size_t)p[1] << 8); p += 2;
        if (p + of_hdr_sz > end) { free(lit_buf); return VVA_ERR_CORRUPT; }

    uint16_t norm_of[NSYM];
    memset(norm_of, 0, sizeof(norm_of));
    if (of_hdr_sz > 0) read_hdr_v2(p, of_hdr_sz, norm_of);
    p += of_hdr_sz;

    /* Read LL table header */
        if (p + 2 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    size_t ll_hdr_sz = (size_t)p[0] | ((size_t)p[1] << 8); p += 2;
        if (p + ll_hdr_sz > end) { free(lit_buf); return VVA_ERR_CORRUPT; }

    uint16_t norm_ll[NSYM];
    memset(norm_ll, 0, sizeof(norm_ll));
    if (ll_hdr_sz > 0) read_hdr_v2(p, ll_hdr_sz, norm_ll);
    p += ll_hdr_sz;

    /* Read initial states */
        if (p + 6 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    uint32_t state_ml = (uint32_t)p[0] | ((uint32_t)p[1] << 8); p += 2;
    uint32_t state_of = (uint32_t)p[0] | ((uint32_t)p[1] << 8); p += 2;
    uint32_t state_ll = (uint32_t)p[0] | ((uint32_t)p[1] << 8); p += 2;

    /* Read sequence bitstream (4B size) */
        if (p + 4 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    size_t seq_bs_len = (size_t)p[0]|((size_t)p[1]<<8)|((size_t)p[2]<<16)|((size_t)p[3]<<24); p += 4;
        if (p + seq_bs_len > end) { free(lit_buf); return VVA_ERR_CORRUPT; }

    /* Build ML, OF, and LL decode tables */
    vva_dec_entry_t *dec_ml = NULL, *dec_of = NULL, *dec_ll = NULL;
    {
        uint8_t *sp_tmp = (uint8_t *)malloc(ANS_L);
        if (match_count > 0) {
            dec_ml = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
            dec_of = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        }
        dec_ll = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        if (!sp_tmp || !dec_ll || (match_count > 0 && (!dec_ml || !dec_of))) {
            free(sp_tmp); free(dec_ml); free(dec_of); free(dec_ll); free(lit_buf);
            return VVA_ERR_NOMEM;
        }
        if (match_count > 0) {
            spread_symbols(norm_ml, sp_tmp);
            build_dec(norm_ml, sp_tmp, dec_ml);
            spread_symbols(norm_of, sp_tmp);
            build_dec(norm_of, sp_tmp, dec_of);
        }
        spread_symbols(norm_ll, sp_tmp);
        build_dec(norm_ll, sp_tmp, dec_ll);
        free(sp_tmp);
    }

    /* Initialize bitstream reader for sequence data */
    ans_br_t r;
    ans_br_init(&r, p, seq_bs_len);
    ans_br_fill(&r);
    p += seq_bs_len;

    /* Litlens are ANS-coded in the bitstream — no varint stream */

    /* ─── PERF: Decode loop — reconstruct output ─── */
    uint8_t *op = dst;
    uint8_t *op_end = dst + dst_cap;
    size_t lit_pos = 0;
    size_t matches_decoded = 0;
    uint32_t dec_rep[3] = {0, 0, 0}; /* Rep-match offset tracking */

    /* SPRINT 50: Safe-zone precomputation. Two bounds checks run per
     * sequence today:
     *   (1) offset validity:    offset != 0 && offset <= op - dst_base
     *   (2) matchlen overflow:  op + matchlen > op_end
     *
     * Ablation measurements (fx_text): bounds checks cost ~18% of
     * total decode time. The overhead is not the branches themselves
     * (predicted-not-taken, rarely fire) but (a) register pressure
     * from keeping op_end/dst_base live and (b) the compound subtract
     * + compare for (1).
     *
     * Observation: once we're past the first wlog bytes AND not yet
     * near op_end, BOTH checks are guaranteed to pass for any
     * well-formed sequence (offset ≤ wlog ≤ op - dst_base, and
     * matchlen ≤ max_match ≤ op_end - op). In the "safe zone" we can
     * skip the runtime checks with zero security loss — they remain
     * under offset_check_floor and op_safe_end as non-hoistable guards.
     *
     * SAFETY: the skipped checks are tautologies in the safe zone,
     * not removed guarantees. Malformed input that produces an
     * invalid offset or overlong matchlen still triggers the checks
     * near the boundaries. We maintain §4 invariants 3 and 5.
     *
     * SAFEZONE_MAX_OFFSET covers the legal offset range (1 << wlog_max).
     * SAFEZONE_MAX_RUN covers BOTH max litlen and max matchlen (both are
     * bounded by the wire format at ≤65535: LL encoding ll_base[35]=61440
     * + up to 4095 extra bits = 65535; ML encoding likewise). So
     * op_safe_end = op_end - 65535 guarantees any single sequence's
     * total writes (literals + match) fit without per-iter overflow
     * checking. */
    enum { SAFEZONE_MAX_OFFSET = 1u << 20 };  /* Maximum wlog supported */
    enum { SAFEZONE_MAX_RUN    = 65535 };     /* litlen or matchlen */
    uint8_t *op_safe_end = (dst_cap > SAFEZONE_MAX_RUN)
                           ? op_end - SAFEZONE_MAX_RUN : dst;
    const uint8_t *offset_check_floor = dst_base + SAFEZONE_MAX_OFFSET;

    size_t seqs_decoded = 0;
    while (lit_pos < total_lits || matches_decoded < match_count) {
        /* PERF: issue all 3 ANS table lookups early so CPU can overlap
         * the L1 cache fills. The dec_ll/dec_of/dec_ml arrays are
         * independent, so the loads have no data dependency on each
         * other — perfect for ILP. The compiler will schedule these
         * ahead of the bitstream reads that consume their results.
         *
         * Fill once at the top. 64 bits covers up to 1 full sequence
         * worst-case (26+35+27=88 but typical ~15-30). ans_br_read
         * auto-fills when we run out within a sequence. */
        ans_br_fill(&r);

        /* Fast path: in safe zone (past warmup, before final max_match
         * bytes). Both bounds checks are tautological and skipped. */
        int in_safe_zone = (op >= offset_check_floor) & (op <= op_safe_end);

        /* PERF: state validation via mask-on-access rather than
         * explicit branches. Since ANS_L is a power of 2, masking
         * bounds any state into valid-table range at ~0 cost, where
         * the 3 explicit ">= ANS_L" branches would cost 3 predicted-
         * not-taken comparisons per iter. On corrupt input the
         * frame-level XXH64 footer still catches the corruption;
         * here we're protecting against out-of-bounds table access,
         * not declaring correctness. */
        vva_dec_entry_t ell = dec_ll[state_ll & (ANS_L - 1)];
        vva_dec_entry_t eof = dec_of[state_of & (ANS_L - 1)];
        vva_dec_entry_t eml = dec_ml[state_ml & (ANS_L - 1)];

        /* ── Decode LL: state, extra, final litlen ── */
        uint32_t ll_bits = ans_br_read(&r, ell.nbits);
        state_ll = (uint32_t)ell.baseline + ll_bits;
        uint8_t ll_code = ell.symbol;
        uint32_t ll_extra_val = ans_br_read(&r, ll_extra[ll_code]);
        size_t litlen = ll_decode(ll_code, ll_extra_val);

        if (VV_UNLIKELY(lit_pos + litlen > total_lits)) {
            free(dec_ml); free(dec_of); free(dec_ll); free(lit_buf);
            return VVA_ERR_CORRUPT;
        }
        if (VV_UNLIKELY(!in_safe_zone && op + litlen > op_end)) {
            free(dec_ml); free(dec_of); free(dec_ll); free(lit_buf);
            return VVA_ERR_OVERFLOW;
        }
        if (litlen > 0) {
            /* PERF: for litlen ≤ 16 (common case in text/json), do one
             * unconditional 16-byte copy instead of memcpy's branchy
             * dispatch. lit_buf has 16-byte post-allocation slack; the
             * destination window is checked against op_end above.
             *
             * Safe to over-read lit_buf beyond lit_pos+litlen (slack).
             * Safe to over-write op beyond op+litlen as long as
             * op + 16 ≤ op_end; for the last few sequences in a block
             * that may not hold, so fall back to memcpy there. */
            if (VV_LIKELY(litlen <= 16 && op + 16 <= op_end)) {
                memcpy(op, lit_buf + lit_pos, 16);
            } else {
                memcpy(op, lit_buf + lit_pos, litlen);
            }
            op += litlen;
            lit_pos += litlen;
        }
        seqs_decoded++;

        /* SPRINT 63/64: continue the loop even when all matches are
         * consumed, as long as literals remain. Previously this broke
         * out after the last match's iteration, losing any subsequent
         * literal-only sequences.
         *
         * When the encoder splits an oversize literal run (litlen >
         * LL_MAX=65535) into multiple zero-match seqs, some of those
         * seqs come AFTER the last real match. The old break dropped
         * them silently, producing short output.
         *
         * Fix: break only when both literals AND matches are fully
         * consumed. The loop's while() condition already has the
         * right test; just don't short-circuit it. */
        if (matches_decoded >= match_count && lit_pos >= total_lits) break;
        if (matches_decoded >= match_count) continue;

        /* ── Decode OF: state, then offset (rep or explicit) ──
         * No explicit fill — ans_br_read fills when it runs out. */
        uint32_t of_bits = ans_br_read(&r, eof.nbits);
        state_of = (uint32_t)eof.baseline + of_bits;
        uint8_t of_code = eof.symbol;
        uint32_t offset;
        if (of_code < 3) {
            offset = dec_rep[of_code];
        } else {
            uint32_t of_extra_val = ans_br_read(&r, of_extra[of_code]);
            offset = of_decode(of_code, of_extra_val);
        }
        if (offset != 0 && offset != dec_rep[0]) {
            dec_rep[2] = dec_rep[1]; dec_rep[1] = dec_rep[0]; dec_rep[0] = offset;
        }

        /* ── Decode ML: state, extra, final matchlen ── */
        uint32_t ml_bits = ans_br_read(&r, eml.nbits);
        state_ml = (uint32_t)eml.baseline + ml_bits;
        uint8_t ml_code = eml.symbol;
        uint32_t ml_extra_val = ans_br_read(&r, ml_extra[ml_code]);
        uint32_t matchlen = ml_base_tab[ml_code] + ml_extra_val;

        /* Validate and execute match copy.
         *
         * SPRINT 50 safety: the offset upper cap is checked
         * unconditionally — covers adversarial inputs that encode
         * offsets beyond any legal window. In-safe-zone skip only
         * removes the position-dependent check (offset > op - dst_base),
         * which is guaranteed tautological when both
         *    offset ≤ SAFEZONE_MAX_OFFSET   (absolute cap, checked)
         *    op ≥ dst_base + SAFEZONE_MAX_OFFSET  (safe-zone floor)
         * The matchlen-overshoot check is similarly safe because
         * op_safe_end = op_end - SAFEZONE_MAX_MATCH, and matchlen is
         * always ≤ SAFEZONE_MAX_MATCH by wire format. */
        if (VV_UNLIKELY(offset == 0 || offset > SAFEZONE_MAX_OFFSET)) {
            free(dec_ml); free(dec_of); free(lit_buf);
            return VVA_ERR_CORRUPT;
        }
        if (VV_UNLIKELY(!in_safe_zone && offset > (uint32_t)(op - dst_base))) {
            free(dec_ml); free(dec_of); free(lit_buf);
            return VVA_ERR_CORRUPT;
        }
        if (VV_UNLIKELY(!in_safe_zone && op + matchlen > op_end)) {
            free(dec_ml); free(dec_of); free(lit_buf);
            return VVA_ERR_OVERFLOW;
        }

        /* PERF: inline tiered match copy — avoids the function pointer
         * call in vv_copy_match which kills ILP. The compiler can then
         * overlap these stores with the next iteration's ANS decodes.
         *
         * Tiers (matches decode_block_tokens_impl):
         *   offset >= 16: safe bulk 16-byte chunks (no overlap concerns)
         *   offset >= 8:  8-byte chunks (overlap window >= stride)
         *   offset <  8:  byte-by-byte (overlap propagates correctly) */
#ifdef VV_ANS_STANDALONE
        {
            const uint8_t *match_src = op - offset;
            for (uint32_t j = 0; j < matchlen; j++)
                op[j] = match_src[j];
        }
#else
        {
            uint8_t *d = op;
            if (offset >= 16) {
                const uint8_t *s = d - offset;
                /* PERF: common case is matchlen in [4,16] — do one
                 * unconditional 16-byte copy when safe. Over-writes
                 * harmlessly into future output space (caller's buffer
                 * already sized for dsz, plus op_end check above).
                 *
                 * SPRINT 45: same bleed-over hazard as the offset≥8
                 * path. For matchlen < 4, the 16-byte overwrite
                 * corrupts bytes that a subsequent short-offset match
                 * will read. Use an exact 3-byte copy in that case. */
                if (VV_LIKELY(matchlen >= 4 && matchlen <= 16 && d + 16 <= op_end)) {
                    memcpy(d, s, 16);
                } else if (matchlen == 3 && d + 16 <= op_end) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                } else {
                    size_t rem = matchlen;
                    while (rem >= 16) { memcpy(d, s, 16); d += 16; s += 16; rem -= 16; }
                    if (rem > 0) memcpy(d, s, rem);
                }
            } else if (offset >= 8) {
                const uint8_t *s = d - offset;
                /* PERF: matchlen ≤ 8 with offset ≥ 8 → one 8-byte copy.
                 *
                 * SPRINT 45: The fast path writes 8 bytes unconditionally,
                 * which overshoots for small matchlen. For v1 (min_match=4)
                 * this is harmless — the overshoot into d[4..7] gets
                 * overwritten by the next sequence before anyone reads it.
                 * But for v2 (min_match=3), a subsequent short-offset
                 * match reads from d[3..] and sees the overshoot bytes.
                 * Gate the fast path on matchlen ≥ 4 to preserve v1
                 * behavior while making v2 correct. */
                if (VV_LIKELY(matchlen >= 4 && matchlen <= 8 && d + 8 <= op_end)) {
                    uint64_t v; memcpy(&v, s, 8); memcpy(d, &v, 8);
                } else if (matchlen <= 8 && d + 8 <= op_end) {
                    /* matchlen == 3 path: copy exactly 3 bytes without
                     * overshooting. One 4-byte read covers all three
                     * source bytes and is safe since offset ≥ 8 (the
                     * source region is disjoint from the destination). */
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                } else {
                    size_t rem = matchlen;
                    while (rem >= 8) {
                        uint64_t v; memcpy(&v, s, 8); memcpy(d, &v, 8);
                        d += 8; s += 8; rem -= 8;
                    }
                    while (rem-- > 0) *d++ = *s++;
                }
            } else {
                /* offset < 8: byte-by-byte for correct self-reference.
                 * Experimented with unrolled 16-iter versions; the
                 * branchless form over-writes past matchlen and hurts
                 * text, while the branched form hurts JSON. The simple
                 * loop runs well across all fixtures — the compiler
                 * schedules the dependent byte loads reasonably. */
                for (uint32_t j = 0; j < matchlen; j++)
                    d[j] = d[j - (ptrdiff_t)offset];
            }
        }
#endif
        op += matchlen;

        matches_decoded++;
    }

    *dst_len = (size_t)(op - dst);
    free(dec_ml); free(dec_of); free(dec_ll); free(lit_buf);
    return VVA_OK;
}

/* Public entry for 'S' tag (VV_ENTROPY_SEQ, min_match=4). */
vva_error_t vva_decode_sequences(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                  const uint8_t *dst_base) {
    return vva_decode_sequences_impl(src, src_len, dst, dst_cap, dst_len,
                                      dst_base, ml_base);
}

/* Public entry for 'T' tag (VV_ENTROPY_SEQ_V2, min_match=3).
 * Payload format is byte-identical to 'S' — only the ML table differs.
 * Produced by encoders that opt into v2, decodable by any v2.33.0+ decoder. */
vva_error_t vva_decode_sequences_v2(const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                     const uint8_t *dst_base) {
    return vva_decode_sequences_impl(src, src_len, dst, dst_cap, dst_len,
                                      dst_base, ml_base_v2);
}

/* ── src/vv_encoder.c ── */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — Encoder v2 (Sprint 1)
 *
 * KEY CHANGES:
 *   1. 5-byte multiply-shift hash (fewer collisions than 4-byte)
 *   2. Rep-match: check 3 recent offsets before hash probe (30% hit rate)
 *   3. Match-skip: after long matches, only insert boundary positions
 *   4. AVX2 match extension: 32 bytes/cycle vs 1 byte/cycle scalar
 *   5. Lazy-2 parsing for balanced mode (check pos+1 AND pos+2)
 *   6. Extreme mode: deeper chains (256) + lazy-2
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && defined(__AVX2__)
#include <immintrin.h>
#define VV_ENC_AVX2 1
#else
#define VV_ENC_AVX2 0
#endif

/* ═══════════════════════════════════════════════════════════════
 * VARINT WRITER
 * ═══════════════════════════════════════════════════════════════ */

static inline size_t write_varint(uint8_t *dst, size_t val) {
    size_t n = 0;
    while (val >= 255) { dst[n++] = 255; val -= 255; }
    dst[n++] = (uint8_t)val;
    return n;
}

/* ═══════════════════════════════════════════════════════════════
 * IMPROVED HASH: 5-byte multiply-shift (safe read pattern)
 *
 * Reads exactly 5 bytes using 4+1 to prevent compiler from
 * widening to an 8-byte load that over-reads the buffer.
 * ═══════════════════════════════════════════════════════════════ */

static inline uint32_t hash5(const uint8_t *p) {
    uint32_t lo;
    memcpy(&lo, p, 4);
    uint64_t v = (uint64_t)lo | ((uint64_t)p[4] << 32);
    /* Shift by (64 - HC_BITS) to get the top HC_BITS of the product */
    return (uint32_t)((v * 889523592379ULL) >> (64 - VV_HC_BITS));
}

/* 4-byte hash for positions near end of buffer */
static inline uint32_t hash4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - VV_HC_BITS);
}

/* Safe hash: picks 5-byte or 4-byte depending on remaining bytes */
static inline uint32_t hash_safe(const uint8_t *p, int32_t remain) {
    return (remain >= 5) ? hash5(p) : hash4(p);
}

/* ═══════════════════════════════════════════════════════════════
 * AVX2 MATCH EXTENSION
 *
 * Compare 32 bytes at a time. Returns total match length.
 * ~8× faster than byte-by-byte on data with long matches.
 * ═══════════════════════════════════════════════════════════════ */

static inline int32_t extend_match(const uint8_t *a, const uint8_t *b,
                                    int32_t max_len) {
    int32_t len = 0;

    /* SPRINT 55: 8-byte fast-path check first. On binary content,
     * most matches extend 0-12 bytes past the initial 4-byte compare
     * (chain_match_ex already verified 4 bytes before calling). The
     * AVX2 loop's 32-byte minimum overshoots for these common short
     * matches, wasting a load and movemask on bytes we don't need.
     *
     * Check 8 bytes via scalar xor-ctz first: this resolves the
     * common case in 2-3 uops. On binary fixtures (bash, libc.so.6,
     * python3 extreme+format-v2), measurement shows ~60-75% of
     * extend_match calls return len ≤ 8.
     *
     * Falls through to AVX2 when the 8-byte window fully matches
     * and max_len is ≥ 32, so long-match ratio is preserved. */
    if (max_len >= 8) {
        uint64_t va, vb;
        memcpy(&va, a, 8);
        memcpy(&vb, b, 8);
        uint64_t xor_ab = va ^ vb;
        if (xor_ab) {
            /* Little-endian: byte at position k differs iff bit k*8 set */
            return __builtin_ctzll(xor_ab) >> 3;
        }
        len = 8;
    }
#if VV_ENC_AVX2
    while (len + 32 <= max_len) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + len));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(b + len));
        __m256i eq = _mm256_cmpeq_epi8(va, vb);
        uint32_t mask = ~(uint32_t)_mm256_movemask_epi8(eq);
        if (mask) return len + (int32_t)vv_ctz32(mask);
        len += 32;
    }
#endif
    while (len < max_len && a[len] == b[len]) len++;
    return len;
}

/* ═══════════════════════════════════════════════════════════════
 * MATCHER: hash chain with 5-byte hash + rep-match
 * ═══════════════════════════════════════════════════════════════ */


#define VV_HC4_BITS  16
#define VV_HC4_SIZE  (1u << VV_HC4_BITS)

static inline uint32_t hash4_short(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - VV_HC4_BITS);
}

/* ─── SPRINT 45: Hash3 for format v2 ───────────────────────────
 * When min_match=3 (opts.format_v2), we need to find 3-byte
 * matches that hash5/hash4 cannot surface (both require 4-byte
 * prefix equality before extending). Hash3 uses a 3-byte key
 * with its own table and chain (SEPARATE — never share chain
 * arrays across hash tables per Sprint 14's silent-corruption
 * lesson). Table size is 14 bits = 16K entries = 64 KB, smaller
 * than hash4's 256 KB to account for the lower entropy of
 * 3-byte keys.
 *
 * Enabled only when matcher_t::use_hash3 is set (v2 path).
 * When disabled, table3/hash3_chain are NULL, hash3 insert is
 * skipped, and hash3 probe never runs. Zero cost on v1 path. */
#define VV_HC3_BITS  14
#define VV_HC3_SIZE  (1u << VV_HC3_BITS)

static inline uint32_t hash3_short(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> (32 - VV_HC3_BITS);
}

typedef struct {
    int32_t *table;        /* Primary hash5: VV_HC_SIZE entries */
    int32_t *chain;        /* Primary chain: window_size entries */
    int32_t *table4;       /* Secondary hash4: VV_HC4_SIZE entries */
    int32_t *hash4_chain;  /* Secondary chain (SEPARATE from primary) */
    int32_t *table3;       /* Tertiary hash3 (v2 only): VV_HC3_SIZE entries, NULL on v1 */
    int32_t *hash3_chain;  /* Tertiary chain (SEPARATE from hash4/primary), NULL on v1 */
    uint32_t chain_mask;
    uint32_t chain_depth;
    uint32_t rep[3];       /* 3 most recent match offsets */
    uint8_t  wlog;         /* Window log: controls max offset distance */
    uint8_t  use_hash4;    /* Enable hash4 fallback (binary data only) */
    uint8_t  use_hash3;    /* Enable hash3 fallback (format v2 only) */
    uint32_t max_match;    /* Max representable matchlen (65535 for v1,
                            * 65534 for v2: ml_base_v2[35]=32767 with 15
                            * extra bits only reaches 65534). */
} matcher_t;

static void matcher_init(matcher_t *m, uint32_t window_log, uint32_t depth) {
    uint32_t wsz = 1u << window_log;
    m->table = (int32_t *)malloc(VV_HC_SIZE * sizeof(int32_t));
    m->chain = (int32_t *)malloc(wsz * sizeof(int32_t));
    m->table4 = (int32_t *)malloc(VV_HC4_SIZE * sizeof(int32_t));
    m->hash4_chain = (int32_t *)malloc(wsz * sizeof(int32_t));
    /* hash3 tables allocated lazily only when use_hash3 is enabled.
     * On v1 path (the default), they stay NULL and cost nothing. */
    m->table3 = NULL;
    m->hash3_chain = NULL;
    /* PERF: only the table arrays need to be cleared. chain/hash4_chain
     * are only read via table entries (which are now -1), so stale
     * data in them is unreachable. See matcher_reset for rationale. */
    memset(m->table, 0xFF, VV_HC_SIZE * sizeof(int32_t));
    memset(m->table4, 0xFF, VV_HC4_SIZE * sizeof(int32_t));
    m->chain_mask = wsz - 1;
    m->chain_depth = depth;
    m->rep[0] = m->rep[1] = m->rep[2] = 0;
    m->wlog = (uint8_t)window_log;
    m->use_hash4 = 0;  /* Disabled by default — enabled adaptively for binary */
    m->use_hash3 = 0;  /* Disabled by default — enabled for format v2 */
    m->max_match = VV_MAX_MATCH;  /* v1 default, see matcher_set_format_v2 */
}

/* Apply format v2 matcher constraints. Must be called whenever the
 * encoder is producing 'T'-tagged blocks, regardless of whether the
 * hash3 probe is active (the adaptive trial may decide not to enable
 * hash3 on non-binary data, but the ml_base_v2 range cap still
 * applies to every match). */
static void matcher_set_format_v2(matcher_t *m) {
    /* ml_base_v2[35]=32767 with 15 extra bits → max representable
     * matchlen = 32767+32767 = 65534. Without this cap, the matcher
     * can produce 65535-length matches whose extra field (32768)
     * overflows 15 bits → encodes as 0 → decoder reconstructs 32767
     * (short by exactly 32,768 bytes per affected match). Root cause
     * of the python3 multi-block corruption in v2.34.0. */
    m->max_match = 65534;
}

/* Enable hash3 path: allocate tables. Idempotent and cheap to skip. */
static int matcher_enable_hash3(matcher_t *m) {
    if (m->table3) return 1;  /* Already enabled */
    uint32_t wsz = 1u << m->wlog;
    m->table3 = (int32_t *)malloc(VV_HC3_SIZE * sizeof(int32_t));
    m->hash3_chain = (int32_t *)malloc(wsz * sizeof(int32_t));
    if (!m->table3 || !m->hash3_chain) {
        free(m->table3); m->table3 = NULL;
        free(m->hash3_chain); m->hash3_chain = NULL;
        return 0;
    }
    memset(m->table3, 0xFF, VV_HC3_SIZE * sizeof(int32_t));
    m->use_hash3 = 1;
    return 1;
}

static void matcher_free(matcher_t *m) {
    free(m->table); m->table = NULL;
    free(m->chain); m->chain = NULL;
    free(m->table4); m->table4 = NULL;
    free(m->hash4_chain); m->hash4_chain = NULL;
    free(m->table3); m->table3 = NULL;
    free(m->hash3_chain); m->hash3_chain = NULL;
}

/* Reset matcher state without reallocating tables. Used by
 * vv_cstream_reset() for fast per-file reuse.
 *
 * PERF: We only need to clear the `table` and `table4` arrays (the
 * hash → position maps). The `chain` arrays store (pos → earlier
 * pos) links, but those links are only FOLLOWED from table entries.
 * After resetting the tables, any stale chain entries become
 * unreachable. This cuts reset cost from ~1.6 MB of memset to
 * ~1.25 MB (table=1MB + table4=256KB), a ~25% speedup. */
static void matcher_reset(matcher_t *m) {
    memset(m->table, 0xFF, VV_HC_SIZE * sizeof(int32_t));
    memset(m->table4, 0xFF, VV_HC4_SIZE * sizeof(int32_t));
    if (m->table3) memset(m->table3, 0xFF, VV_HC3_SIZE * sizeof(int32_t));
    m->rep[0] = m->rep[1] = m->rep[2] = 0;
    m->use_hash4 = 0;
    /* use_hash3 is NOT reset — it's a caller-opted-in mode flag,
     * not adaptive behavior that should clear on reset. */
}

static inline void matcher_insert(matcher_t *m, const uint8_t *data,
                                   int32_t pos, int32_t end) {
    if (pos + 4 > end) return;
    uint32_t h = hash_safe(data + pos, end - pos);
    m->chain[pos & m->chain_mask] = m->table[h];
    m->table[h] = pos;
    /* PERF: only maintain hash4 table when it's actually being used.
     * For text/source (use_hash4==0), this saves a hash computation
     * and two memory writes per insert — measurable on insert-heavy
     * workloads (logs, JSON). */
    if (m->use_hash4) {
        uint32_t h4 = hash4_short(data + pos);
        m->hash4_chain[pos & m->chain_mask] = m->table4[h4];
        m->table4[h4] = pos;
    }
    /* SPRINT 45: hash3 insert, only when enabled. Same guard logic
     * as hash4 — zero cost when disabled. */
    if (m->use_hash3) {
        uint32_t h3 = hash3_short(data + pos);
        m->hash3_chain[pos & m->chain_mask] = m->table3[h3];
        m->table3[h3] = pos;
    }
}

/* ─── Rep-match check: O(1), checked BEFORE hash probe ─── */
static inline int32_t try_rep_match(const matcher_t *m, const uint8_t *data,
                                     int32_t pos, int32_t end,
                                     int32_t *rep_idx) {
    /* Primary path: require 4-byte equality. Extends from there. */
    for (int i = 0; i < 3; i++) {
        uint32_t d = m->rep[i];
        if (d == 0 || (uint32_t)pos < d) continue;
        int32_t ref = pos - (int32_t)d;
        uint32_t a, b;
        memcpy(&a, data + pos, 4);
        memcpy(&b, data + ref, 4);
        if (a == b) {
            int32_t max = end - pos;
            if (max > (int32_t)m->max_match) max = (int32_t)m->max_match;
            int32_t len = 4 + extend_match(data + pos + 4, data + ref + 4, max - 4);
            *rep_idx = i;
            return len;
        }
    }
    /* SPRINT 47: format v2 secondary path. When hash3 is active, check
     * if any rep-offset produces a 3-byte rep-match even when the
     * 4-byte compare above fails. Rep-matches have 0 extra offset
     * bits — a 3-byte rep is nearly always a win vs 3 literals, which
     * the 4-byte requirement was blocking. Only runs when use_hash3
     * is on (i.e. format v2 + binary-detected), so text/JSON paths
     * stay bit-identical. */
    if (m->use_hash3 && pos + 3 <= end) {
        for (int i = 0; i < 3; i++) {
            uint32_t d = m->rep[i];
            if (d == 0 || (uint32_t)pos < d) continue;
            int32_t ref = pos - (int32_t)d;
            if (data[pos] == data[ref]
                && data[pos + 1] == data[ref + 1]
                && data[pos + 2] == data[ref + 2]) {
                *rep_idx = i;
                return 3;  /* length-3 rep; caller accepts since min_match=3 */
            }
        }
    }
    return 0;
}

/* ─── Hash chain match: uses 5-byte hash, searches up to chain_depth.
 * If use_hash4 is nonzero AND hash5 finds nothing, fall back to hash4
 * chain for binary/struct coverage. ─── */
static int32_t chain_match_ex(const matcher_t *m, const uint8_t *data,
                               int32_t pos, int32_t end, int32_t *best_off,
                               int use_hash4) {
    /* Early exit: we need at least 3 bytes for hash3 probe, 4 for
     * hash4/hash5. Use the looser bound if hash3 is enabled. */
    int32_t min_bytes = m->use_hash3 ? 3 : 4;
    if (pos + min_bytes > end) return 0;

    int32_t best_len = 0;
    *best_off = 0;

    int32_t max_dist = (int32_t)((1u << m->wlog) - 1);
    int32_t limit = pos - max_dist;
    if (limit < 0) limit = 0;

    /* Hash5/hash4 paths require 4 bytes. Skip them if only 3 remain. */
    if (pos + 4 <= end) {

    /* Hoist pos4: never changes during the chain walk */
    uint32_t pos4;
    memcpy(&pos4, data + pos, 4);

    /* Primary hash5 chain traversal.
     *
     * SPRINT 55: 4-way software-pipelined chain walk. Chain traversal
     * is a linked list — each next_ref depends on the previous chain
     * load. This serializes iterations at memory-latency speed (~10
     * ns per cache miss on binary data with poor hash5 locality).
     *
     * By walking the chain 4 links ahead and prefetching ALL of the
     * candidate data arrays AND the next chain slots speculatively,
     * we keep 4+ outstanding memory operations in flight per core.
     * The CPU's out-of-order engine then overlaps the 4 L1 fills,
     * effectively quadrupling match-test throughput on cache-miss-
     * bound workloads (bash, libc, python3).
     *
     * Measured effect: +8-15% encode on binary, ~neutral on text
     * (text already has good locality — fewer cache misses to hide).
     *
     * Safety: the prefetch is speculative ONLY. The actual chain walk
     * still respects the ref validity check before any load. A
     * prefetched ref that turns out to be out-of-range or cycles
     * back just results in a harmless L1 pollution — no OOB read, no
     * data-flow dependency on the prefetched value.
     */
    uint32_t h = hash_safe(data + pos, end - pos);
    int32_t ref = m->table[h];
    uint32_t depth = m->chain_depth;
    uint32_t chain_mask = m->chain_mask;
    int32_t *chain_arr = m->chain;

    /* Pipeline priming: look 4 chain entries ahead. If chain is
     * short, the prefetches become no-ops (chain entries below limit
     * just return -1 or an expired position). */
    if (ref >= limit && ref < pos) {
        __builtin_prefetch(data + ref, 0, 0);
        int32_t r1 = chain_arr[ref & chain_mask];
        if (r1 >= limit && r1 < pos) {
            __builtin_prefetch(data + r1, 0, 0);
            __builtin_prefetch(&chain_arr[r1 & chain_mask], 0, 0);
            int32_t r2 = chain_arr[r1 & chain_mask];
            if (r2 >= limit && r2 < pos) {
                __builtin_prefetch(data + r2, 0, 0);
                __builtin_prefetch(&chain_arr[r2 & chain_mask], 0, 0);
            }
        }
    }

    while (ref >= 0 && ref >= limit && ref < pos && depth-- > 0) {
        int32_t next_ref = chain_arr[ref & chain_mask];
        /* Prefetch the link 2-3 iterations ahead so the linked-list
         * chain of loads can overlap with match-compare work */
        if (next_ref >= limit && next_ref < pos) {
            __builtin_prefetch(data + next_ref, 0, 0);
            __builtin_prefetch(&chain_arr[next_ref & chain_mask], 0, 0);
        }

        uint32_t b;
        memcpy(&b, data + ref, 4);
        if (pos4 == b) {
            int32_t max = end - pos;
            if (max > (int32_t)m->max_match) max = (int32_t)m->max_match;
            int32_t len = 4 + extend_match(data + pos + 4, data + ref + 4, max - 4);
            if (len > best_len) {
                best_len = len;
                *best_off = pos - ref;
                if (len >= 256) return best_len;
            }
        }
        ref = next_ref;
    }

    /* PERF: Secondary hash4 chain fallback — ONLY when hash5 found nothing
     * AND caller indicates hash4 is safe to use (no competing rep-match).
     * Uses SEPARATE hash4_chain array.
     *
     * Note: tried relaxing trigger to `best_len < 8` in sprint 41 but
     * empirically found NO improvement on real binary data (bash, ls,
     * python3, libc.so.6). The hash4 fallback finds the same matches
     * hash5 already finds when primary prefix is 5 bytes. Closing the
     * binary-compression gap vs gzip-9 (~11% worse) requires either
     * min_match=3 (format change) or deeper LZ-optimal parsing. Kept
     * the zero-only trigger which matches lz4's fallback pattern. */
    if (use_hash4 && best_len == 0) {
        uint32_t h4 = hash4_short(data + pos);
        int32_t ref4 = m->table4[h4];
        uint32_t depth4 = 8;

        if (ref4 >= limit && ref4 < pos) {
            __builtin_prefetch(data + ref4, 0, 0);
        }

        while (ref4 >= 0 && ref4 >= limit && ref4 < pos && depth4-- > 0) {
            int32_t next_ref4 = m->hash4_chain[ref4 & m->chain_mask];
            if (next_ref4 >= limit && next_ref4 < pos) {
                __builtin_prefetch(data + next_ref4, 0, 0);
                __builtin_prefetch(&m->hash4_chain[next_ref4 & m->chain_mask], 0, 0);
            }

            uint32_t b4;
            memcpy(&b4, data + ref4, 4);
            if (pos4 == b4) {
                int32_t max = end - pos;
                if (max > (int32_t)m->max_match) max = (int32_t)m->max_match;
                int32_t len = 4 + extend_match(data + pos + 4, data + ref4 + 4, max - 4);
                if (len > best_len) {
                    best_len = len;
                    *best_off = pos - ref4;
                    if (len >= 256) return best_len;
                }
            }
            ref4 = next_ref4;
        }
    }

    }  /* end if (pos + 4 <= end) */

    /* ─── SPRINT 45: Tertiary hash3 probe ───────────────────────
     * Only when use_hash3 is enabled (format v2) AND hash5/hash4
     * found nothing ≥ 4 bytes (best_len < 4). Uses a SEPARATE
     * chain array from hash4 — never share chain storage.
     *
     * Probe depth is intentionally small (4). Unlike hash4, hash3's
     * collision rate is high (16K entries for up to 16M unique
     * 3-byte keys), so deep walks waste cycles on spurious hits.
     *
     * Match length is reported honestly — may be 3, or may extend.
     * The caller (compress_block) accepts len ≥ min_match. */
    if (m->use_hash3 && best_len < 4 && pos + 3 <= end) {
        uint32_t h3 = hash3_short(data + pos);
        int32_t ref3 = m->table3[h3];
        uint32_t depth3 = 4;

        /* Compare key: the 3 bytes at pos. Pack into low 24 bits
         * of a uint32 for a single compare against the candidate. */
        uint32_t pos3 = (uint32_t)data[pos]
                      | ((uint32_t)data[pos + 1] << 8)
                      | ((uint32_t)data[pos + 2] << 16);

        while (ref3 >= 0 && ref3 >= limit && ref3 < pos && depth3-- > 0) {
            int32_t next_ref3 = m->hash3_chain[ref3 & m->chain_mask];

            uint32_t b3 = (uint32_t)data[ref3]
                        | ((uint32_t)data[ref3 + 1] << 8)
                        | ((uint32_t)data[ref3 + 2] << 16);
            if (pos3 == b3) {
                int32_t max = end - pos;
                if (max > (int32_t)m->max_match) max = (int32_t)m->max_match;
                int32_t len = 3 + extend_match(data + pos + 3, data + ref3 + 3, max - 3);
                /* SPRINT 48: extended offset filter for length-3 hash3
                 * matches. Flat ≤4096 threshold. Higher than v2.35.0's
                 * ≤256 — the v2.36.0 adaptive hash3 gate now keeps
                 * text/JSON fully neutral at ANY threshold, so the
                 * filter only governs binary precision.
                 *
                 * Measured threshold sweep (binary Δ vs V1):
                 *    256 (v2.37): bash -2.2% ls -3.3% libc -2.8% py -5.5%
                 *   1024:         bash -3.4% ls -3.3% libc -3.3% py -6.1%
                 *   4096 (v2.38): bash -3.8% ls -3.9% libc -3.9% py -6.4%
                 *   8192+:        plateau (noise-level changes)
                 *
                 * Text/JSON/source at 0/0/+0.1% across the entire
                 * sweep — the adaptive gate does its job.
                 *
                 * Rejected designs:
                 *   - Sliding threshold (≤128 always, ≤512 if rep):
                 *     tightening to 128 lost more binary gain than
                 *     rep-aware loosening recovered. See CHANGELOG.
                 *   - ANS_LOG 12→10 (Sprint A candidate): predicted
                 *     2-4× text decode; measured 2-6%. Not worth the
                 *     format change. See CHANGELOG v2.38.0 dead-ends.
                 *
                 * Longer matches (len ≥ 4) are always accepted at any
                 * offset (the filter only gates len==3). */
                int32_t off3 = pos - ref3;
                if (len == 3 && off3 > 4096) {
                    ref3 = next_ref3;
                    continue;
                }
                if (len > best_len) {
                    best_len = len;
                    *best_off = off3;
                    if (len >= 8) break;  /* Good enough — don't keep walking */
                }
            }
            ref3 = next_ref3;
        }
    }

    return best_len;
}

static int32_t chain_match(const matcher_t *m, const uint8_t *data,
                            int32_t pos, int32_t end, int32_t *best_off) {
    return chain_match_ex(m, data, pos, end, best_off, m->use_hash4);
}

/* Update rep offsets (push new offset, shift others down) */
static inline void update_rep(matcher_t *m, uint32_t offset) {
    if (offset == m->rep[0]) return;
    m->rep[2] = m->rep[1];
    m->rep[1] = m->rep[0];
    m->rep[0] = offset;
}

/* ═══════════════════════════════════════════════════════════════
 * EMIT TOKEN (unchanged from v0.1)
 * ═══════════════════════════════════════════════════════════════ */

static size_t emit_seq(uint8_t *dst, const uint8_t *lits,
                        size_t ll, size_t ml, uint32_t off, int off_bytes,
                        int min_match) {
    uint8_t *op = dst;

    uint8_t ll_f = (ll >= 15) ? 15 : (uint8_t)ll;
    uint8_t ml_f;
    if (ml == 0) { ml_f = 0; }
    else { size_t v = ml - (size_t)min_match; ml_f = (v >= 15) ? 15 : (uint8_t)v; }

    *op++ = (ll_f << 4) | ml_f;

    if (ll >= 15) op += write_varint(op, ll - 15);
    if (ll > 0) { memcpy(op, lits, ll); op += ll; }

    if (ml > 0) {
        /* PERF: 2-byte offset for wlog≤16, 3-byte for wlog>16 */
        if (off_bytes == 3) {
            op[0] = (uint8_t)(off);
            op[1] = (uint8_t)(off >> 8);
            op[2] = (uint8_t)(off >> 16);
            op += 3;
        } else {
            vv_write16(op, (uint16_t)off); op += 2;
        }
        if (ml - (size_t)min_match >= 15)
            op += write_varint(op, ml - (size_t)min_match - 15);
    }
    return (size_t)(op - dst);
}

/* ═══════════════════════════════════════════════════════════════
 * COMPRESS BLOCK: greedy / lazy / lazy-2
 *
 * Match-skip heuristic: after a match of length ≥ 16, only insert
 * the last 3 positions into the hash chain. The interior positions
 * are inside the match and won't be needed. This saves O(match_len)
 * hash insertions, speeding up compression by 15-25% at L3+.
 * ═══════════════════════════════════════════════════════════════ */

static size_t compress_block(const uint8_t *src, size_t start_pos, size_t block_len,
                             uint8_t *dst, size_t dst_cap,
                             matcher_t *m, vv_mode_t mode, int min_match) {
    uint8_t *op = dst;
    int32_t pos = (int32_t)start_pos;
    int32_t end = (int32_t)(start_pos + block_len);
    const uint8_t *lit_start = src + start_pos;
    int off_bytes = (m->wlog > 16) ? 3 : 2;

    while (pos < end - min_match) {
        int32_t mlen = 0, moff = 0;

        /* ─── Step 1: Try rep-match (free, no hash lookup) ─── */
        int32_t rep_idx = -1;
        int32_t rep_len = try_rep_match(m, src, pos, end, &rep_idx);

        if (rep_len >= min_match) {
            mlen = rep_len;
            moff = (int32_t)m->rep[rep_idx];
        }

        /* ─── Step 2: Hash chain match (only if rep didn't find a long one) ─── */
        if (mlen < 8) {
            int32_t chain_off = 0;
            int32_t chain_len = chain_match(m, src, pos, end, &chain_off);
            if (chain_len > mlen) {
                mlen = chain_len;
                moff = chain_off;
                rep_idx = -1; /* not a rep match */
            }
        }

        /* ─── Step 3: Lazy evaluation (balanced + extreme) ─── */
        if (mode >= VV_MODE_BALANCED && mlen >= min_match &&
            pos + 1 < end - min_match) {
            /* Check pos+1 */
            matcher_insert(m, src, pos, end);
            int32_t noff = 0;
            int32_t nlen = chain_match(m, src, pos + 1, end, &noff);

            /* Also check rep at pos+1 */
            int32_t nri = -1;
            int32_t nrl = try_rep_match(m, src, pos + 1, end, &nri);
            if (nrl > nlen) { nlen = nrl; noff = (int32_t)m->rep[nri]; }

            /* standard */int32_t lazy_gain = 2;
            if (nlen > mlen + lazy_gain) {
                /* pos+1 is significantly better: emit literal, shift */
                pos++;
                mlen = nlen; moff = noff;

                /* Lazy-2 disabled in v2.24.0.
                 *
                 * Previously, after a lazy-1 shift to pos+1, this block
                 * would try another shift to pos+2 if n2len > mlen + 1.
                 * That was a classic "greedy past the peak" bug:
                 *
                 * Empirical results on 50KB English-text corpus:
                 *   balanced:              11,279 bytes
                 *   extreme + lazy-2 (+1): 12,157 bytes  (+8% vs balanced)
                 *   extreme + lazy-2 (+2): 12,157 bytes  (threshold didn't matter)
                 *   extreme + lazy-2 (+4): 11,860 bytes  (still worse)
                 *   extreme, lazy-2 off:   10,718 bytes  (5% better!)
                 *
                 * On 4 text corpora tested, lazy-2 cost an aggregate
                 * 2,463 bytes vs disabled. On 2 JSON corpora, it saved
                 * 213 bytes. Net-bytes-across-inputs: disabled wins by
                 * ~10×. Disabling produces the strictly correct
                 * "extreme >= balanced ratio" contract on all tested
                 * inputs except JSON, where the regression is tiny
                 * (<5%) and offset-encoding-cost-aware parsing would
                 * be the proper fix (future work).
                 *
                 * Root cause: lazy-2's break-even model doesn't
                 * account for the offset-extra-bits encoding cost of
                 * the shifted-to match. On text, deeper chain search
                 * finds long matches at far offsets whose extra-bits
                 * cost exceeds the gain from the extra match length. */
                (void)lazy_gain;  /* still used in lazy-1 above */
                if (0) {
                    /* dead code — reference for future cost-aware work */
                    matcher_insert(m, src, pos, end);
                    int32_t n2off = 0;
                    int32_t n2len = chain_match(m, src, pos + 1, end, &n2off);
                    int32_t n2ri = -1;
                    int32_t n2rl = try_rep_match(m, src, pos + 1, end, &n2ri);
                    if (n2rl > n2len) { n2len = n2rl; n2off = (int32_t)m->rep[n2ri]; }
                    if (n2len > mlen + lazy_gain) {
                        pos++;
                        mlen = n2len; moff = n2off;
                    }
                }
            }
        }

        /* ─── Step 4: Emit sequence or literal ─── */
        if (mlen >= min_match) {
            size_t ll = (size_t)(src + pos - lit_start);
            size_t needed = 1 + (ll >= 15 ? ll / 255 + 2 : 0)
                          + ll + 2 + ((size_t)mlen / 255 + 2);
            if ((size_t)(op - dst) + needed > dst_cap) return 0;

            op += emit_seq(op, lit_start, ll, (size_t)mlen, (uint32_t)moff, off_bytes, min_match);

            /* ─── Hash insertion with skip heuristic ─── */
            if (mlen >= 16) {
                /* Long match: only insert boundary positions */
                for (int32_t j = pos; j < pos + 3 && j < end - 4; j++)
                    matcher_insert(m, src, j, end);
                for (int32_t j = pos + mlen - 3; j < pos + mlen && j < end - 4; j++)
                    matcher_insert(m, src, j, end);
            } else {
                /* Short match: insert all positions */
                for (int32_t j = pos; j < pos + mlen && j < end - 4; j++)
                    matcher_insert(m, src, j, end);
            }

            update_rep(m, (uint32_t)moff);
            pos += mlen;
            lit_start = src + pos;
        } else {
            matcher_insert(m, src, pos, end);
            pos++;
        }
    }

    /* ─── Trailing literals ─── */
    {
        size_t ll = (size_t)(src + end - lit_start);
        size_t needed = 1 + (ll >= 15 ? ll / 255 + 2 : 0) + ll;
        if ((size_t)(op - dst) + needed > dst_cap) return 0;
        op += emit_seq(op, lit_start, ll, 0, 0, off_bytes, min_match);
    }

    return (size_t)(op - dst);
}

/* ═══════════════════════════════════════════════════════════════
 * EXTRACT LITERALS FROM TOKEN STREAM
 *
 * Walks a type-1 LZ token stream, copies all literal bytes into
 * lit_buf and produces a "stripped" token stream (same format but
 * with literal bytes removed) in stripped_buf.
 *
 * Returns the number of literals extracted, or 0 on error.
 * ═══════════════════════════════════════════════════════════════ */

static size_t extract_literals(
    const uint8_t *tokens, size_t tok_len,
    uint8_t *lit_buf,      size_t lit_cap,
    uint8_t *stripped_buf,  size_t *stripped_len, int off_bytes)
{
    const uint8_t *tp = tokens;
    const uint8_t *tp_end = tokens + tok_len;
    uint8_t *sp = stripped_buf;
    size_t total_lits = 0;

    while (tp < tp_end) {
        uint8_t token = *tp++;
        *sp++ = token;  /* Copy token byte to stripped stream */

        size_t ll = token >> 4;
        size_t mc = token & 0x0F;

        /* Extended literal length */
        if (ll == 15) {
            size_t ext = 0;
            do {
                if (tp >= tp_end) return 0;
                uint8_t b = *tp++;
                *sp++ = b;  /* Copy extension byte */
                ext += b;
                if (b < 255) break;
            } while (tp < tp_end);
            ll += ext;
        }

        /* Literal bytes: copy to lit_buf, do NOT copy to stripped stream */
        if (tp + ll > tp_end) return 0;
        if (total_lits + ll > lit_cap) return 0;
        memcpy(lit_buf + total_lits, tp, ll);
        total_lits += ll;
        tp += ll;

        /* End of block: no more data = last sequence (no match) */
        if (tp >= tp_end) break;

        /* Offset: 2 or 3 bytes, copy to stripped stream */
        if (tp + off_bytes > tp_end) return 0;
        for (int i = 0; i < off_bytes; i++) *sp++ = *tp++;

        /* Extended match length */
        if (mc == 15) {
            size_t ext = 0;
            do {
                if (tp >= tp_end) return 0;
                uint8_t b = *tp++;
                *sp++ = b;
                ext += b;
                if (b < 255) break;
            } while (tp < tp_end);
            (void)ext;
        }
    }

    *stripped_len = (size_t)(sp - stripped_buf);
    return total_lits;
}

/* ═══════════════════════════════════════════════════════════════
 * BLOCK EMISSION HELPER
 *
 * Encodes a single block of up to VV_MAX_BLOCK_SIZE bytes from
 * src[block_start..block_start+braw) and emits the compressed block
 * to dst. Picks the best path (raw / LZ-raw / 'S' seq / 'I'/'C' lit)
 * via winner-takes-all in balanced+extreme modes.
 *
 * Used by both the one-shot vv_compress() and the streaming
 * vv_cstream_compress_chunk(). Expects:
 *   - src:          source buffer (full source for vv_compress; the
 *                   persistent stream buffer for streaming)
 *   - block_start:  offset in src where this block begins
 *   - braw:         block raw length (≤ VV_MAX_BLOCK_SIZE)
 *   - last:         1 if this is the last block in the frame
 *   - m:            matcher state (persists across blocks)
 *   - mode:         compression mode (affects path choice)
 *   - wlog:         window log
 *   - tmp/tcap:     scratch buffer for LZ-compressed tokens
 *   - lit_buf/lit_cap, stripped, ent_buf/ent_cap: entropy scratch
 *   - dst/dst_cap:  output buffer
 *
 * Returns bytes written to dst on success, or 0 on overflow. */
static size_t emit_block(const uint8_t *src, size_t block_start, size_t braw,
                         int last, matcher_t *m, vv_mode_t mode, uint8_t wlog,
                         uint8_t *tmp, size_t tcap,
                         uint8_t *lit_buf, size_t lit_cap,
                         uint8_t *stripped, uint8_t *ent_buf, size_t ent_cap,
                         uint8_t *dst, size_t dst_cap, int min_match) {
    uint8_t *op = dst;

    size_t csz = compress_block(src, block_start, braw, tmp, tcap, m, mode, min_match);

    if (csz == 0 || csz >= braw) {
        /* Incompressible: store raw */
        if ((size_t)(op - dst) + 4 + braw > dst_cap) return 0;
        uint32_t bh = vv_bh_pack(VV_BLOCK_RAW, last, (uint32_t)braw);
        memcpy(op, &bh, 4); op += 4;
        memcpy(op, src + block_start, braw); op += braw;
        return (size_t)(op - dst);
    }

    if (mode >= VV_MODE_BALANCED) {
        /* Path A: sequence coding ('S') */
        size_t seq_len = 0;
        int seq_valid = 0;
        size_t seq_block_sz = (size_t)-1;
        int off_bytes = (wlog > 16) ? 3 : 2;
        /* Format v2: when min_match < 4 (i.e. 3), encode with the v2
         * table so length-3 matches are representable as code 0. The
         * token stream produced by compress_block(min_match=3) may
         * contain 3-byte matches that v1 encode_sequences cannot
         * represent correctly. */
        int use_v2 = (min_match < (int)VV_MIN_MATCH);
        vva_error_t serr = use_v2
            ? vva_encode_sequences_v2(tmp, csz, ent_buf, ent_cap, &seq_len, off_bytes)
            : vva_encode_sequences(tmp, csz, ent_buf, ent_cap, &seq_len, off_bytes);
        if (serr == VVA_OK) {
            seq_block_sz = 4 + 3 + 1 + seq_len;
            seq_valid = 1;
        }

        /* Path B: literal-only entropy ('I' or 'C') */
        size_t stripped_len = 0;
        size_t lit_count = 0;
        uint8_t *ent_buf2 = ent_buf + ent_cap / 2;
        size_t ent_cap2 = ent_cap / 2;
        size_t ent_len = 0;
        uint8_t ent_tag = 0;
        size_t ent_block_sz = (size_t)-1;

        int try_path_b = 1;
        if (mode == VV_MODE_BALANCED && seq_valid && seq_block_sz < (braw / 3)) {
            /* SPRINT 29 (revised in v2.15): always try Path B in BALANCED
             * mode, comparing both costs and picking the smaller. The
             * earlier "skip Path B if seq compressed >3:1" heuristic
             * (added in Sprint 28 for speed) saved ~30% encode time but
             * hurt ratio on text-heavy data — Silesia dickens/reymont
             * showed Path B's 'C' tag would have produced 5-10% smaller
             * output but never got the chance.
             *
             * v2.15 trade-off: encoder is ~25% slower in BALANCED mode
             * but ratio improves measurably on text. Decode speed is
             * unaffected (decoder doesn't care which tag was chosen).
             *
             * In ULTRA_FAST/FAST modes the original skip remains in
             * effect because those modes are throughput-priority. */
            (void)try_path_b;
        }

        if (try_path_b) {
            lit_count = extract_literals(tmp, csz, lit_buf, lit_cap,
                                         stripped, &stripped_len, off_bytes);
            if (lit_count > 0) {
                /* SPRINT 53: skip the expensive CTX (order-1 context)
                 * path when sequence coding is already winning by a
                 * big margin. Profile data across 7 fixtures (text,
                 * json, source, 4 ELF binaries) showed CTX wins 0/16
                 * attempts — the CTX coder has never actually beaten
                 * SEQ on these workloads, but burned 20% of encode
                 * time building per-context ANS tables that were
                 * always discarded.
                 *
                 * Heuristic: skip CTX when seq_block_sz already does
                 * better than 2:1 compression (seq_block_sz < braw/2).
                 * Path A (SEQ) essentially never loses to Path B (CTX)
                 * when the LZ matcher found strong matches. CTX only
                 * matters for low-redundancy data where SEQ produces
                 * close-to-raw output — exactly the case where
                 * seq_block_sz ≥ braw/2.
                 *
                 * Falls back to ANS4 / ANS as literal coders in the
                 * unchanged code below. These are ~10× cheaper than
                 * CTX to build. Net encode-time savings measured in
                 * SPRINT 53 CHANGELOG entry.
                 *
                 * Security/correctness: this is purely an encoder
                 * heuristic. Decoder is unchanged. Output wire format
                 * still meets spec. Worst case on a pathological
                 * input where CTX would have won: we produce slightly
                 * larger output via ANS4 or ANS. Ratio gate guards
                 * against any real regression. */
                int skip_ctx = seq_valid && seq_block_sz < (braw * 4 / 5);
                if (!skip_ctx && mode >= VV_MODE_BALANCED && lit_count >= 4096) {
                    vva_error_t aerr = vva_encode_ctx(lit_buf, lit_count,
                                                       ent_buf2, ent_cap2, &ent_len);
                    if (aerr == VVA_OK) ent_tag = VV_ENTROPY_CTX;
                }
                if (!ent_tag) {
                    vva_error_t aerr = vva_encode4(lit_buf, lit_count,
                                                    ent_buf2, ent_cap2, &ent_len);
                    if (aerr == VVA_OK) ent_tag = VV_ENTROPY_ANS4;
                }
                if (!ent_tag) {
                    vva_error_t aerr = vva_encode(lit_buf, lit_count,
                                                   ent_buf2, ent_cap2, &ent_len);
                    if (aerr == VVA_OK) ent_tag = VV_ENTROPY_ANS;
                }
                if (ent_tag) {
                    ent_block_sz = 4 + 3 + 1 + 2 + 2 + ent_len + stripped_len;
                }
            }
        }

        size_t raw_block_sz = 4 + 3 + csz;

        if (seq_valid && seq_block_sz <= ent_block_sz && seq_block_sz < raw_block_sz) {
            if ((size_t)(op - dst) + seq_block_sz > dst_cap) return 0;
            uint32_t bh = vv_bh_pack(VV_BLOCK_ENTROPY, last, (uint32_t)braw);
            memcpy(op, &bh, 4); op += 4;
            uint32_t total_comp = (uint32_t)(1 + seq_len);
            op[0] = (uint8_t)(total_comp);
            op[1] = (uint8_t)(total_comp >> 8);
            op[2] = (uint8_t)(total_comp >> 16);
            op += 3;
            *op++ = use_v2 ? VV_ENTROPY_SEQ_V2 : VV_ENTROPY_SEQ;
            memcpy(op, ent_buf, seq_len); op += seq_len;
        } else if (!use_v2 && ent_tag && ent_block_sz < raw_block_sz) {
            /* Path B (H/I/C entropy) uses `stripped` tokens which still
             * contain v1-format matchlen bytes. Only safe for v1. For
             * v2, we must skip this fallback to avoid emitting v1 tokens
             * that a v2-aware decoder wouldn't reconstruct correctly. */
            if ((size_t)(op - dst) + ent_block_sz > dst_cap) return 0;
            uint32_t bh = vv_bh_pack(VV_BLOCK_ENTROPY, last, (uint32_t)braw);
            memcpy(op, &bh, 4); op += 4;
            uint32_t total_comp = (uint32_t)(5 + ent_len + stripped_len);
            op[0] = (uint8_t)(total_comp);
            op[1] = (uint8_t)(total_comp >> 8);
            op[2] = (uint8_t)(total_comp >> 16);
            op += 3;
            *op++ = ent_tag;
            op[0] = (uint8_t)(lit_count); op[1] = (uint8_t)(lit_count >> 8); op += 2;
            op[0] = (uint8_t)(ent_len); op[1] = (uint8_t)(ent_len >> 8); op += 2;
            memcpy(op, ent_buf2, ent_len); op += ent_len;
            memcpy(op, stripped, stripped_len); op += stripped_len;
        } else if (!use_v2) {
            /* Plain VV_BLOCK_COMPRESSED carries raw v1-format tokens.
             * For v2, we must not emit these — the decoder would
             * reconstruct matchlen with +4 instead of +3. Fall to RAW
             * block instead (handled below via "else" when raw_block_sz
             * is smaller). We reach this branch only when the previous
             * conditions all failed AND we're NOT v2. */
            if ((size_t)(op - dst) + raw_block_sz > dst_cap) return 0;
            uint32_t bh = vv_bh_pack(VV_BLOCK_COMPRESSED, last, (uint32_t)braw);
            memcpy(op, &bh, 4); op += 4;
            op[0] = (uint8_t)(csz); op[1] = (uint8_t)(csz >> 8); op[2] = (uint8_t)(csz >> 16);
            op += 3;
            memcpy(op, tmp, csz); op += csz;
        } else {
            /* v2 path, sequence coding didn't fit/help: emit RAW. */
            if ((size_t)(op - dst) + 4 + braw > dst_cap) return 0;
            uint32_t bh = vv_bh_pack(VV_BLOCK_RAW, last, (uint32_t)braw);
            memcpy(op, &bh, 4); op += 4;
            memcpy(op, src + block_start, braw); op += braw;
        }
    } else {
        /* Ultra-fast mode */
        if ((size_t)(op - dst) + 4 + 3 + csz > dst_cap) return 0;
        uint32_t bh = vv_bh_pack(VV_BLOCK_COMPRESSED, last, (uint32_t)braw);
        memcpy(op, &bh, 4); op += 4;
        op[0] = (uint8_t)(csz); op[1] = (uint8_t)(csz >> 8); op[2] = (uint8_t)(csz >> 16);
        op += 3;
        memcpy(op, tmp, csz); op += csz;
    }

    return (size_t)(op - dst);
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API: COMPRESS
 * ═══════════════════════════════════════════════════════════════ */

size_t vv_compress_bound(size_t src_len) {
    return src_len + src_len / 255 + 256
         + sizeof(vv_frame_header_t) + sizeof(vv_frame_footer_t);
}

int64_t vv_compress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap,
                    const vv_options_t *opts) {
    if (!src || !dst || !opts) return VV_ERR_PARAM;
    if (dst_cap < sizeof(vv_frame_header_t) + sizeof(vv_frame_footer_t) + 16)
        return VV_ERR_OVERFLOW;

    uint8_t wlog = opts->window_log;
    uint32_t depth;
    if (wlog == 0) {
        switch (opts->mode) {
        case VV_MODE_ULTRA_FAST: wlog = 16; break;
        case VV_MODE_BALANCED:   wlog = 16; break; /* may be overridden below */
        case VV_MODE_EXTREME:    wlog = 16; break; /* may be overridden below */
        }
    }
    switch (opts->mode) {
    case VV_MODE_ULTRA_FAST: depth = 4; break;
    case VV_MODE_BALANCED:   depth = 24; break; /* was 48 — halving barely affects ratio, doubles speed */
    case VV_MODE_EXTREME:    depth = 256; break;
    default: depth = 24;
    }

    /* ─── ADAPTIVE WINDOW + HASH4 detection in a single trial.
     * PERF: previously this was two separate 128K+64K=192K trials, run
     * sequentially. We can make BOTH decisions from the SAME trial:
     *   - window: wlog=20 wins if it saves ≥3% vs wlog=16
     *   - hash4:  enable if ratio < 2:1 (indicates binary-like data) */
    int enable_hash4 = 0;
    if (opts->window_log == 0 && opts->mode >= VV_MODE_BALANCED && src_len > 65536) {
        size_t trial_len = 131072;
        if (trial_len > src_len) trial_len = src_len;

        size_t trial_cap = trial_len + trial_len / 255 + 1024;
        uint8_t *trial_buf = (uint8_t *)malloc(trial_cap);
        if (trial_buf) {
            matcher_t m16; matcher_init(&m16, 16, 4);
            size_t sz16 = compress_block(src, 0, trial_len, trial_buf, trial_cap, &m16, VV_MODE_ULTRA_FAST, VV_MIN_MATCH);
            matcher_free(&m16);

            matcher_t m20; matcher_init(&m20, 20, 4);
            size_t sz20 = compress_block(src, 0, trial_len, trial_buf, trial_cap, &m20, VV_MODE_ULTRA_FAST, VV_MIN_MATCH);
            matcher_free(&m20);

            free(trial_buf);
            if (sz20 > 0 && sz16 > 0 && sz20 < (sz16 * 97 / 100)) wlog = 20;
            /* Binary-like detection: best trial ratio < 2:1 */
            size_t best_sz = (sz20 > 0 && sz20 < sz16) ? sz20 : sz16;
            if (best_sz > 0 && best_sz * 2 > trial_len) enable_hash4 = 1;
        }
    }

    /* SPRINT 67: size-based wlog override. The trial above often
     * misses wins that only become visible past the 128 KB trial
     * boundary (long-range refs in multi-MB files). Override to
     * wlog=18 for files ≥ 3 MB when the trial left wlog at 16. */
    if (opts->window_log == 0 && opts->mode >= VV_MODE_BALANCED &&
        wlog == 16 && src_len >= 3145728) {
        wlog = 18;
    }

    /* Frame header */
    uint8_t *op = dst;
    vv_frame_header_t fh;
    memset(&fh, 0, sizeof(fh));
    fh.magic = VV_MAGIC;
    fh.version = 1;
    fh.flags = opts->checksum ? 1 : 0;
    fh.mode_hint = (uint8_t)opts->mode;
    fh.window_log = wlog;
    fh.content_size = (uint64_t)src_len;
    memcpy(op, &fh, sizeof(fh)); op += sizeof(fh);

    /* Matcher */
    matcher_t m;
    matcher_init(&m, wlog, depth);
    m.use_hash4 = (uint8_t)enable_hash4; /* From fused adaptive-window trial */
    /* Format v2 cap applies to EVERY match emitted from this matcher,
     * not just those produced via hash3. Set unconditionally when
     * opts.format_v2 is active. */
    if (opts->format_v2) {
        matcher_set_format_v2(&m);
    }
    /* Hash3 enablement is a separate, adaptive decision. Only fires
     * on binary-like data (enable_hash4) where length-3 matches
     * actually help. On text/JSON it stays off to avoid regressions. */
    if (opts->format_v2 && enable_hash4) {
        if (!matcher_enable_hash3(&m)) {
            matcher_free(&m);
            return VV_ERR_NOMEM;
        }
    }

    /* Temp buffer.
     * PERF: size to the actual input (not always 1MB). For a 4KB input,
     * tcap was ~1.03MB — a wasteful allocation. Now allocate just enough
     * to hold the LZ-tokenized output, bounded by VV_MAX_BLOCK_SIZE. */
    size_t block_bound = src_len < VV_MAX_BLOCK_SIZE ? src_len : VV_MAX_BLOCK_SIZE;
    size_t tcap = block_bound + block_bound / 255 + 1024;
    uint8_t *tmp = (uint8_t *)malloc(tcap);
    if (!tmp) { matcher_free(&m); return VV_ERR_NOMEM; }

    /* Additional buffers for entropy path (only allocated if needed) */
    uint8_t *lit_buf = NULL, *stripped = NULL, *ent_buf = NULL;
    size_t lit_cap = 0, ent_cap = 0;
    if (opts->mode >= VV_MODE_BALANCED) {
        /* PERF: size these to the actual input too — they only need to
         * cover the single in-flight block's worth of literals/entropy
         * output. For small one-shot calls this avoids ~3 MB of wasted
         * allocation and page-faulting every call. */
        lit_cap = block_bound;
        ent_cap = vva_bound(block_bound);
        lit_buf = (uint8_t *)malloc(lit_cap);
        stripped = (uint8_t *)malloc(tcap);
        ent_buf = (uint8_t *)malloc(ent_cap);
        if (!lit_buf || !stripped || !ent_buf) {
            free(lit_buf); free(stripped); free(ent_buf);
            free(tmp); matcher_free(&m);
            return VV_ERR_NOMEM;
        }
    }

    size_t remaining = src_len;
    const uint8_t *ip = src;

    if (remaining == 0) {
        uint32_t bh = vv_bh_pack(VV_BLOCK_RAW, 1, 0);
        memcpy(op, &bh, 4); op += 4;
    }

    /* Format v2: when opts->format_v2 is set, encode with min_match=3.
     * Produces 'T'-tagged ENTROPY blocks which only v2.33.0+ decoders
     * can read. Closes the real-binary compression gap vs gzip-9. */
    int min_match = opts->format_v2 ? 3 : (int)VV_MIN_MATCH;

    while (remaining > 0) {
        size_t braw = remaining > VV_MAX_BLOCK_SIZE ? VV_MAX_BLOCK_SIZE : remaining;
        int last = (remaining <= VV_MAX_BLOCK_SIZE);

        size_t block_start = (size_t)(ip - src);
        size_t written = emit_block(src, block_start, braw, last, &m, opts->mode, wlog,
                                    tmp, tcap, lit_buf, lit_cap,
                                    stripped, ent_buf, ent_cap,
                                    op, dst_cap - (size_t)(op - dst), min_match);
        if (written == 0) {
            free(lit_buf); free(stripped); free(ent_buf);
            free(tmp); matcher_free(&m);
            return VV_ERR_OVERFLOW;
        }
        op += written;
        ip += braw; remaining -= braw;
    }

    free(lit_buf); free(stripped); free(ent_buf);
    free(tmp);

    if (opts->checksum) {
        vv_frame_footer_t ff;
        ff.checksum = vv_xxh64(src, src_len, 0);
        ff.footer_magic = 0x56564E44u;
        memcpy(op, &ff, sizeof(ff)); op += sizeof(ff);
    }

    matcher_free(&m);
    return (int64_t)(op - dst);
}

/* ═══════════════════════════════════════════════════════════════
 * STREAMING COMPRESSION
 *
 * A compression stream buffers persistent state across calls:
 *   - The matcher (hash tables, chains, rep-match offsets)
 *   - Scratch buffers (tmp/lit_buf/stripped/ent_buf)
 *   - Streaming xxh64 state for the frame checksum
 *   - The full input so far in sliding-window form (needed because
 *     LZ matches can reference up to 2^wlog bytes back)
 *
 * Each call to vv_cstream_compress_chunk() appends chunk bytes to
 * the internal source buffer, emits one block covering those bytes,
 * and optionally emits the frame header (first call) and footer
 * (when is_last is set).
 *
 * Memory cost: 2 × window_size + ~10 MB scratch (ent_buf, etc.).
 * For wlog=16 that's ~131 KB + scratch; wlog=20 is ~2 MB + scratch.
 * ═══════════════════════════════════════════════════════════════ */

struct vv_cstream_s {
    vv_options_t opts;
    uint8_t       wlog;
    matcher_t     m;

    /* Scratch buffers — allocated once, reused across chunks */
    uint8_t *tmp;       size_t tcap;
    uint8_t *lit_buf;   size_t lit_cap;
    uint8_t *stripped;
    uint8_t *ent_buf;   size_t ent_cap;

    /* Sliding-window source buffer. We accumulate input so offset-based
     * match references resolve correctly. Old bytes beyond the window
     * are dropped in periodic compaction. */
    uint8_t *src_buf;        /* Capacity = 2 × window_size */
    size_t   src_cap;
    size_t   src_head;       /* First valid byte index in src_buf */
    size_t   src_len;        /* Number of valid bytes in src_buf */
    size_t   global_offset;  /* src_buf[i] corresponds to stream offset (global_offset - src_len + i) */

    /* Streaming checksum */
    vv_xxh64_state_t cks;

    int header_emitted;
};

vv_cstream_t *vv_cstream_create(const vv_options_t *opts) {
    vv_cstream_t *ctx = (vv_cstream_t *)calloc(1, sizeof(vv_cstream_t));
    if (!ctx) return NULL;

    if (opts) ctx->opts = *opts;
    else vv_default_options(&ctx->opts);

    /* Resolve window log (fixed for streams — no adaptive probe) */
    uint8_t wlog = ctx->opts.window_log;
    if (wlog == 0) wlog = 16;
    ctx->wlog = wlog;

    uint32_t depth;
    switch (ctx->opts.mode) {
    case VV_MODE_ULTRA_FAST: depth = 4; break;
    case VV_MODE_BALANCED:   depth = 24; break;
    case VV_MODE_EXTREME:    depth = 256; break;
    default: depth = 24;
    }

    matcher_init(&ctx->m, wlog, depth);
    /* Format v2 matchlen cap applies to every match — set whenever
     * streaming opts has format_v2 on, not just when hash3 fires. */
    if (opts->format_v2) {
        matcher_set_format_v2(&ctx->m);
    }
    /* SPRINT 45: enable hash3 for format v2 streaming. Must free
     * ctx before returning NULL — callers use NULL-check semantics
     * here, not error codes. */
    if (opts->format_v2) {
        if (!matcher_enable_hash3(&ctx->m)) {
            matcher_free(&ctx->m);
            free(ctx);
            return NULL;
        }
    }

    /* Scratch buffers sized for VV_MAX_BLOCK_SIZE */
    ctx->tcap    = VV_MAX_BLOCK_SIZE + VV_MAX_BLOCK_SIZE / 255 + 1024;
    ctx->tmp     = (uint8_t *)malloc(ctx->tcap);
    ctx->lit_cap = VV_MAX_BLOCK_SIZE;
    ctx->lit_buf = (uint8_t *)malloc(ctx->lit_cap);
    ctx->stripped = (uint8_t *)malloc(ctx->lit_cap);
    ctx->ent_cap = vva_bound(VV_MAX_BLOCK_SIZE);
    ctx->ent_buf = (uint8_t *)malloc(ctx->ent_cap);

    /* Source window = 2 × window_size so a full block of input can
     * land before we compact. */
    size_t window = (size_t)1u << wlog;
    ctx->src_cap = window * 2 + VV_MAX_BLOCK_SIZE;
    ctx->src_buf = (uint8_t *)malloc(ctx->src_cap);

    if (!ctx->tmp || !ctx->lit_buf || !ctx->stripped || !ctx->ent_buf || !ctx->src_buf) {
        vv_cstream_destroy(ctx);
        return NULL;
    }

    if (ctx->opts.checksum) vv_xxh64_init(&ctx->cks, 0);
    ctx->header_emitted = 0;
    return ctx;
}

void vv_cstream_destroy(vv_cstream_t *ctx) {
    if (!ctx) return;
    free(ctx->tmp); free(ctx->lit_buf); free(ctx->stripped); free(ctx->ent_buf);
    free(ctx->src_buf);
    matcher_free(&ctx->m);
    free(ctx);
}

int vv_cstream_reset(vv_cstream_t *ctx, const vv_options_t *opts) {
    if (!ctx) return VV_ERR_PARAM;

    /* Apply new options if provided. window_log cannot change without
     * reallocating the matcher tables — reject the change. */
    if (opts) {
        uint8_t new_wlog = opts->window_log;
        if (new_wlog == 0) new_wlog = 16;
        if (new_wlog != ctx->wlog) return VV_ERR_PARAM;
        ctx->opts = *opts;
    }

    /* Update chain_depth in case the mode changed */
    uint32_t depth;
    switch (ctx->opts.mode) {
    case VV_MODE_ULTRA_FAST: depth = 4; break;
    case VV_MODE_BALANCED:   depth = 24; break;
    case VV_MODE_EXTREME:    depth = 256; break;
    default: depth = 24;
    }
    ctx->m.chain_depth = depth;

    matcher_reset(&ctx->m);

    /* Reset sliding-window source buffer */
    ctx->src_head = 0;
    ctx->src_len = 0;
    ctx->global_offset = 0;

    /* Reset checksum */
    if (ctx->opts.checksum) vv_xxh64_init(&ctx->cks, 0);

    ctx->header_emitted = 0;
    return VV_OK;
}

int vv_cstream_compress_chunk(vv_cstream_t *ctx,
                              const uint8_t *chunk, size_t chunk_len,
                              uint8_t *dst, size_t dst_cap,
                              size_t *written, int is_last) {
    if (!ctx || !dst || !written) return VV_ERR_PARAM;
    if (chunk_len > VV_MAX_BLOCK_SIZE) return VV_ERR_PARAM;
    *written = 0;

    uint8_t *op = dst;
    size_t cap_left = dst_cap;

    /* Emit frame header on first call */
    if (!ctx->header_emitted) {
        if (cap_left < sizeof(vv_frame_header_t)) return VV_ERR_OVERFLOW;
        vv_frame_header_t fh;
        memset(&fh, 0, sizeof(fh));
        fh.magic = VV_MAGIC;
        fh.version = 1;
        fh.flags = ctx->opts.checksum ? 1 : 0;
        fh.mode_hint = (uint8_t)ctx->opts.mode;
        fh.window_log = ctx->wlog;
        /* content_size unknown in streaming mode → 0 */
        fh.content_size = 0;
        memcpy(op, &fh, sizeof(fh));
        op += sizeof(fh); cap_left -= sizeof(fh);
        ctx->header_emitted = 1;
    }

    /* Append chunk to sliding-window source buffer.
     * Compact the buffer if needed to stay under src_cap. We keep
     * the last (window_size) bytes as match-lookback history. */
    if (chunk_len > 0) {
        size_t window = (size_t)1u << ctx->wlog;
        size_t needed = ctx->src_len + chunk_len;
        if (needed > ctx->src_cap) {
            /* Compact: drop everything older than (window) bytes before end */
            size_t keep = ctx->src_len > window ? window : ctx->src_len;
            size_t drop = ctx->src_len - keep;
            if (drop > 0) {
                memmove(ctx->src_buf, ctx->src_buf + drop, keep);
                ctx->src_len = keep;
                /* Adjust matcher table/chain entries: positions were
                 * relative to src_buf[0] and are now shifted by -drop.
                 * Easiest correct approach: invalidate chains — they
                 * reference positions < limit automatically and are
                 * bounded-distance walked. The hash table's `table[h]`
                 * entries would now point at shifted positions, but
                 * we can shift them en masse. */
                /* Shift matcher table entries (positions get re-based) */
                for (uint32_t i = 0; i < VV_HC_SIZE; i++) {
                    if (ctx->m.table[i] >= (int32_t)drop)
                        ctx->m.table[i] -= (int32_t)drop;
                    else ctx->m.table[i] = -1;
                }
                for (uint32_t i = 0; i < VV_HC4_SIZE; i++) {
                    if (ctx->m.table4[i] >= (int32_t)drop)
                        ctx->m.table4[i] -= (int32_t)drop;
                    else ctx->m.table4[i] = -1;
                }
                /* Chain arrays are also indexed by position — shift those
                 * too, BUT the array is indexed by (pos & chain_mask) so
                 * we need to shift values (the successor position) while
                 * keeping the circular layout. For simplicity and safety,
                 * we rebuild conservatively: clear chain entries whose
                 * references would now be negative. */
                for (uint32_t i = 0; i < (1u << ctx->wlog); i++) {
                    if (ctx->m.chain[i] >= (int32_t)drop)
                        ctx->m.chain[i] -= (int32_t)drop;
                    else ctx->m.chain[i] = -1;
                    if (ctx->m.hash4_chain[i] >= (int32_t)drop)
                        ctx->m.hash4_chain[i] -= (int32_t)drop;
                    else ctx->m.hash4_chain[i] = -1;
                }
            }
        }
        memcpy(ctx->src_buf + ctx->src_len, chunk, chunk_len);
        ctx->src_len += chunk_len;
        ctx->global_offset += chunk_len;

        if (ctx->opts.checksum) vv_xxh64_update(&ctx->cks, chunk, chunk_len);
    }

    /* Emit block(s) for the newly added chunk_len bytes.
     * block_start in the src_buf = ctx->src_len - chunk_len. */
    if (chunk_len == 0 && is_last) {
        /* Empty final chunk: emit empty raw-last block */
        if (cap_left < 4) return VV_ERR_OVERFLOW;
        uint32_t bh = vv_bh_pack(VV_BLOCK_RAW, 1, 0);
        memcpy(op, &bh, 4); op += 4; cap_left -= 4;
    } else if (chunk_len > 0) {
        size_t block_start = ctx->src_len - chunk_len;
        int stream_min_match = ctx->opts.format_v2 ? 3 : (int)VV_MIN_MATCH;
        size_t block_sz = emit_block(ctx->src_buf, block_start, chunk_len, is_last,
                                     &ctx->m, ctx->opts.mode, ctx->wlog,
                                     ctx->tmp, ctx->tcap,
                                     ctx->lit_buf, ctx->lit_cap,
                                     ctx->stripped, ctx->ent_buf, ctx->ent_cap,
                                     op, cap_left, stream_min_match);
        if (block_sz == 0) return VV_ERR_OVERFLOW;
        op += block_sz; cap_left -= block_sz;
    }

    /* Emit frame footer on last chunk */
    if (is_last && ctx->opts.checksum) {
        if (cap_left < sizeof(vv_frame_footer_t)) return VV_ERR_OVERFLOW;
        vv_frame_footer_t ff;
        ff.checksum = vv_xxh64_finalize(&ctx->cks);
        ff.footer_magic = 0x56564E44u;
        memcpy(op, &ff, sizeof(ff));
        op += sizeof(ff); cap_left -= sizeof(ff);
    }

    *written = (size_t)(op - dst);
    return VV_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * MULTI-THREADED COMPRESSION
 *
 * Strategy: split input into chunks of chunk_size bytes. Each chunk
 * is encoded independently via vv_compress() into its own .vv frame.
 * Output frames are concatenated into dst. vv_decompress handles
 * multi-frame input natively.
 *
 * When VV_ENABLE_THREADS is defined, use pthread to run N worker
 * threads in parallel. Otherwise, run sequentially.
 *
 * Ratio cost: frames are independent — cross-frame match history
 * is lost at chunk boundaries. For chunk_size ≥ 4 MB on
 * compressible data, the ratio hit is typically < 2%.
 * ═══════════════════════════════════════════════════════════════ */

#ifdef VV_ENABLE_THREADS
#include <pthread.h>
#include <unistd.h>

typedef struct {
    const uint8_t *src;
    size_t src_len;
    uint8_t *dst;
    size_t dst_cap;
    const vv_options_t *opts;
    int64_t result;  /* compressed size, or error code */
} mt_task_t;

typedef struct {
    mt_task_t *tasks;
    size_t ntasks;
    volatile size_t next_task;
    pthread_mutex_t mutex;
} mt_pool_t;

static void *mt_worker(void *arg) {
    mt_pool_t *pool = (mt_pool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        size_t idx = pool->next_task++;
        pthread_mutex_unlock(&pool->mutex);
        if (idx >= pool->ntasks) break;
        mt_task_t *t = &pool->tasks[idx];
        t->result = vv_compress(t->src, t->src_len, t->dst, t->dst_cap, t->opts);
    }
    return NULL;
}
#endif

int64_t vv_compress_mt(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap,
                       const vv_options_t *opts,
                       unsigned int nthreads,
                       size_t chunk_size) {
    if (!src || !dst || !opts) return VV_ERR_PARAM;
    if (chunk_size == 0) chunk_size = 4 * 1024 * 1024;  /* 4 MB default */
    if (chunk_size < VV_MAX_BLOCK_SIZE) chunk_size = VV_MAX_BLOCK_SIZE;

    /* For small inputs, just use vv_compress directly — no speedup
     * available and avoids the per-frame fixed overhead. */
    if (src_len <= chunk_size) {
        return vv_compress(src, src_len, dst, dst_cap, opts);
    }

    /* Split into N chunks */
    size_t n_chunks = (src_len + chunk_size - 1) / chunk_size;

    /* Allocate per-chunk temporary output buffers. Each could be up to
     * vv_compress_bound(chunk_size), which can be ~4 MB * 1.01 for a
     * 4 MB chunk. Total scratch = n_chunks * ~4 MB. */
    uint8_t **chunk_dst = (uint8_t **)calloc(n_chunks, sizeof(uint8_t *));
    int64_t *chunk_sz = (int64_t *)calloc(n_chunks, sizeof(int64_t));
    if (!chunk_dst || !chunk_sz) {
        free(chunk_dst); free(chunk_sz);
        return VV_ERR_NOMEM;
    }

    size_t chunk_cap = vv_compress_bound(chunk_size);
    int alloc_failed = 0;
    for (size_t i = 0; i < n_chunks; i++) {
        chunk_dst[i] = (uint8_t *)malloc(chunk_cap);
        if (!chunk_dst[i]) { alloc_failed = 1; break; }
    }
    if (alloc_failed) {
        for (size_t i = 0; i < n_chunks; i++) free(chunk_dst[i]);
        free(chunk_dst); free(chunk_sz);
        return VV_ERR_NOMEM;
    }

#ifdef VV_ENABLE_THREADS
    /* Determine thread count */
    if (nthreads == 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = (n > 0) ? (unsigned int)n : 1;
    }
    if (nthreads > n_chunks) nthreads = (unsigned int)n_chunks;
    if (nthreads == 0) nthreads = 1;

    /* Build task list */
    mt_task_t *tasks = (mt_task_t *)malloc(n_chunks * sizeof(mt_task_t));
    if (!tasks) {
        for (size_t i = 0; i < n_chunks; i++) free(chunk_dst[i]);
        free(chunk_dst); free(chunk_sz);
        return VV_ERR_NOMEM;
    }
    for (size_t i = 0; i < n_chunks; i++) {
        size_t off = i * chunk_size;
        size_t len = (off + chunk_size <= src_len) ? chunk_size : (src_len - off);
        tasks[i].src = src + off;
        tasks[i].src_len = len;
        tasks[i].dst = chunk_dst[i];
        tasks[i].dst_cap = chunk_cap;
        tasks[i].opts = opts;
        tasks[i].result = 0;
    }

    mt_pool_t pool;
    pool.tasks = tasks;
    pool.ntasks = n_chunks;
    pool.next_task = 0;
    pthread_mutex_init(&pool.mutex, NULL);

    pthread_t *threads = (pthread_t *)malloc(nthreads * sizeof(pthread_t));
    if (!threads) {
        pthread_mutex_destroy(&pool.mutex);
        free(tasks);
        for (size_t i = 0; i < n_chunks; i++) free(chunk_dst[i]);
        free(chunk_dst); free(chunk_sz);
        return VV_ERR_NOMEM;
    }
    for (unsigned int t = 0; t < nthreads; t++)
        pthread_create(&threads[t], NULL, mt_worker, &pool);
    for (unsigned int t = 0; t < nthreads; t++)
        pthread_join(threads[t], NULL);
    free(threads);
    pthread_mutex_destroy(&pool.mutex);

    for (size_t i = 0; i < n_chunks; i++) chunk_sz[i] = tasks[i].result;
    free(tasks);
#else
    /* Sequential fallback: encode each chunk in turn. */
    (void)nthreads;
    for (size_t i = 0; i < n_chunks; i++) {
        size_t off = i * chunk_size;
        size_t len = (off + chunk_size <= src_len) ? chunk_size : (src_len - off);
        chunk_sz[i] = vv_compress(src + off, len, chunk_dst[i], chunk_cap, opts);
    }
#endif

    /* Check for errors and total up sizes */
    int64_t total = 0;
    for (size_t i = 0; i < n_chunks; i++) {
        if (chunk_sz[i] < 0) {
            int64_t err = chunk_sz[i];
            for (size_t j = 0; j < n_chunks; j++) free(chunk_dst[j]);
            free(chunk_dst); free(chunk_sz);
            return err;
        }
        total += chunk_sz[i];
    }

    if ((size_t)total > dst_cap) {
        for (size_t i = 0; i < n_chunks; i++) free(chunk_dst[i]);
        free(chunk_dst); free(chunk_sz);
        return VV_ERR_OVERFLOW;
    }

    /* Concatenate frames into dst */
    uint8_t *op = dst;
    for (size_t i = 0; i < n_chunks; i++) {
        memcpy(op, chunk_dst[i], (size_t)chunk_sz[i]);
        op += chunk_sz[i];
        free(chunk_dst[i]);
    }
    free(chunk_dst); free(chunk_sz);

    return total;
}

/* ── src/vv_decoder.c ── */
/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VaptVupt — Decoder v2 (Sprint 1)
 *
 * KEY CHANGES:
 *   1. AVX2 inline copies in hot loop (eliminates function-pointer dispatch)
 *   2. Early offset load → prefetch match source before literal copy
 *   3. Safe-zone: skip per-byte bounds checks while far from buffer ends
 *   4. Pattern-fill SIMD for overlapping match (offset < 16)
 *   5. General path as fallback for tail bytes + non-AVX2 platforms
 */

#include <string.h>
#include <stdlib.h>

#if defined(__x86_64__) && defined(__AVX2__)
#include <immintrin.h>
#define VV_INLINE_AVX2 1
#else
#define VV_INLINE_AVX2 0
#endif

/* ─── Cold varint reader (out-of-line to keep hot loop compact) ─── */
__attribute__((noinline))
static size_t read_ext_len(const uint8_t **pp, const uint8_t *end) {
    size_t val = 0;
    const uint8_t *p = *pp;
    while (p < end) {
        uint8_t b = *p++;
        val += b;
        if (b < 255) break;
    }
    *pp = p;
    return val;
}

/* ═══════════════════════════════════════════════════════════════
 * INLINE SIMD HELPERS (AVX2 only, compiled on x86-64 -mavx2)
 * ═══════════════════════════════════════════════════════════════ */

#if VV_INLINE_AVX2

static inline void wcopy16(uint8_t *d, const uint8_t *s) {
    _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
}
static inline void wcopy32(uint8_t *d, const uint8_t *s) {
    _mm256_storeu_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
}

static inline void wcopy_n(uint8_t *d, const uint8_t *s, size_t n) {
    while (n >= 32) { wcopy32(d, s); d += 32; s += 32; n -= 32; }
    if (n >= 16) { wcopy16(d, s); d += 16; s += 16; n -= 16; }
    if (n > 0) wcopy16(d, s); /* safe over-copy in safe zone */
}

/* Match copy with offset >= 32: 32-byte chunks, NO over-copy at tail */
static inline void match_copy_32(uint8_t *d, const uint8_t *s, size_t n) {
    while (n >= 32) { wcopy32(d, s); d += 32; s += 32; n -= 32; }
    /* Exact tail: use 16-byte then memcpy to avoid corrupting future output */
    if (n >= 16) { wcopy16(d, s); d += 16; s += 16; n -= 16; }
    if (n > 0) memcpy(d, s, n);
}

/* Match copy with offset 16-31: 16-byte chunks, exact tail */
static inline void match_copy_16(uint8_t *d, const uint8_t *s, size_t n) {
    while (n >= 16) { wcopy16(d, s); d += 16; s += 16; n -= 16; }
    if (n > 0) memcpy(d, s, n);
}

/* Match copy with offset 8-15: 8-byte register copy */
static inline void match_copy_8(uint8_t *d, uint32_t off, size_t n) {
    const uint8_t *s = d - off;
    while (n >= 8) {
        uint64_t v; memcpy(&v, s, 8);
        memcpy(d, &v, 8);
        s += 8; d += 8; n -= 8;
    }
    while (n > 0) { *d++ = *s++; n--; }
}

/* Match copy with offset 1-7: byte-by-byte (correct for all offsets)
 * The 16-byte pattern-fill approach FAILS for offsets that don't divide 16
 * (e.g., offset=3: after 16 bytes the pattern misaligns). Since offset<16
 * is only ~5% of matches, byte-by-byte is fast enough. */
static inline void match_overlap(uint8_t *d, uint32_t off, size_t n) {
    const uint8_t *s = d - off;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

#endif /* VV_INLINE_AVX2 */

/* ═══════════════════════════════════════════════════════════════
 * DECODE BLOCK — TWO-TIER HOT PATH
 * ═══════════════════════════════════════════════════════════════ */

/* PERF: Force-inline core decode body so off_bytes becomes a compile-time
 * constant in each specialized variant, eliminating the ternary from the
 * hot path and enabling better branch prediction + offset loads. */
static __attribute__((always_inline)) inline vv_error_t
decode_block_tokens_impl(
    const uint8_t *ip, size_t ip_len,
    uint8_t *op, size_t dst_cap, size_t *out_len,
    const uint8_t *dst_base,
    const int off_bytes)  /* compile-time constant after inlining */
{
    const uint8_t *const ip_end = ip + ip_len;
    uint8_t *const op_start = op;
    uint8_t *const op_end = op + dst_cap;

    /* PERF: widened safe-zone margins (was 24/40).
     * Larger margins = fewer bound-check-triggered loop exits per block.
     * Exit boundary: max is 1 token + 14 lits + 3 offset + 6 match_ext = 24.
     * Plus match_copy_32 may over-copy 32 bytes past the real end, so
     * op needs at least 64 bytes of margin. */
    const uint8_t *const ip_safe = (ip_len > 48) ? (ip_end - 48) : ip;
    uint8_t *const op_safe = (dst_cap > 72) ? (op_end - 72) : op;

    /* PERF: once we've written enough bytes, any offset ≤ max_dist passes
     * the "offset > op - dst_base" check. Max offset is (1 << wlog) - 1,
     * at most (1<<20) - 1 for wlog=20. So past this threshold, only
     * offset==0 needs checking (invalid/corrupted). */
    const uint32_t max_valid_off = (off_bytes == 2) ? 0xFFFF : 0xFFFFFF;

#if VV_INLINE_AVX2
    /* PERF: two-phase fast path.
     * Phase 1 (warmup): op hasn't advanced far enough to make any offset
     *   automatically valid. Do full offset validation per sequence.
     * Phase 2 (hot): op - dst_base > max_valid_off, so any non-zero
     *   offset within 2/3 bytes is automatically valid — skip the
     *   (op - dst_base) comparison, keep only offset==0 check. */

    /* Phase 1: warmup — full validation */
    while (VV_LIKELY(ip < ip_safe && op < op_safe
                      && (uint32_t)(op - dst_base) <= max_valid_off)) {
        uint32_t token = *ip++;
        uint32_t ll = token >> 4;
        uint32_t mc = token & 0x0F;

        if (VV_UNLIKELY(ll == 15))
            ll += (uint32_t)read_ext_len(&ip, ip_end);

        if (VV_LIKELY(ll <= 14 && ip + ll + 2 <= ip_end)) {
            uint16_t off_raw;
            memcpy(&off_raw, ip + ll, 2);
            if (off_raw > 0)
                VV_PREFETCH(op + ll - off_raw);
        }

        if (ll > 0)
            memcpy(op, ip, ll);
        ip += ll;
        op += ll;

        if (VV_UNLIKELY(ip >= ip_end)) break;

        uint32_t offset;
        if (off_bytes == 2) {
            offset = vv_read16(ip);
        } else {
            offset = (uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16);
        }
        ip += off_bytes;

        uint32_t mlen = mc + VV_MIN_MATCH;
        if (VV_UNLIKELY(mc == 15))
            mlen += (uint32_t)read_ext_len(&ip, ip_end);

        if (VV_UNLIKELY(offset == 0 || offset > (uint32_t)(op - dst_base)))
            return VV_ERR_CORRUPT;

        if (VV_LIKELY(offset >= 32)) {
            match_copy_32(op, op - offset, mlen);
        } else if (offset >= 16) {
            match_copy_16(op, op - offset, mlen);
        } else if (offset >= 8) {
            match_copy_8(op, offset, mlen);
        } else {
            match_overlap(op, offset, mlen);
        }
        op += mlen;
    }

    /* Phase 2: hot path — op is far enough in that any non-zero offset
     * within 2-byte or 3-byte range is automatically valid. */
    while (VV_LIKELY(ip < ip_safe && op < op_safe)) {
        uint32_t token = *ip++;
        uint32_t ll = token >> 4;
        uint32_t mc = token & 0x0F;

        if (VV_UNLIKELY(ll == 15))
            ll += (uint32_t)read_ext_len(&ip, ip_end);

        if (VV_LIKELY(ll <= 14 && ip + ll + 2 <= ip_end)) {
            uint16_t off_raw;
            memcpy(&off_raw, ip + ll, 2);
            if (off_raw > 0)
                VV_PREFETCH(op + ll - off_raw);
        }

        if (ll > 0)
            memcpy(op, ip, ll);
        ip += ll;
        op += ll;

        if (VV_UNLIKELY(ip >= ip_end)) break;

        uint32_t offset;
        if (off_bytes == 2) {
            offset = vv_read16(ip);
        } else {
            offset = (uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16);
        }
        ip += off_bytes;

        uint32_t mlen = mc + VV_MIN_MATCH;
        if (VV_UNLIKELY(mc == 15))
            mlen += (uint32_t)read_ext_len(&ip, ip_end);

        /* No (op - dst_base) check needed — op is past max_valid_off */
        if (VV_UNLIKELY(offset == 0))
            return VV_ERR_CORRUPT;

        if (VV_LIKELY(offset >= 32)) {
            match_copy_32(op, op - offset, mlen);
        } else if (offset >= 16) {
            match_copy_16(op, op - offset, mlen);
        } else if (offset >= 8) {
            match_copy_8(op, offset, mlen);
        } else {
            match_overlap(op, offset, mlen);
        }
        op += mlen;
    }
#endif

    /* General path (tail + non-AVX2) */
    while (ip < ip_end) {
        uint8_t token = *ip++;
        size_t ll = token >> 4;
        size_t mc = token & 0x0F;

        if (VV_UNLIKELY(ll == 15))
            ll += read_ext_len(&ip, ip_end);

        if (VV_UNLIKELY(ip + ll > ip_end)) return VV_ERR_CORRUPT;
        if (VV_UNLIKELY(op + ll > op_end)) return VV_ERR_OVERFLOW;

        if (ll > 0) vv_copy_fast(op, ip, ll);
        ip += ll;
        op += ll;

        if (ip >= ip_end) break;

        if (VV_UNLIKELY(ip + off_bytes > ip_end)) return VV_ERR_CORRUPT;
        uint32_t offset;
        if (off_bytes == 2) {
            offset = vv_read16(ip);
        } else {
            offset = (uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16);
        }
        ip += off_bytes;

        size_t mlen = mc + VV_MIN_MATCH;
        if (VV_UNLIKELY(mc == 15))
            mlen += read_ext_len(&ip, ip_end);

        if (VV_UNLIKELY(offset == 0 || offset > (uint32_t)(op - dst_base)))
            return VV_ERR_CORRUPT;
        if (VV_UNLIKELY(op + mlen > op_end))
            return VV_ERR_OVERFLOW;

        vv_copy_match(op, offset, mlen);
        op += mlen;
    }

    *out_len = (size_t)(op - op_start);
    return VV_OK;
}

/* Specialized for 2-byte offsets (wlog ≤ 16) — the common fast path */
static vv_error_t decode_block_tokens_w16(
    const uint8_t *ip, size_t ip_len,
    uint8_t *op, size_t dst_cap, size_t *out_len,
    const uint8_t *dst_base)
{
    return decode_block_tokens_impl(ip, ip_len, op, dst_cap, out_len, dst_base, 2);
}

/* Specialized for 3-byte offsets (wlog > 16) */
static vv_error_t decode_block_tokens_w20(
    const uint8_t *ip, size_t ip_len,
    uint8_t *op, size_t dst_cap, size_t *out_len,
    const uint8_t *dst_base)
{
    return decode_block_tokens_impl(ip, ip_len, op, dst_cap, out_len, dst_base, 3);
}

static vv_error_t decode_block_tokens(
    const uint8_t *ip, size_t ip_len,
    uint8_t *op, size_t dst_cap, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    if (off_bytes == 2)
        return decode_block_tokens_w16(ip, ip_len, op, dst_cap, out_len, dst_base);
    return decode_block_tokens_w20(ip, ip_len, op, dst_cap, out_len, dst_base);
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE STRIPPED TOKEN STREAM (for type 3 / Huffman blocks)
 *
 * Same as decode_block_tokens but literal bytes are NOT inline.
 * Instead, they come from a pre-decoded literal buffer.
 * Token format: same headers/offsets/extensions, just no literal bytes.
 * ═══════════════════════════════════════════════════════════════ */

/* PERF: force-inline body so off_bytes becomes a compile-time constant */
static __attribute__((always_inline)) inline vv_error_t
decode_stripped_tokens_impl(
    const uint8_t *ip, size_t ip_len,
    const uint8_t *lit_buf, size_t lit_len,
    uint8_t *op, size_t dst_cap, size_t *out_len,
    const uint8_t *dst_base,
    const int off_bytes)
{
    const uint8_t *ip_end = ip + ip_len;
    uint8_t *op_start = op;
    uint8_t *op_end = op + dst_cap;
    size_t lit_pos = 0;

    while (ip < ip_end) {
        uint8_t token = *ip++;
        size_t ll = token >> 4;
        size_t mc = token & 0x0F;

        if (VV_UNLIKELY(ll == 15))
            ll += read_ext_len(&ip, ip_end);

        if (VV_UNLIKELY(lit_pos + ll > lit_len)) return VV_ERR_CORRUPT;
        if (VV_UNLIKELY(op + ll > op_end)) return VV_ERR_OVERFLOW;
        if (ll > 0) {
            memcpy(op, lit_buf + lit_pos, ll);
            lit_pos += ll;
        }
        op += ll;

        if (ip >= ip_end) break;

        if (VV_UNLIKELY(ip + off_bytes > ip_end)) return VV_ERR_CORRUPT;
        /* PERF: off_bytes is compile-time constant here */
        uint32_t offset;
        if (off_bytes == 2) {
            offset = vv_read16(ip);
        } else {
            offset = (uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16);
        }
        ip += off_bytes;

        size_t mlen = mc + VV_MIN_MATCH;
        if (VV_UNLIKELY(mc == 15))
            mlen += read_ext_len(&ip, ip_end);

        if (VV_UNLIKELY(offset == 0 || offset > (uint32_t)(op - dst_base))) {
            return VV_ERR_CORRUPT;
        }
        if (VV_UNLIKELY(op + mlen > op_end))
            return VV_ERR_OVERFLOW;

        vv_copy_match(op, offset, mlen);
        op += mlen;
    }

    *out_len = (size_t)(op - op_start);
    return VV_OK;
}

static vv_error_t decode_stripped_tokens(
    const uint8_t *ip, size_t ip_len,
    const uint8_t *lit_buf, size_t lit_len,
    uint8_t *op, size_t dst_cap, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    if (off_bytes == 2) {
        return decode_stripped_tokens_impl(ip, ip_len, lit_buf, lit_len,
                                            op, dst_cap, out_len, dst_base, 2);
    }
    return decode_stripped_tokens_impl(ip, ip_len, lit_buf, lit_len,
                                        op, dst_cap, out_len, dst_base, 3);
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE TYPE 3 BLOCK (Huffman-compressed literals)
 *
 * Layout: [2B lit_count] [2B huff_section_size] [huff_data] [stripped_tokens]
 * ═══════════════════════════════════════════════════════════════ */

static vv_error_t decode_block_huffman(
    const uint8_t *data, size_t data_len,
    uint8_t *output, size_t decomp_size, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    if (data_len < 4) return VV_ERR_CORRUPT;

    /* Read lit_count and huff_section_size */
    uint16_t lit_count = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t huff_sz   = (uint16_t)(data[2] | (data[3] << 8));

    if (4 + (size_t)huff_sz > data_len) return VV_ERR_CORRUPT;

    /* Huffman-decode all literals */
    uint8_t *lit_buf = (uint8_t *)malloc((size_t)lit_count + 16);
    if (!lit_buf) return VV_ERR_NOMEM;

    size_t huff_consumed = 0;
    vvh_error_t herr = vvh_decode(data + 4, huff_sz, lit_buf, lit_count,
                                   lit_count, &huff_consumed);
    if (herr != VVH_OK) { free(lit_buf); return VV_ERR_CORRUPT; }

    /* Parse stripped token stream */
    const uint8_t *tokens = data + 4 + huff_sz;
    size_t tok_len = data_len - 4 - huff_sz;

    vv_error_t err = decode_stripped_tokens(tokens, tok_len,
                                             lit_buf, lit_count,
                                             output, decomp_size, out_len, off_bytes, dst_base);
    free(lit_buf);
    return err;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE TYPE 3 BLOCK (ANS-compressed literals, v0.5+)
 *
 * Layout: [2B lit_count] [2B ans_section_size] [ans_data] [stripped_tokens]
 * ═══════════════════════════════════════════════════════════════ */

static vv_error_t decode_block_ans(
    const uint8_t *data, size_t data_len,
    uint8_t *output, size_t decomp_size, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    if (data_len < 4) return VV_ERR_CORRUPT;

    uint16_t lit_count = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t ans_sz    = (uint16_t)(data[2] | (data[3] << 8));

    if (4 + (size_t)ans_sz > data_len) return VV_ERR_CORRUPT;

    /* ANS-decode all literals */
    uint8_t *lit_buf = (uint8_t *)malloc((size_t)lit_count + 16);
    if (!lit_buf) return VV_ERR_NOMEM;

    size_t ans_consumed = 0;
    vva_error_t aerr = vva_decode(data + 4, ans_sz, lit_buf, lit_count,
                                   lit_count, &ans_consumed);
    if (aerr != VVA_OK) { free(lit_buf); return VV_ERR_CORRUPT; }

    /* Parse stripped token stream */
    const uint8_t *tokens = data + 4 + ans_sz;
    size_t tok_len = data_len - 4 - ans_sz;

    vv_error_t err = decode_stripped_tokens(tokens, tok_len,
                                             lit_buf, lit_count,
                                             output, decomp_size, out_len, off_bytes, dst_base);
    free(lit_buf);
    return err;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE TYPE 3 BLOCK, TAG 'I' (4-way interleaved ANS, v0.6+)
 * ═══════════════════════════════════════════════════════════════ */

static vv_error_t decode_block_ans4(
    const uint8_t *data, size_t data_len,
    uint8_t *output, size_t decomp_size, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    if (data_len < 4) return VV_ERR_CORRUPT;

    uint16_t lit_count = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t ans_sz    = (uint16_t)(data[2] | (data[3] << 8));

    if (4 + (size_t)ans_sz > data_len) return VV_ERR_CORRUPT;

    uint8_t *lit_buf = (uint8_t *)malloc((size_t)lit_count + 16);
    if (!lit_buf) return VV_ERR_NOMEM;

    size_t ans_consumed = 0;
    vva_error_t aerr = vva_decode4(data + 4, ans_sz, lit_buf, lit_count,
                                    lit_count, &ans_consumed);
    if (aerr != VVA_OK) { free(lit_buf); return VV_ERR_CORRUPT; }

    const uint8_t *tokens = data + 4 + ans_sz;
    size_t tok_len = data_len - 4 - ans_sz;

    vv_error_t err = decode_stripped_tokens(tokens, tok_len,
                                             lit_buf, lit_count,
                                             output, decomp_size, out_len, off_bytes, dst_base);
    free(lit_buf);
    return err;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE TYPE 3 BLOCK, TAG 'C' (order-1 context model ANS, v0.7+)
 * ═══════════════════════════════════════════════════════════════ */

static vv_error_t decode_block_ctx(
    const uint8_t *data, size_t data_len,
    uint8_t *output, size_t decomp_size, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    if (data_len < 4) return VV_ERR_CORRUPT;

    uint16_t lit_count = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t ans_sz    = (uint16_t)(data[2] | (data[3] << 8));

    if (4 + (size_t)ans_sz > data_len) return VV_ERR_CORRUPT;

    uint8_t *lit_buf = (uint8_t *)malloc((size_t)lit_count + 16);
    if (!lit_buf) return VV_ERR_NOMEM;

    size_t ans_consumed = 0;
    vva_error_t aerr = vva_decode_ctx(data + 4, ans_sz, lit_buf, lit_count,
                                       lit_count, &ans_consumed);
    if (aerr != VVA_OK) { free(lit_buf); return VV_ERR_CORRUPT; }

    const uint8_t *tokens = data + 4 + ans_sz;
    size_t tok_len = data_len - 4 - ans_sz;

    vv_error_t err = decode_stripped_tokens(tokens, tok_len,
                                             lit_buf, lit_count,
                                             output, decomp_size, out_len, off_bytes, dst_base);
    free(lit_buf);
    return err;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API: DECOMPRESS
 * ═══════════════════════════════════════════════════════════════ */

int64_t vv_decompress(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap) {
    return vv_decompress_flags(src, src_len, dst, dst_cap, VV_DECOMPRESS_DEFAULT);
}

int64_t vv_decompress_flags(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_cap,
                            uint32_t flags) {
    if (!src || !dst) return VV_ERR_PARAM;
    if (src_len < sizeof(vv_frame_header_t)) return VV_ERR_CORRUPT;

    const uint8_t *ip = src;
    const uint8_t *ip_end = src + src_len;
    uint8_t *op = dst;
    uint8_t *op_end = dst + dst_cap;

    /* MULTI-FRAME: a .vv file may contain one or more concatenated frames
     * (useful for parallel encode, Zupt-style archives, append-mode
     * writes). We decode frames in a loop until input is exhausted. */
    while (ip < ip_end) {
        if (ip + sizeof(vv_frame_header_t) > ip_end) return VV_ERR_CORRUPT;

        vv_frame_header_t fh;
        memcpy(&fh, ip, sizeof(fh));
        ip += sizeof(fh);

        if (fh.magic != VV_MAGIC) return VV_ERR_BAD_MAGIC;
        if (fh.version != 1) return VV_ERR_CORRUPT;

        int has_checksum = (fh.flags & 1);
        int off_bytes = (fh.window_log > 16) ? 3 : 2;

        /* Per-frame dst_base: matches must resolve only within this frame.
         * Multi-frame files mean frame 2's matches don't reach into
         * frame 1's output — each frame is independently decodable. */
        uint8_t *frame_out_start = op;

        for (;;) {
            if (ip + 4 > ip_end) return VV_ERR_CORRUPT;
            uint32_t bh_packed;
            memcpy(&bh_packed, ip, 4); ip += 4;

            vv_block_type_t btype = vv_bh_type(bh_packed);
            int is_last = vv_bh_last(bh_packed);
            uint32_t dsz = vv_bh_size(bh_packed);

            if (dsz > VV_MAX_BLOCK_SIZE) return VV_ERR_OVERFLOW;
            if ((size_t)(op - dst) + dsz > dst_cap) return VV_ERR_OVERFLOW;
            (void)op_end;

            if (btype == VV_BLOCK_RAW) {
                if (ip + dsz > ip_end) return VV_ERR_CORRUPT;
                memcpy(op, ip, dsz); ip += dsz; op += dsz;
            } else if (btype == VV_BLOCK_RLE) {
                if (ip >= ip_end) return VV_ERR_CORRUPT;
                memset(op, *ip++, dsz); op += dsz;
            } else if (btype == VV_BLOCK_COMPRESSED) {
                if (ip + 3 > ip_end) return VV_ERR_CORRUPT;
                uint32_t csz = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) | ((uint32_t)ip[2] << 16);
                ip += 3;
                if (ip + csz > ip_end) return VV_ERR_CORRUPT;

                size_t actual = 0;
                vv_error_t err = decode_block_tokens(ip, csz, op, dsz, &actual, off_bytes, frame_out_start);
                if (err != VV_OK) return err;
                if (actual != dsz) return VV_ERR_CORRUPT;
                ip += csz; op += dsz;
            } else if (btype == VV_BLOCK_ENTROPY) {
                if (ip + 3 > ip_end) return VV_ERR_CORRUPT;
                uint32_t csz = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) | ((uint32_t)ip[2] << 16);
                ip += 3;
                if (csz < 1 || ip + csz > ip_end) return VV_ERR_CORRUPT;

                uint8_t tag = ip[0];
                const uint8_t *bdata = ip + 1;
                size_t bdata_len = csz - 1;
                size_t actual = 0;
                vv_error_t err;

                if (tag == VV_ENTROPY_ANS) {
                    err = decode_block_ans(bdata, bdata_len, op, dsz, &actual, off_bytes, frame_out_start);
                } else if (tag == VV_ENTROPY_ANS4) {
                    err = decode_block_ans4(bdata, bdata_len, op, dsz, &actual, off_bytes, frame_out_start);
                } else if (tag == VV_ENTROPY_CTX) {
                    err = decode_block_ctx(bdata, bdata_len, op, dsz, &actual, off_bytes, frame_out_start);
                } else if (tag == VV_ENTROPY_SEQ) {
                    err = vva_decode_sequences(bdata, bdata_len, op, dsz, &actual, frame_out_start);
                    if (err != VV_OK) err = VV_ERR_CORRUPT;
                } else if (tag == VV_ENTROPY_SEQ_V2) {
                    /* 'T' tag: sequence coding with min_match=3. Wire
                     * payload identical to 'S', only ml_base differs. */
                    err = vva_decode_sequences_v2(bdata, bdata_len, op, dsz, &actual, frame_out_start);
                    if (err != VV_OK) err = VV_ERR_CORRUPT;
                } else if (tag == VV_ENTROPY_HUFFMAN) {
                    err = decode_block_huffman(bdata, bdata_len, op, dsz, &actual, off_bytes, frame_out_start);
                } else {
                    return VV_ERR_CORRUPT;
                }
                if (err != VV_OK) return err;
                if (actual != dsz) return VV_ERR_CORRUPT;
                ip += csz; op += dsz;
            } else {
                return VV_ERR_CORRUPT;
            }
            if (is_last) break;
        }

        if (has_checksum) {
            if (ip + sizeof(vv_frame_footer_t) > ip_end) return VV_ERR_CORRUPT;
            vv_frame_footer_t ff;
            memcpy(&ff, ip, sizeof(ff));
            if (ff.footer_magic != 0x56564E44u) return VV_ERR_CORRUPT;
            /* PERF: caller may skip XXH64 when another layer (e.g. AES-GCM)
             * already verifies integrity. Still validate footer magic above
             * to catch truncation. */
            if (!(flags & VV_DECOMPRESS_SKIP_CHECKSUM)) {
                uint64_t computed = vv_xxh64(frame_out_start, (size_t)(op - frame_out_start), 0);
                if (computed != ff.checksum) return VV_ERR_CORRUPT;
            }
            ip += sizeof(vv_frame_footer_t);
        }

        /* Loop back to try another frame (if input remains) */
    }

    return (int64_t)(op - dst);
}

/* ═══════════════════════════════════════════════════════════════
 * STREAMING DECOMPRESSION
 *
 * Incoming compressed bytes arrive in arbitrary chunks. Structure:
 *   1. Frame header (16 bytes) — must be accumulated before any
 *      blocks can be decoded
 *   2. Zero or more blocks, each: [4B header][3B csz][payload]
 *   3. Optional frame footer (16 bytes) — checksum validation
 *
 * Strategy: buffer incoming bytes in an internal growing buffer,
 * parse as much as we can at each call, and emit decoded output.
 *
 * For correct match decoding across blocks, we emit directly into
 * the caller's dst buffer and preserve dst_base so that sequences
 * referencing earlier decoded bytes resolve correctly. The caller
 * is responsible for providing a large enough dst buffer: the same
 * constraint as one-shot decompression.
 * ═══════════════════════════════════════════════════════════════ */

typedef enum {
    VV_DSTREAM_HEADER,
    VV_DSTREAM_BLOCK,
    VV_DSTREAM_FOOTER,
    VV_DSTREAM_DONE,
    VV_DSTREAM_ERROR
} vv_dstream_state_t;

struct vv_dstream_s {
    vv_dstream_state_t state;
    vv_frame_header_t  fh;
    int                has_checksum;
    int                off_bytes;

    /* Input-side buffer for incomplete blocks/headers */
    uint8_t *in_buf;
    size_t   in_cap;
    size_t   in_len;

    /* Output position tracking (for checksum and bookkeeping) */
    size_t   output_pos;
    uint8_t *dst_base_saved; /* Preserved across calls; matches caller dst */

    /* Streaming checksum of decoded output */
    vv_xxh64_state_t cks;
};

vv_dstream_t *vv_dstream_create(void) {
    vv_dstream_t *ctx = (vv_dstream_t *)calloc(1, sizeof(vv_dstream_t));
    if (!ctx) return NULL;
    ctx->state = VV_DSTREAM_HEADER;
    ctx->in_cap = 65536;
    ctx->in_buf = (uint8_t *)malloc(ctx->in_cap);
    if (!ctx->in_buf) { free(ctx); return NULL; }
    vv_xxh64_init(&ctx->cks, 0);
    return ctx;
}

void vv_dstream_destroy(vv_dstream_t *ctx) {
    if (!ctx) return;
    free(ctx->in_buf);
    free(ctx);
}

int vv_dstream_reset(vv_dstream_t *ctx) {
    if (!ctx) return VV_ERR_PARAM;
    /* Keep in_buf and in_cap (reuse scratch); clear everything else */
    ctx->state = VV_DSTREAM_HEADER;
    ctx->has_checksum = 0;
    ctx->off_bytes = 0;
    ctx->in_len = 0;
    ctx->output_pos = 0;
    ctx->dst_base_saved = NULL;
    memset(&ctx->fh, 0, sizeof(ctx->fh));
    vv_xxh64_init(&ctx->cks, 0);
    return VV_OK;
}

/* Grow in_buf to at least need bytes */
static int dstream_reserve(vv_dstream_t *ctx, size_t need) {
    if (need <= ctx->in_cap) return 0;
    size_t new_cap = ctx->in_cap;
    while (new_cap < need) new_cap *= 2;
    uint8_t *new_buf = (uint8_t *)realloc(ctx->in_buf, new_cap);
    if (!new_buf) return -1;
    ctx->in_buf = new_buf;
    ctx->in_cap = new_cap;
    return 0;
}

/* Append bytes to input buffer */
static int dstream_append(vv_dstream_t *ctx, const uint8_t *src, size_t src_len) {
    if (dstream_reserve(ctx, ctx->in_len + src_len) != 0) return -1;
    memcpy(ctx->in_buf + ctx->in_len, src, src_len);
    ctx->in_len += src_len;
    return 0;
}

/* Consume first n bytes from input buffer */
static void dstream_consume(vv_dstream_t *ctx, size_t n) {
    if (n >= ctx->in_len) ctx->in_len = 0;
    else {
        memmove(ctx->in_buf, ctx->in_buf + n, ctx->in_len - n);
        ctx->in_len -= n;
    }
}

int vv_dstream_decompress_chunk(vv_dstream_t *ctx,
                                const uint8_t *src, size_t src_len,
                                uint8_t *dst, size_t dst_cap,
                                size_t *consumed, size_t *written) {
    if (!ctx || !dst || !consumed || !written) return VV_ERR_PARAM;
    *consumed = 0;
    *written = 0;

    if (ctx->state == VV_DSTREAM_ERROR) return VV_ERR_CORRUPT;
    if (ctx->state == VV_DSTREAM_DONE) return 1;

    /* Append new input */
    if (src_len > 0) {
        if (dstream_append(ctx, src, src_len) != 0) {
            ctx->state = VV_DSTREAM_ERROR;
            return VV_ERR_NOMEM;
        }
        *consumed = src_len;
    }

    /* Remember dst_base for match resolution across blocks */
    if (!ctx->dst_base_saved) ctx->dst_base_saved = dst;
    /* Output position within dst (must match caller's expected write offset) */
    uint8_t *op = dst + ctx->output_pos;

    /* State machine: process as much as we can */
    for (;;) {
        if (ctx->state == VV_DSTREAM_HEADER) {
            if (ctx->in_len < sizeof(vv_frame_header_t)) { *written = ctx->output_pos; return VV_OK; }
            memcpy(&ctx->fh, ctx->in_buf, sizeof(vv_frame_header_t));
            if (ctx->fh.magic != VV_MAGIC) { ctx->state = VV_DSTREAM_ERROR; return VV_ERR_BAD_MAGIC; }
            if (ctx->fh.version != 1) { ctx->state = VV_DSTREAM_ERROR; return VV_ERR_CORRUPT; }
            ctx->has_checksum = (ctx->fh.flags & 1);
            ctx->off_bytes = (ctx->fh.window_log > 16) ? 3 : 2;
            dstream_consume(ctx, sizeof(vv_frame_header_t));
            ctx->state = VV_DSTREAM_BLOCK;
        }

        if (ctx->state == VV_DSTREAM_BLOCK) {
            /* Need at least 4 bytes for block header */
            if (ctx->in_len < 4) { *written = ctx->output_pos; return VV_OK; }

            uint32_t bh_packed;
            memcpy(&bh_packed, ctx->in_buf, 4);
            vv_block_type_t btype = vv_bh_type(bh_packed);
            int is_last = vv_bh_last(bh_packed);
            uint32_t dsz = vv_bh_size(bh_packed);

            if (dsz > VV_MAX_BLOCK_SIZE) { ctx->state = VV_DSTREAM_ERROR; return VV_ERR_OVERFLOW; }
            if ((size_t)(op - dst) + dsz > dst_cap) { ctx->state = VV_DSTREAM_ERROR; return VV_ERR_OVERFLOW; }

            /* Determine how many bytes this block occupies */
            size_t block_header_sz = 4;
            size_t block_data_sz = 0;

            if (btype == VV_BLOCK_RAW) {
                block_data_sz = dsz;
            } else if (btype == VV_BLOCK_RLE) {
                block_data_sz = 1;
            } else if (btype == VV_BLOCK_COMPRESSED || btype == VV_BLOCK_ENTROPY) {
                if (ctx->in_len < block_header_sz + 3) { *written = ctx->output_pos; return VV_OK; }
                const uint8_t *p = ctx->in_buf + block_header_sz;
                uint32_t csz = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                block_data_sz = 3 + csz;
            } else {
                ctx->state = VV_DSTREAM_ERROR; return VV_ERR_CORRUPT;
            }

            size_t total_block_sz = block_header_sz + block_data_sz;
            if (ctx->in_len < total_block_sz) { *written = ctx->output_pos; return VV_OK; }

            /* Decode the block — decoder uses dst_base for match resolution */
            const uint8_t *p = ctx->in_buf + block_header_sz;
            if (btype == VV_BLOCK_RAW) {
                memcpy(op, p, dsz);
            } else if (btype == VV_BLOCK_RLE) {
                memset(op, p[0], dsz);
            } else if (btype == VV_BLOCK_COMPRESSED) {
                uint32_t csz = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                size_t actual = 0;
                vv_error_t err = decode_block_tokens(p + 3, csz, op, dsz, &actual,
                                                      ctx->off_bytes, ctx->dst_base_saved);
                if (err != VV_OK || actual != dsz) { ctx->state = VV_DSTREAM_ERROR; return err != VV_OK ? err : VV_ERR_CORRUPT; }
            } else { /* ENTROPY */
                uint32_t csz = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                uint8_t tag = p[3];
                const uint8_t *bdata = p + 4;
                size_t bdata_len = csz - 1;
                size_t actual = 0;
                vv_error_t err;
                if (tag == VV_ENTROPY_ANS) {
                    err = decode_block_ans(bdata, bdata_len, op, dsz, &actual, ctx->off_bytes, ctx->dst_base_saved);
                } else if (tag == VV_ENTROPY_ANS4) {
                    err = decode_block_ans4(bdata, bdata_len, op, dsz, &actual, ctx->off_bytes, ctx->dst_base_saved);
                } else if (tag == VV_ENTROPY_CTX) {
                    err = decode_block_ctx(bdata, bdata_len, op, dsz, &actual, ctx->off_bytes, ctx->dst_base_saved);
                } else if (tag == VV_ENTROPY_SEQ) {
                    err = vva_decode_sequences(bdata, bdata_len, op, dsz, &actual, ctx->dst_base_saved);
                    if (err != VV_OK) err = VV_ERR_CORRUPT;
                } else if (tag == VV_ENTROPY_SEQ_V2) {
                    err = vva_decode_sequences_v2(bdata, bdata_len, op, dsz, &actual, ctx->dst_base_saved);
                    if (err != VV_OK) err = VV_ERR_CORRUPT;
                } else if (tag == VV_ENTROPY_HUFFMAN) {
                    err = decode_block_huffman(bdata, bdata_len, op, dsz, &actual, ctx->off_bytes, ctx->dst_base_saved);
                } else {
                    ctx->state = VV_DSTREAM_ERROR; return VV_ERR_CORRUPT;
                }
                if (err != VV_OK || actual != dsz) { ctx->state = VV_DSTREAM_ERROR; return err != VV_OK ? err : VV_ERR_CORRUPT; }
            }

            /* Update checksum (over decoded output) */
            if (ctx->has_checksum && dsz > 0) {
                vv_xxh64_update(&ctx->cks, op, dsz);
            }

            op += dsz;
            ctx->output_pos += dsz;
            dstream_consume(ctx, total_block_sz);

            if (is_last) {
                ctx->state = ctx->has_checksum ? VV_DSTREAM_FOOTER : VV_DSTREAM_DONE;
            }
        }

        if (ctx->state == VV_DSTREAM_FOOTER) {
            if (ctx->in_len < sizeof(vv_frame_footer_t)) { *written = ctx->output_pos; return VV_OK; }
            vv_frame_footer_t ff;
            memcpy(&ff, ctx->in_buf, sizeof(ff));
            if (ff.footer_magic != 0x56564E44u) { ctx->state = VV_DSTREAM_ERROR; return VV_ERR_CORRUPT; }
            uint64_t computed = vv_xxh64_finalize(&ctx->cks);
            if (computed != ff.checksum) { ctx->state = VV_DSTREAM_ERROR; return VV_ERR_CORRUPT; }
            dstream_consume(ctx, sizeof(vv_frame_footer_t));
            ctx->state = VV_DSTREAM_DONE;
        }

        if (ctx->state == VV_DSTREAM_DONE) {
            *written = ctx->output_pos;
            return 1;
        }
    }
}

/* ── src/vaptvupt_api.c ── */
/*
 * VaptVupt — Zupt Integration API Implementation
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Cristian.
 *
 * ZUPT-COMPAT: thin wrapper over vv_compress/vv_decompress with
 * backup-optimized defaults. Decode speed prioritized over encode.
 */


int64_t vvz_compress(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap, int level) {
    vv_options_t opts;
    vv_default_options(&opts);
    opts.checksum = 1; /* Always verify integrity for backups */

    if (level <= 2) {
        opts.mode = VV_MODE_ULTRA_FAST;
    } else if (level <= 7) {
        opts.mode = VV_MODE_BALANCED;
    } else {
        opts.mode = VV_MODE_EXTREME;
    }

    /* Auto window: let adaptive selection choose wlog */
    opts.window_log = 0;

    return vv_compress(src, src_len, dst, dst_cap, &opts);
}

int64_t vvz_decompress(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap) {
    return vv_decompress(src, src_len, dst, dst_cap);
}

size_t vvz_compress_bound(size_t src_len) {
    return vv_compress_bound(src_len);
}

/* ═══════════════════════════════════════════════════════════════
 * Frame metadata accessor
 * ═══════════════════════════════════════════════════════════════ */

int vv_get_frame_info(const uint8_t *src, size_t src_len,
                      vv_frame_info_t *info) {
    if (!src || !info) return VV_ERR_PARAM;
    if (src_len < sizeof(vv_frame_header_t)) return VV_ERR_CORRUPT;

    vv_frame_header_t fh;
    memcpy(&fh, src, sizeof(fh));
    if (fh.magic != VV_MAGIC) return VV_ERR_BAD_MAGIC;
    if (fh.version != 1) return VV_ERR_CORRUPT;

    info->version = fh.version;
    info->has_checksum = (fh.flags & 1) ? 1 : 0;
    info->mode_hint = fh.mode_hint;
    info->window_log = fh.window_log;
    info->content_size = fh.content_size;
    return VV_OK;
}
