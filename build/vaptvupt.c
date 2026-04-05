/* VaptVupt amalgamation — single-file build */
#include "vaptvupt.h"

/* ── src/vv_xxh64.c ── */
/*
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

/* ── src/vv_simd.c ── */
/*
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
    } else if (offset >= 4) {
        /* Moderate overlap: 8-byte copy with re-read */
        while (length >= 8) {
            uint64_t v;
            memcpy(&v, src, 8);
            memcpy(dst, &v, 8);
            dst += 8; src += 8; length -= 8;
        }
        while (length-- > 0) *dst++ = *src++;
    } else {
        /* Very short overlap (1-3): byte-by-byte */
        for (size_t i = 0; i < length; i++) dst[i] = src[i];
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

        if (__builtin_expect(len > 0, 1)) {
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

typedef struct { uint64_t a; int n; uint8_t *b; size_t p, c; } bw_t;

static inline void bw_init(bw_t *w, uint8_t *b, size_t c) {
    w->a = 0; w->n = 0; w->b = b; w->p = 0; w->c = c;
}
static inline void bw_add(bw_t *w, uint32_t v, int nb) {
    if (!nb) return;
    w->a |= (uint64_t)(v & ((1u << nb) - 1)) << w->n;
    w->n += nb;
    while (w->n >= 8 && w->p < w->c) {
        w->b[w->p++] = (uint8_t)w->a;
        w->a >>= 8;
        w->n -= 8;
    }
}
static inline size_t bw_flush(bw_t *w) {
    while (w->n > 0 && w->p < w->c) {
        w->b[w->p++] = (uint8_t)w->a;
        w->a >>= 8;
        w->n -= 8;
    }
    return w->p;
}

typedef struct { uint64_t a; int n; const uint8_t *s; size_t p, l; } br_t;

static inline void br_init(br_t *r, const uint8_t *s, size_t l) {
    r->a = 0; r->n = 0; r->s = s; r->p = 0; r->l = l;
}
static inline void br_fill(br_t *r) {
    while (r->n <= 56 && r->p < r->l) {
        r->a |= (uint64_t)r->s[r->p++] << r->n;
        r->n += 8;
    }
}
static inline uint32_t br_read(br_t *r, int nb) {
    if (!nb) return 0;
    if (r->n < nb) br_fill(r);
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
    t->spread = (uint8_t *)malloc(ANS_L);
    t->dec = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
    if (!t->spread || !t->dec) {
        free(t->spread); free(t->dec);
        t->spread = NULL; t->dec = NULL; t->enc = NULL;
        return -1;
    }
    spread_symbols(norm, t->spread);
    build_dec(norm, t->spread, t->dec);
    t->enc = build_enc(norm, t->spread, t->dec);
    if (!t->enc) {
        free(t->spread); free(t->dec);
        t->spread = NULL; t->dec = NULL;
        return -1;
    }
    return 0;
}

static void free_all(tables_t *t) {
    free(t->spread);
    free(t->dec);
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

    bitpair_t *pairs = (bitpair_t *)malloc(src_len * sizeof(bitpair_t));
    if (!pairs) { free_all(&t); return VVA_ERR_NOMEM; }

    uint32_t state = 0;
    for (size_t ii = src_len; ii > 0; ii--) {
        uint32_t bv; int bn;
        int slot = enc_sym(t.enc, state, src[ii - 1], &bv, &bn);
        if (slot < 0) { free_all(&t); free(pairs); return VVA_ERR_CORRUPT; }
        pairs[ii - 1].val = (uint32_t)bv;
        pairs[ii - 1].nb = (uint8_t)bn;
        state = (uint32_t)slot;
    }

    size_t bs_cap = (src_len * 15 + 7) / 8 + 16;
    uint8_t *bs = (uint8_t *)malloc(bs_cap);
    if (!bs) { free_all(&t); free(pairs); return VVA_ERR_NOMEM; }

    bw_t w;
    bw_init(&w, bs, bs_cap);
    for (size_t i = 0; i < src_len; i++)
        bw_add(&w, pairs[i].val, pairs[i].nb);
    size_t bs_len = bw_flush(&w);

    size_t total = hdr + 2 + bs_len;
    if (total > dst_cap || total >= src_len) {
        free_all(&t); free(pairs); free(bs);
        return VVA_ERR_OVERFLOW;
    }

    dst[hdr]     = (uint8_t)(state & 0xFF);
    dst[hdr + 1] = (uint8_t)((state >> 8) & 0xFF);
    memcpy(dst + hdr + 2, bs, bs_len);

    *dst_len = total;
    free_all(&t); free(pairs); free(bs);
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

    br_t r;
    br_init(&r, src + hdr + 2, src_len - hdr - 2);
    br_fill(&r);

    for (size_t i = 0; i < num_literals; i++) {
        if (r.n < ANS_LOG) br_fill(&r);
        vva_dec_entry_t e = dec[state];
        dst[i] = e.symbol;
        uint32_t bits = br_read(&r, e.nbits);
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
    uint8_t *bs_bufs[4] = {NULL, NULL, NULL, NULL};
    size_t bs_lens[4] = {0, 0, 0, 0};
    uint16_t states[4] = {0, 0, 0, 0};

    for (int lane = 0; lane < 4; lane++) {
        /* Count symbols in this lane */
        size_t lane_len = 0;
        for (size_t i = (size_t)lane; i < src_len; i += 4) lane_len++;
        if (lane_len == 0) continue;

        /* Collect bit-pairs for this lane */
        bitpair_t *pairs = (bitpair_t *)malloc(lane_len * sizeof(bitpair_t));
        if (!pairs) {
            for (int j = 0; j < lane; j++) free(bs_bufs[j]);
            free_all(&t); return VVA_ERR_NOMEM;
        }

        uint32_t state = 0;
        /* Encode backward within this lane */
        size_t ki = lane_len;
        for (size_t idx = (lane_len - 1) * 4 + (size_t)lane; ; idx -= 4) {
            ki--;
            if (idx >= src_len) { ki++; if (idx < 4) break; continue; }
            uint32_t bv; int bn;
            int slot = enc_sym(t.enc, state, src[idx], &bv, &bn);
            if (slot < 0) {
                free(pairs);
                for (int j = 0; j < lane; j++) free(bs_bufs[j]);
                free_all(&t); return VVA_ERR_CORRUPT;
            }
            pairs[ki].val = (uint32_t)bv;
            pairs[ki].nb = (uint8_t)bn;
            state = (uint32_t)slot;
            if (idx < 4) break;
        }

        /* Write bitstream for this lane */
        bs_bufs[lane] = (uint8_t *)malloc(bs_cap / 4 + 16);
        if (!bs_bufs[lane]) {
            free(pairs);
            for (int j = 0; j < lane; j++) free(bs_bufs[j]);
            free_all(&t); return VVA_ERR_NOMEM;
        }

        bw_t w;
        bw_init(&w, bs_bufs[lane], bs_cap / 4 + 16);
        for (size_t i = 0; i < lane_len; i++)
            bw_add(&w, pairs[i].val, pairs[i].nb);
        bs_lens[lane] = bw_flush(&w);
        states[lane] = (uint16_t)state;

        free(pairs);
    }

    free_all(&t);

    /* Output: [header] [4×2B states] [4×2B bs_lens] [bs0][bs1][bs2][bs3] */
    size_t overhead = hdr + 8 + 8; /* 4 states + 4 sizes (2B each) */
    size_t total_bs = bs_lens[0] + bs_lens[1] + bs_lens[2] + bs_lens[3];
    size_t total = overhead + total_bs;

    if (total > dst_cap || total >= src_len) {
        for (int i = 0; i < 4; i++) free(bs_bufs[i]);
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
        op[1] = (uint8_t)(bs_lens[i] >> 8);
        op += 2;
    }
    for (int i = 0; i < 4; i++) {
        memcpy(op, bs_bufs[i], bs_lens[i]);
        op += bs_lens[i];
        free(bs_bufs[i]);
    }

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

    /* Read 4 states + 4 bitstream sizes */
    const uint8_t *p = src + hdr;
    if (p + 16 > src + src_len) { free(dec); return VVA_ERR_CORRUPT; }

    uint32_t s[4];
    size_t bsz[4];
    for (int i = 0; i < 4; i++) {
        s[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        p += 2;
        if (s[i] >= (uint32_t)ANS_L) { free(dec); return VVA_ERR_CORRUPT; }
    }
    for (int i = 0; i < 4; i++) {
        bsz[i] = (size_t)p[0] | ((size_t)p[1] << 8);
        p += 2;
    }

    /* Set up 4 independent bit readers */
    br_t r[4];
    const uint8_t *bp = p;
    for (int i = 0; i < 4; i++) {
        if (bp + bsz[i] > src + src_len) { free(dec); return VVA_ERR_CORRUPT; }
        br_init(&r[i], bp, bsz[i]);
        br_fill(&r[i]);
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
        if (r[0].n < ANS_LOG) br_fill(&r[0]);
        s[0] = (uint32_t)e0.baseline + br_read(&r[0], e0.nbits);

        if (r[1].n < ANS_LOG) br_fill(&r[1]);
        s[1] = (uint32_t)e1.baseline + br_read(&r[1], e1.nbits);

        if (r[2].n < ANS_LOG) br_fill(&r[2]);
        s[2] = (uint32_t)e2.baseline + br_read(&r[2], e2.nbits);

        if (r[3].n < ANS_LOG) br_fill(&r[3]);
        s[3] = (uint32_t)e3.baseline + br_read(&r[3], e3.nbits);
    }

    /* Scalar tail for remaining 0-3 symbols */
    for (size_t i = full_quads * 4; i < num_literals; i++) {
        int lane = (int)(i & 3);
        if (r[lane].n < ANS_LOG) br_fill(&r[lane]);
        vva_dec_entry_t e = dec[s[lane]];
        dst[i] = e.symbol;
        s[lane] = (uint32_t)e.baseline + br_read(&r[lane], e.nbits);
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

    bw_t w;
    bw_init(&w, bs, bs_cap);
    for (size_t i = 0; i < src_len; i++)
        bw_add(&w, pairs[i].val, pairs[i].nb);
    size_t bs_len = bw_flush(&w);
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
        br_t r;
        br_init(&r, p, (size_t)(end - p));
        br_fill(&r);

        /* Decode forward with context tracking.
         * PERF: prefetch next context table to hide L2/L3 latency.
         * Each context table is 16KB. Without prefetch: ~50 MB/s (L3 thrash).
         * With prefetch: hides latency by 1 iteration → ~300+ MB/s. */
        uint8_t prev_ctx = 0;
        for (size_t i = 0; i < num_literals; i++) {
            if (r.n < ANS_LOG) br_fill(&r);

            uint32_t st = ctx_states[prev_ctx];
            if (st >= (uint32_t)ANS_L) goto ctx_dec_fail;

            vva_dec_entry_t e = ctx_dec[prev_ctx][st];
            dst[i] = e.symbol;

            uint32_t bits = br_read(&r, e.nbits);
            ctx_states[prev_ctx] = (uint16_t)((uint32_t)e.baseline + bits);

            prev_ctx = e.symbol;

            /* Prefetch next context's decode table into L2 cache.
             * The next iteration will access ctx_dec[prev_ctx][ctx_states[prev_ctx]].
             * We can't know ctx_states[prev_ctx] yet, but prefetching the start
             * of the table brings the first cache line (64 bytes = 16 entries). */
            __builtin_prefetch(&ctx_dec[prev_ctx][0], 0, 2);
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

/* Encode match length → (code, extra_value, extra_bits) */
static void ml_encode(uint32_t mlen, uint8_t *code, uint32_t *extra, int *nbits) {
    for (int c = VVA_ML_CODES - 1; c >= 0; c--) {
        if (mlen >= ml_base[c]) {
            *code = (uint8_t)c;
            *extra = mlen - ml_base[c];
            *nbits = ml_extra[c];
            return;
        }
    }
    *code = 0; *extra = 0; *nbits = 0;
}

/* Decode match length code → length */
static uint32_t ml_decode(uint8_t code, uint32_t extra) {
    return ml_base[code] + extra;
}

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

/* Write a varint to a buffer, return bytes written */
static size_t seq_write_varint(uint8_t *dst, size_t val) {
    size_t n = 0;
    while (val >= 255) { dst[n++] = 255; val -= 255; }
    dst[n++] = (uint8_t)val;
    return n;
}

/* Read a varint from a buffer, advance pointer */
static size_t seq_read_varint(const uint8_t **pp, const uint8_t *end) {
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
                               size_t *total_lits, int off_bytes) {
    const uint8_t *tp = tokens, *tp_end = tokens + tok_len;
    size_t nseq = 0, nlits = 0;

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

        size_t mlen = mc + 4; /* VV_MIN_MATCH = 4 */
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

vva_error_t vva_encode_sequences(const uint8_t *tokens, size_t tok_len,
                                  uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                  int off_bytes) {
    if (!tok_len) { *dst_len = 0; return VVA_OK; }

    /* Parse into sequences */
    size_t max_seqs = tok_len; /* Upper bound */
    seq_t *seqs = (seq_t *)malloc(max_seqs * sizeof(seq_t));
    uint8_t *lit_buf = (uint8_t *)malloc(tok_len);
    if (!seqs || !lit_buf) { free(seqs); free(lit_buf); return VVA_ERR_NOMEM; }

    size_t total_lits = 0;
    size_t nseq = parse_sequences(tokens, tok_len, lit_buf, tok_len, seqs, max_seqs, &total_lits, off_bytes);
    if (nseq == 0) { free(seqs); free(lit_buf); return VVA_ERR_CORRUPT; }

    /* ─── Encode literals with 4-way ANS ─── */
    size_t lit_cap = vva_bound(total_lits);
    uint8_t *lit_enc = (uint8_t *)malloc(lit_cap);
    if (!lit_enc) { free(seqs); free(lit_buf); return VVA_ERR_NOMEM; }

    size_t lit_enc_len = 0;
    uint8_t lit_fmt = 0; /* 0=raw, 1=ANS4, 2=ANS1 */
    if (total_lits > 0) {
        vva_error_t lit_err = vva_encode4(lit_buf, total_lits,
                                           lit_enc, lit_cap, &lit_enc_len);
        if (lit_err == VVA_OK) {
            lit_fmt = 1;
        } else {
            lit_err = vva_encode(lit_buf, total_lits,
                                  lit_enc, lit_cap, &lit_enc_len);
            if (lit_err == VVA_OK) {
                lit_fmt = 2;
            } else {
                /* Store raw */
                if (total_lits <= lit_cap) {
                    memcpy(lit_enc, lit_buf, total_lits);
                    lit_enc_len = total_lits;
                    lit_fmt = 0;
                }
            }
        }
    }

    /* ─── Count ML and OF code frequencies with rep-match tracking ─── */
    uint32_t freq_ml[VVA_ML_CODES], freq_of[VVA_OF_CODES];
    memset(freq_ml, 0, sizeof(freq_ml));
    memset(freq_of, 0, sizeof(freq_of));

    /* Precompute OF codes with rep-match detection (forward pass).
     * Store in per-sequence arrays so the backward ANS pass can use them. */
    uint8_t *seq_of_code = NULL;
    uint32_t *seq_of_extra = NULL;
    int *seq_of_nbits = NULL;
    seq_of_code = (uint8_t *)malloc(nseq * sizeof(uint8_t));
    seq_of_extra = (uint32_t *)malloc(nseq * sizeof(uint32_t));
    seq_of_nbits = (int *)malloc(nseq * sizeof(int));
    if (!seq_of_code || !seq_of_extra || !seq_of_nbits) {
        free(seq_of_code); free(seq_of_extra); free(seq_of_nbits);
        free(seqs); free(lit_buf); free(lit_enc);
        return VVA_ERR_NOMEM;
    }

    size_t match_count = 0;
    uint32_t enc_rep[3] = {0, 0, 0}; /* Rep-match tracking during forward pass */
    for (size_t i = 0; i < nseq; i++) {
        if (seqs[i].matchlen > 0) {
            uint8_t mc; uint32_t mx; int mn;
            ml_encode(seqs[i].matchlen, &mc, &mx, &mn);
            freq_ml[mc]++;

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
        }
    }

    /* ─── Build ML and OF ANS tables ─── */
    /* Normalize frequencies to sum=4096 for tables with ≤36/24 symbols */
    uint16_t norm_ml[NSYM], norm_of[NSYM];
    memset(norm_ml, 0, sizeof(norm_ml));
    memset(norm_of, 0, sizeof(norm_of));

    uint8_t *ml_hdr_buf = NULL, *of_hdr_buf = NULL;
    size_t ml_hdr_sz = 0, of_hdr_sz = 0;
    uint8_t *seq_bs = NULL;
    size_t seq_bs_len = 0;
    uint8_t *litlen_buf = NULL;
    size_t litlen_len = 0;
    uint32_t state_ml = 0, state_of = 0;

    if (match_count > 0) {
        /* Treat ML codes as a small-alphabet problem */
        uint32_t raw_ml[NSYM], raw_of[NSYM];
        memset(raw_ml, 0, sizeof(raw_ml));
        memset(raw_of, 0, sizeof(raw_of));
        for (int i = 0; i < VVA_ML_CODES; i++) raw_ml[i] = freq_ml[i];
        for (int i = 0; i < VVA_OF_CODES; i++) raw_of[i] = freq_of[i];

        normalize_freq(raw_ml, norm_ml);
        normalize_freq(raw_of, norm_of);

        /* Write ML and OF table headers */
        ml_hdr_buf = (uint8_t *)malloc(600);
        of_hdr_buf = (uint8_t *)malloc(600);
        if (!ml_hdr_buf || !of_hdr_buf) goto seq_fail;

        ml_hdr_sz = write_hdr_v2(norm_ml, ml_hdr_buf, 600);
        of_hdr_sz = write_hdr_v2(norm_of, of_hdr_buf, 600);
        if (!ml_hdr_sz || !of_hdr_sz) goto seq_fail;

        /* ─── Build encode tables ─── */
        uint8_t *sp_ml = (uint8_t *)malloc(ANS_L);
        vva_dec_entry_t *dec_ml = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        uint8_t *sp_of = (uint8_t *)malloc(ANS_L);
        vva_dec_entry_t *dec_of = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        if (!sp_ml || !dec_ml || !sp_of || !dec_of) {
            free(sp_ml); free(dec_ml); free(sp_of); free(dec_of);
            goto seq_fail;
        }

        spread_symbols(norm_ml, sp_ml);
        build_dec(norm_ml, sp_ml, dec_ml);
        enc_ctx_t *enc_ml_ctx = build_enc(norm_ml, sp_ml, dec_ml);
        free(sp_ml);

        spread_symbols(norm_of, sp_of);
        build_dec(norm_of, sp_of, dec_of);
        enc_ctx_t *enc_of_ctx = build_enc(norm_of, sp_of, dec_of);
        free(sp_of);

        free(dec_ml); free(dec_of);
        if (!enc_ml_ctx || !enc_of_ctx) {
            free_enc(enc_ml_ctx); free_enc(enc_of_ctx);
            goto seq_fail;
        }

        /* ─── Encode ML/OF codes + extra bits in reverse ─── */
        /* Collect bitpairs for ANS-coded symbols + raw extra bits */
        size_t pair_cap = match_count * 4; /* 2 ANS + 2 extra max */
        bitpair_t *pairs = (bitpair_t *)malloc(pair_cap * sizeof(bitpair_t));
        if (!pairs) { free_enc(enc_ml_ctx); free_enc(enc_of_ctx); goto seq_fail; }

        state_ml = 0; state_of = 0;
        size_t npairs = 0;

        /* Process matches in reverse for ANS LIFO */
        for (size_t ii = nseq; ii > 0; ii--) {
            if (seqs[ii - 1].matchlen == 0) continue;

            uint8_t mc;
            uint32_t mx;
            int mn;
            ml_encode(seqs[ii - 1].matchlen, &mc, &mx, &mn);

            /* Use precomputed OF code from forward pass (rep-match aware) */
            uint8_t oc = seq_of_code[ii - 1];
            uint32_t ox = seq_of_extra[ii - 1];
            int on = seq_of_nbits[ii - 1];

            /* Encode in this order (reversed): ml_code, ml_extra, of_code, of_extra
             * Decoder reads: of_extra, of_code, ml_extra, ml_code */

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
                /* Handle >16 extra bits for large offsets */
                if (on > 16) {
                    /* Split: already wrote low 16 bits, now high bits */
                    /* Actually our bw_add handles up to ~30 bits, so OK */
                }
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

        free_enc(enc_ml_ctx); free_enc(enc_of_ctx);

        /* Write pairs in reverse (so decoder reads forward) */
        size_t bs_cap = npairs * 2 + 16;
        seq_bs = (uint8_t *)malloc(bs_cap);
        if (!seq_bs) { free(pairs); goto seq_fail; }

        bw_t w;
        bw_init(&w, seq_bs, bs_cap);
        for (size_t i = npairs; i > 0; i--)
            bw_add(&w, pairs[i - 1].val, pairs[i - 1].nb);
        seq_bs_len = bw_flush(&w);
        free(pairs);
    }

    /* ─── Encode litlen varints ─── */
    litlen_buf = (uint8_t *)malloc(nseq * 5 + 1);
    if (!litlen_buf) goto seq_fail;
    {
        size_t pos = 0;
        for (size_t i = 0; i < nseq; i++)
            pos += seq_write_varint(litlen_buf + pos, seqs[i].litlen);
        litlen_len = pos;
    }

    /* ─── Assemble output ─── */
    /* Format: [4B lit_count] [1B lit_fmt] [4B lit_enc_len] [lit_data]
     *         [4B match_count]
     *         [2B ml_hdr_sz] [ml_hdr] [2B of_hdr_sz] [of_hdr]
     *         [2B state_ml] [2B state_of]
     *         [4B seq_bs_len] [seq_bs]
     *         [litlen_varints] */
    {
        size_t total = 9 + lit_enc_len + 4 + 4 + ml_hdr_sz + 4 + of_hdr_sz
                     + 4 + 4 + seq_bs_len + litlen_len;

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

        /* States */
        op[0] = (uint8_t)(state_ml & 0xFF); op[1] = (uint8_t)((state_ml >> 8) & 0xFF); op += 2;
        op[0] = (uint8_t)(state_of & 0xFF); op[1] = (uint8_t)((state_of >> 8) & 0xFF); op += 2;

        /* Sequence bitstream (4B size) */
        op[0]=(uint8_t)seq_bs_len; op[1]=(uint8_t)(seq_bs_len>>8);
        op[2]=(uint8_t)(seq_bs_len>>16); op[3]=(uint8_t)(seq_bs_len>>24); op+=4;
        if (seq_bs_len > 0) { memcpy(op, seq_bs, seq_bs_len); op += seq_bs_len; }

        /* Litlen varints */
        memcpy(op, litlen_buf, litlen_len); op += litlen_len;

        *dst_len = (size_t)(op - dst);
    }

    free(seqs); free(lit_buf); free(lit_enc);
    free(seq_of_code); free(seq_of_extra); free(seq_of_nbits);
    free(ml_hdr_buf); free(of_hdr_buf); free(seq_bs); free(litlen_buf);
    return VVA_OK;

seq_fail:
    free(seqs); free(lit_buf); free(lit_enc);
    free(seq_of_code); free(seq_of_extra); free(seq_of_nbits);
    free(ml_hdr_buf); free(of_hdr_buf); free(seq_bs); free(litlen_buf);
    return VVA_ERR_OVERFLOW;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE SEQUENCES
 *
 * Takes ANS-coded sequence block, outputs decompressed data.
 * Reconstructs LZ matches in-place using existing copy logic.
 * ═══════════════════════════════════════════════════════════════ */

vva_error_t vva_decode_sequences(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                  const uint8_t *dst_base) {
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

    /* Read initial states */
    if (p + 4 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    uint32_t state_ml = (uint32_t)p[0] | ((uint32_t)p[1] << 8); p += 2;
    uint32_t state_of = (uint32_t)p[0] | ((uint32_t)p[1] << 8); p += 2;

    /* Read sequence bitstream (4B size) */
    if (p + 4 > end) { free(lit_buf); return VVA_ERR_CORRUPT; }
    size_t seq_bs_len = (size_t)p[0]|((size_t)p[1]<<8)|((size_t)p[2]<<16)|((size_t)p[3]<<24); p += 4;
    if (p + seq_bs_len > end) { free(lit_buf); return VVA_ERR_CORRUPT; }

    /* Build ML and OF decode tables */
    vva_dec_entry_t *dec_ml = NULL, *dec_of = NULL;
    if (match_count > 0) {
        uint8_t *sp_tmp = (uint8_t *)malloc(ANS_L);
        dec_ml = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        dec_of = (vva_dec_entry_t *)malloc(ANS_L * sizeof(vva_dec_entry_t));
        if (!sp_tmp || !dec_ml || !dec_of) {
            free(sp_tmp); free(dec_ml); free(dec_of); free(lit_buf);
            return VVA_ERR_NOMEM;
        }
        spread_symbols(norm_ml, sp_tmp);
        build_dec(norm_ml, sp_tmp, dec_ml);
        spread_symbols(norm_of, sp_tmp);
        build_dec(norm_of, sp_tmp, dec_of);
        free(sp_tmp);
    }

    /* Initialize bitstream reader for sequence data */
    br_t r;
    br_init(&r, p, seq_bs_len);
    br_fill(&r);
    p += seq_bs_len;

    /* Litlen varint stream starts at p */
    const uint8_t *ll_p = p;

    /* ─── PERF: Decode loop — reconstruct output ─── */
    uint8_t *op = dst;
    uint8_t *op_end = dst + dst_cap;
    size_t lit_pos = 0;
    size_t matches_decoded = 0;
    uint32_t dec_rep[3] = {0, 0, 0}; /* Rep-match offset tracking */

    while (lit_pos < total_lits || matches_decoded < match_count) {
        size_t litlen = seq_read_varint(&ll_p, end);

        if (lit_pos + litlen > total_lits) { free(dec_ml); free(dec_of); free(lit_buf); return VVA_ERR_CORRUPT; }
        if (op + litlen > op_end) { free(dec_ml); free(dec_of); free(lit_buf); return VVA_ERR_OVERFLOW; }
        if (litlen > 0) {
            memcpy(op, lit_buf + lit_pos, litlen);
            op += litlen;
            lit_pos += litlen;
        }

        if (matches_decoded >= match_count) break;

        /* Decode OF code */
        if (r.n < ANS_LOG) br_fill(&r);
        if (state_of >= (uint32_t)ANS_L) { free(dec_ml); free(dec_of); free(lit_buf); return VVA_ERR_CORRUPT; }
        vva_dec_entry_t eof = dec_of[state_of];
        uint32_t of_bits = br_read(&r, eof.nbits);
        state_of = (uint32_t)eof.baseline + of_bits;

        /* Resolve offset: codes 0-2 = rep-match, 3+ = explicit */
        uint8_t of_code = eof.symbol;
        uint32_t offset;
        if (of_code < 3) {
            offset = dec_rep[of_code];
        } else {
            uint32_t of_extra_val = 0;
            if (of_code < VVA_OF_CODES && of_extra[of_code] > 0) {
                of_extra_val = br_read(&r, of_extra[of_code]);
            }
            offset = of_decode(of_code, of_extra_val);
        }
        /* Update rep offsets */
        if (offset != 0 && offset != dec_rep[0]) {
            dec_rep[2] = dec_rep[1]; dec_rep[1] = dec_rep[0]; dec_rep[0] = offset;
        }

        /* Decode match length */
        if (r.n < ANS_LOG) br_fill(&r);
        if (state_ml >= (uint32_t)ANS_L) { free(dec_ml); free(dec_of); free(lit_buf); return VVA_ERR_CORRUPT; }
        vva_dec_entry_t eml = dec_ml[state_ml];
        uint32_t ml_bits = br_read(&r, eml.nbits);
        state_ml = (uint32_t)eml.baseline + ml_bits;

        /* Read matchlen extra bits */
        uint8_t ml_code = eml.symbol;
        uint32_t ml_extra_val = 0;
        if (ml_code < VVA_ML_CODES && ml_extra[ml_code] > 0) {
            ml_extra_val = br_read(&r, ml_extra[ml_code]);
        }
        uint32_t matchlen = ml_decode(ml_code, ml_extra_val);

        /* Validate and execute match copy */
        if (offset == 0 || offset > (uint32_t)(op - dst_base)) {
            free(dec_ml); free(dec_of); free(lit_buf);
            return VVA_ERR_CORRUPT;
        }
        if (op + matchlen > op_end) {
            free(dec_ml); free(dec_of); free(lit_buf);
            return VVA_ERR_OVERFLOW;
        }

        /* PERF: match copy — use SIMD tiered copy when available.
         * ZUPT-COMPAT: standalone path uses scalar copy for portability. */
#ifdef VV_ANS_STANDALONE
        {
            const uint8_t *match_src = op - offset;
            for (uint32_t j = 0; j < matchlen; j++)
                op[j] = match_src[j];
        }
#else
        vv_copy_match(op, offset, matchlen);
#endif
        op += matchlen;

        matches_decoded++;
    }

    *dst_len = (size_t)(op - dst);
    free(dec_ml); free(dec_of); free(lit_buf);
    return VVA_OK;
}

/* ── src/vv_encoder.c ── */
/*
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
    __builtin_memcpy(&lo, p, 4);
    uint64_t v = (uint64_t)lo | ((uint64_t)p[4] << 32);
    /* Shift by (64 - HC_BITS) to get the top HC_BITS of the product */
    return (uint32_t)((v * 889523592379ULL) >> (64 - VV_HC_BITS));
}

/* 4-byte hash for positions near end of buffer */
static inline uint32_t hash4(const uint8_t *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
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
#if VV_ENC_AVX2
    while (len + 32 <= max_len) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(a + len));
        __m256i vb = _mm256_loadu_si256((const __m256i *)(b + len));
        __m256i eq = _mm256_cmpeq_epi8(va, vb);
        uint32_t mask = ~(uint32_t)_mm256_movemask_epi8(eq);
        if (mask) return len + (int32_t)__builtin_ctz(mask);
        len += 32;
    }
#endif
    while (len < max_len && a[len] == b[len]) len++;
    return len;
}

/* ═══════════════════════════════════════════════════════════════
 * MATCHER: hash chain with 5-byte hash + rep-match
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    int32_t *table;    /* Hash table: VV_HC_SIZE entries, heap-allocated */
    int32_t *chain;    /* Chain array: window_size entries */
    uint32_t chain_mask;
    uint32_t chain_depth;
    uint32_t rep[3];   /* 3 most recent match offsets */
    uint8_t  wlog;     /* Window log: controls max offset distance */
} matcher_t;

static void matcher_init(matcher_t *m, uint32_t window_log, uint32_t depth) {
    uint32_t wsz = 1u << window_log;
    m->table = (int32_t *)malloc(VV_HC_SIZE * sizeof(int32_t));
    m->chain = (int32_t *)malloc(wsz * sizeof(int32_t));
    memset(m->table, 0xFF, VV_HC_SIZE * sizeof(int32_t));  /* -1 */
    memset(m->chain, 0xFF, wsz * sizeof(int32_t));          /* -1 */
    m->chain_mask = wsz - 1;
    m->chain_depth = depth;
    m->rep[0] = m->rep[1] = m->rep[2] = 0;
    m->wlog = (uint8_t)window_log;
}

static void matcher_free(matcher_t *m) {
    free(m->table); m->table = NULL;
    free(m->chain); m->chain = NULL;
}

static inline void matcher_insert(matcher_t *m, const uint8_t *data,
                                   int32_t pos, int32_t end) {
    if (pos + 4 > end) return;
    uint32_t h = hash_safe(data + pos, end - pos);
    m->chain[pos & m->chain_mask] = m->table[h];
    m->table[h] = pos;
}

/* ─── Rep-match check: O(1), checked BEFORE hash probe ─── */
static inline int32_t try_rep_match(const matcher_t *m, const uint8_t *data,
                                     int32_t pos, int32_t end,
                                     int32_t *rep_idx) {
    for (int i = 0; i < 3; i++) {
        uint32_t d = m->rep[i];
        if (d == 0 || (uint32_t)pos < d) continue;
        int32_t ref = pos - (int32_t)d;
        /* Quick 4-byte check */
        uint32_t a, b;
        __builtin_memcpy(&a, data + pos, 4);
        __builtin_memcpy(&b, data + ref, 4);
        if (a == b) {
            int32_t max = end - pos;
            if (max > VV_MAX_MATCH) max = VV_MAX_MATCH;
            int32_t len = 4 + extend_match(data + pos + 4, data + ref + 4, max - 4);
            *rep_idx = i;
            return len;
        }
    }
    return 0;
}

/* ─── Hash chain match: uses 5-byte hash, searches up to chain_depth ─── */
static int32_t chain_match(const matcher_t *m, const uint8_t *data,
                            int32_t pos, int32_t end, int32_t *best_off) {
    if (pos + 4 > end) return 0;
    uint32_t h = hash_safe(data + pos, end - pos);
    int32_t ref = m->table[h];
    int32_t best_len = 0;
    *best_off = 0;

    uint32_t depth = m->chain_depth;
    /* PERF: match distance limit derived from window log.
     * wlog=16 → 65535, wlog=20 → 1048575, wlog=22 → 4194303. */
    int32_t max_dist = (int32_t)((1u << m->wlog) - 1);
    int32_t limit = pos - max_dist;
    if (limit < 0) limit = 0;

    while (ref >= 0 && ref >= limit && ref < pos && depth-- > 0) {
        /* Quick 4-byte prefix check */
        uint32_t a, b;
        __builtin_memcpy(&a, data + pos, 4);
        __builtin_memcpy(&b, data + ref, 4);
        if (a == b) {
            int32_t max = end - pos;
            if (max > VV_MAX_MATCH) max = VV_MAX_MATCH;
            int32_t len = 4 + extend_match(data + pos + 4, data + ref + 4, max - 4);
            if (len > best_len) {
                best_len = len;
                *best_off = pos - ref;
                if (len >= 256) break; /* good enough */
            }
        }
        ref = m->chain[ref & m->chain_mask];
    }
    return best_len;
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
                        size_t ll, size_t ml, uint32_t off, int off_bytes) {
    uint8_t *op = dst;

    uint8_t ll_f = (ll >= 15) ? 15 : (uint8_t)ll;
    uint8_t ml_f;
    if (ml == 0) { ml_f = 0; }
    else { size_t v = ml - VV_MIN_MATCH; ml_f = (v >= 15) ? 15 : (uint8_t)v; }

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
        if (ml - VV_MIN_MATCH >= 15)
            op += write_varint(op, ml - VV_MIN_MATCH - 15);
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
                             matcher_t *m, vv_mode_t mode) {
    uint8_t *op = dst;
    int32_t pos = (int32_t)start_pos;
    int32_t end = (int32_t)(start_pos + block_len);
    const uint8_t *lit_start = src + start_pos;
    int off_bytes = (m->wlog > 16) ? 3 : 2;

    while (pos < end - (int32_t)VV_MIN_MATCH) {
        int32_t mlen = 0, moff = 0;

        /* ─── Step 1: Try rep-match (free, no hash lookup) ─── */
        int32_t rep_idx = -1;
        int32_t rep_len = try_rep_match(m, src, pos, end, &rep_idx);

        if (rep_len >= (int32_t)VV_MIN_MATCH) {
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
        if (mode >= VV_MODE_BALANCED && mlen >= (int32_t)VV_MIN_MATCH &&
            pos + 1 < end - (int32_t)VV_MIN_MATCH) {
            /* Check pos+1 */
            matcher_insert(m, src, pos, end);
            int32_t noff = 0;
            int32_t nlen = chain_match(m, src, pos + 1, end, &noff);

            /* Also check rep at pos+1 */
            int32_t nri = -1;
            int32_t nrl = try_rep_match(m, src, pos + 1, end, &nri);
            if (nrl > nlen) { nlen = nrl; noff = (int32_t)m->rep[nri]; }

            if (nlen > mlen + 1) {
                /* pos+1 is significantly better: emit literal, shift */
                pos++;
                mlen = nlen; moff = noff;

                /* Lazy-2: also check pos+2 (extreme mode) */
                if (mode >= VV_MODE_EXTREME && pos + 1 < end - (int32_t)VV_MIN_MATCH) {
                    matcher_insert(m, src, pos, end);
                    int32_t n2off = 0;
                    int32_t n2len = chain_match(m, src, pos + 1, end, &n2off);
                    int32_t n2ri = -1;
                    int32_t n2rl = try_rep_match(m, src, pos + 1, end, &n2ri);
                    if (n2rl > n2len) { n2len = n2rl; n2off = (int32_t)m->rep[n2ri]; }
                    if (n2len > mlen + 1) {
                        pos++;
                        mlen = n2len; moff = n2off;
                    }
                }
            }
        }

        /* ─── Step 4: Emit sequence or literal ─── */
        if (mlen >= (int32_t)VV_MIN_MATCH) {
            size_t ll = (size_t)(src + pos - lit_start);
            size_t needed = 1 + (ll >= 15 ? ll / 255 + 2 : 0)
                          + ll + 2 + ((size_t)mlen / 255 + 2);
            if ((size_t)(op - dst) + needed > dst_cap) return 0;

            op += emit_seq(op, lit_start, ll, (size_t)mlen, (uint32_t)moff, off_bytes);

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
        op += emit_seq(op, lit_start, ll, 0, 0, off_bytes);
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
    case VV_MODE_BALANCED:   depth = 48; break;
    case VV_MODE_EXTREME:    depth = 256; break;
    default: depth = 48;
    }

    /* ─── ADAPTIVE WINDOW: sample first 64KB at wlog=16 vs wlog=20.
     * PERF: only samples 64KB (not full 1MB block) — 16× faster trial.
     * If wlog=20 saves ≥3%, use wider window for the whole frame. ─── */
    if (opts->window_log == 0 && opts->mode >= VV_MODE_BALANCED && src_len > 65536) {
        size_t trial_len = 262144; /* Sample 256KB — catches patterns up to 200KB apart */
        if (trial_len > src_len) trial_len = src_len;

        size_t trial_cap = trial_len + trial_len / 255 + 1024;
        uint8_t *trial_buf = (uint8_t *)malloc(trial_cap);
        if (trial_buf) {
            /* PERF: use greedy depth=4 for trials — 10× faster than lazy-48 */
            matcher_t m16; matcher_init(&m16, 16, 4);
            size_t sz16 = compress_block(src, 0, trial_len, trial_buf, trial_cap, &m16, VV_MODE_ULTRA_FAST);
            matcher_free(&m16);

            matcher_t m20; matcher_init(&m20, 20, 4);
            size_t sz20 = compress_block(src, 0, trial_len, trial_buf, trial_cap, &m20, VV_MODE_ULTRA_FAST);
            matcher_free(&m20);

            free(trial_buf);
            if (sz20 > 0 && sz16 > 0 && sz20 < (sz16 * 97 / 100)) wlog = 20;
        }
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

    /* Temp buffer */
    size_t tcap = VV_MAX_BLOCK_SIZE + VV_MAX_BLOCK_SIZE / 255 + 1024;
    uint8_t *tmp = (uint8_t *)malloc(tcap);
    if (!tmp) { matcher_free(&m); return VV_ERR_NOMEM; }

    /* Additional buffers for entropy path (only allocated if needed) */
    uint8_t *lit_buf = NULL, *stripped = NULL, *ent_buf = NULL;
    size_t lit_cap = 0, ent_cap = 0;
    if (opts->mode >= VV_MODE_BALANCED) {
        lit_cap = VV_MAX_BLOCK_SIZE;
        ent_cap = vva_bound(VV_MAX_BLOCK_SIZE);
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

    while (remaining > 0) {
        size_t braw = remaining > VV_MAX_BLOCK_SIZE ? VV_MAX_BLOCK_SIZE : remaining;
        int last = (remaining <= VV_MAX_BLOCK_SIZE);

        size_t block_start = (size_t)(ip - src);
        size_t csz = compress_block(src, block_start, braw, tmp, tcap, &m, opts->mode);

        if (csz == 0 || csz >= braw) {
            /* Incompressible: store raw */
            uint32_t bh = vv_bh_pack(VV_BLOCK_RAW, last, (uint32_t)braw);
            memcpy(op, &bh, 4); op += 4;
            memcpy(op, ip, braw); op += braw;
        } else if (opts->mode >= VV_MODE_BALANCED) {
            /* ═══ WINNER-TAKES-ALL block selection ═══
             * TRADEOFF: we encode the block twice (once 'S', once 'I'/'C')
             * and pick the smaller. This costs ~2× encode time but ensures
             * we NEVER regress ratio vs any previous codec version.
             * Encode speed is not the bottleneck (decode is). */

            /* ── Path A: sequence coding ('S') ── */
            size_t seq_len = 0;
            int seq_valid = 0;
            size_t seq_block_sz = (size_t)-1; /* Total bytes if we emit 'S' */
            int off_bytes = (wlog > 16) ? 3 : 2;
            vva_error_t serr = vva_encode_sequences(tmp, csz,
                                                     ent_buf, ent_cap, &seq_len, off_bytes);
            if (serr == VVA_OK) {
                seq_block_sz = 4 + 3 + 1 + seq_len; /* block_hdr + comp_sz + tag + data */
                seq_valid = 1;
            }

            /* ── Path B: literal-only entropy ('I' or 'C') ── */
            size_t stripped_len = 0;
            size_t lit_count = extract_literals(tmp, csz, lit_buf, lit_cap,
                                                stripped, &stripped_len, off_bytes);

            /* Use second half of ent_buf for path B to avoid overwriting path A */
            uint8_t *ent_buf2 = ent_buf + ent_cap / 2;
            size_t ent_cap2 = ent_cap / 2;
            size_t ent_len = 0;
            uint8_t ent_tag = 0;
            size_t ent_block_sz = (size_t)-1;

            if (lit_count > 0) {
                if (opts->mode >= VV_MODE_EXTREME && lit_count >= 64) {
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

            /* ── Path C: raw type-1 block ── */
            size_t raw_block_sz = 4 + 3 + csz;

            /* ── Pick winner ── */
            if (seq_valid && seq_block_sz <= ent_block_sz && seq_block_sz < raw_block_sz) {
                /* 'S' wins — emit sequence-coded block */
                uint32_t bh = vv_bh_pack(VV_BLOCK_ENTROPY, last, (uint32_t)braw);
                memcpy(op, &bh, 4); op += 4;
                uint32_t total_comp = (uint32_t)(1 + seq_len);
                op[0] = (uint8_t)(total_comp);
                op[1] = (uint8_t)(total_comp >> 8);
                op[2] = (uint8_t)(total_comp >> 16);
                op += 3;
                *op++ = VV_ENTROPY_SEQ;
                memcpy(op, ent_buf, seq_len); op += seq_len;
            } else if (ent_tag && ent_block_sz < raw_block_sz) {
                /* 'I'/'C' wins — emit literal-entropy block */
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
            } else {
                /* Raw type-1 wins (or nothing compresses) */
                uint32_t bh = vv_bh_pack(VV_BLOCK_COMPRESSED, last, (uint32_t)braw);
                memcpy(op, &bh, 4); op += 4;
                op[0] = (uint8_t)(csz); op[1] = (uint8_t)(csz >> 8); op[2] = (uint8_t)(csz >> 16);
                op += 3;
                memcpy(op, tmp, csz); op += csz;
            }
        } else {
            /* Ultra-fast mode: emit type 1 block directly */
            uint32_t bh = vv_bh_pack(VV_BLOCK_COMPRESSED, last, (uint32_t)braw);
            memcpy(op, &bh, 4); op += 4;
            op[0] = (uint8_t)(csz); op[1] = (uint8_t)(csz >> 8); op[2] = (uint8_t)(csz >> 16);
            op += 3;
            memcpy(op, tmp, csz); op += csz;
        }
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

/* ── src/vv_decoder.c ── */
/*
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
    if (n > 0) __builtin_memcpy(d, s, n);
}

/* Match copy with offset 16-31: 16-byte chunks, exact tail */
static inline void match_copy_16(uint8_t *d, const uint8_t *s, size_t n) {
    while (n >= 16) { wcopy16(d, s); d += 16; s += 16; n -= 16; }
    if (n > 0) __builtin_memcpy(d, s, n);
}

/* Match copy with offset 8-15: 8-byte register copy */
static inline void match_copy_8(uint8_t *d, uint32_t off, size_t n) {
    const uint8_t *s = d - off;
    while (n >= 8) {
        uint64_t v; __builtin_memcpy(&v, s, 8);
        __builtin_memcpy(d, &v, 8);
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

static vv_error_t decode_block_tokens(
    const uint8_t *ip, size_t ip_len,
    uint8_t *op, size_t dst_cap, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)  /* Base of full output buffer for cross-block offset validation */
{
    const uint8_t *const ip_end = ip + ip_len;
    uint8_t *const op_start = op;
    uint8_t *const op_end = op + dst_cap;

    /* Safe zone boundaries: skip per-op bounds checks while inside.
     * Guard against underflow: if block is smaller than margin, skip fast path. */
    const uint8_t *const ip_safe = (ip_len > 24) ? (ip_end - 24) : ip;
    uint8_t *const op_safe = (dst_cap > 40) ? (op_end - 40) : op;

#if VV_INLINE_AVX2
    /* ═══ AVX2 FAST PATH ═══
     *
     * Runs while both ip and op are in the safe zone.
     * No per-byte bounds checks. Inline SIMD copies.
     * Prefetch match source at offset-load time.
     *
     * Per-sequence cost (common case, litlen≤14, matchlen≤18):
     *   token load + decode:   3 cycles
     *   early offset load:     4 cycles (overlapped)
     *   prefetch:              0 cycles (non-blocking)
     *   literal wcopy16:       5 cycles
     *   match wcopy32:         5 cycles
     *   pointer advance:       2 cycles
     *   loop branch:           0 cycles (predicted)
     *   ─────────────────────────────────
     *   Total: ~10 cycles for ~12 output bytes → 1.2 bytes/cycle
     *   At 4 GHz: ~4.8 GB/s (theoretical, real ~2-3 GB/s with cache)
     */
    while (__builtin_expect(ip < ip_safe && op < op_safe, 1)) {

        uint32_t token = *ip++;
        uint32_t ll = token >> 4;
        uint32_t mc = token & 0x0F;

        /* Extended literal length → cold path */
        if (__builtin_expect(ll == 15, 0))
            ll += (uint32_t)read_ext_len(&ip, ip_end);

        /* ── Early offset load + prefetch ──
         * The offset is at ip+ll (after the literal bytes).
         * Only do this for small litlen where we know ip+ll+2 is in the safe zone.
         * The safe-zone margin (24) guarantees: token(1) + lits(≤14) + offset(2) +
         * match_ext(≤6) + margin ≤ 24. */
        if (__builtin_expect(ll <= 14 && ip + ll + 2 <= ip_end, 1)) {
            uint16_t off_raw;
            __builtin_memcpy(&off_raw, ip + ll, 2);
            if (off_raw != 0 && off_raw <= (uint32_t)(op + ll - op_start))
                __builtin_prefetch(op + ll - off_raw, 0, 1);
        }

        /* ── Literal copy (EXACT — no wild over-copy) ──
         * Wild-copy writes garbage past op+ll that corrupts positions
         * referenced by future matches. Must use exact-length copies.
         * memcpy compiles to optimal SIMD for small constant-like sizes. */
        if (ll > 0)
            __builtin_memcpy(op, ip, ll);
        ip += ll;
        op += ll;

        /* ── End of block ── */
        if (__builtin_expect(ip >= ip_end, 0)) break;

        /* ── Offset ── */
        uint32_t offset = (off_bytes == 3) ? ((uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16)) : vv_read16(ip);
        ip += off_bytes;

        /* ── Match length ── */
        uint32_t mlen = mc + VV_MIN_MATCH;
        if (__builtin_expect(mc == 15, 0))
            mlen += (uint32_t)read_ext_len(&ip, ip_end);

        /* ── Validate offset ── */
        if (__builtin_expect(offset == 0 || offset > (uint32_t)(op - dst_base), 0))
            return VV_ERR_CORRUPT;

        /* ── Match copy (inline AVX2, tiered by offset) ── */
        if (__builtin_expect(offset >= 32, 1)) {
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
#endif /* VV_INLINE_AVX2 */

    /* ═══ GENERAL PATH (tail + non-AVX2) ═══ */
    while (ip < ip_end) {
        uint8_t token = *ip++;
        size_t ll = token >> 4;
        size_t mc = token & 0x0F;

        if (__builtin_expect(ll == 15, 0))
            ll += read_ext_len(&ip, ip_end);

        if (__builtin_expect(ip + ll > ip_end, 0)) return VV_ERR_CORRUPT;
        if (__builtin_expect(op + ll > op_end, 0)) return VV_ERR_OVERFLOW;

        if (ll > 0) vv_copy_fast(op, ip, ll);
        ip += ll;
        op += ll;

        if (ip >= ip_end) break;

        if (__builtin_expect(ip + off_bytes > ip_end, 0)) return VV_ERR_CORRUPT;
        uint32_t offset = (off_bytes == 3) ? ((uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16)) : vv_read16(ip);
        ip += off_bytes;

        size_t mlen = mc + VV_MIN_MATCH;
        if (__builtin_expect(mc == 15, 0))
            mlen += read_ext_len(&ip, ip_end);

        if (__builtin_expect(offset == 0 || offset > (uint32_t)(op - dst_base), 0))
            return VV_ERR_CORRUPT;
        if (__builtin_expect(op + mlen > op_end, 0))
            return VV_ERR_OVERFLOW;

        vv_copy_match(op, offset, mlen);
        op += mlen;
    }

    *out_len = (size_t)(op - op_start);
    return VV_OK;
}

/* ═══════════════════════════════════════════════════════════════
 * DECODE STRIPPED TOKEN STREAM (for type 3 / Huffman blocks)
 *
 * Same as decode_block_tokens but literal bytes are NOT inline.
 * Instead, they come from a pre-decoded literal buffer.
 * Token format: same headers/offsets/extensions, just no literal bytes.
 * ═══════════════════════════════════════════════════════════════ */

static vv_error_t decode_stripped_tokens(
    const uint8_t *ip, size_t ip_len,       /* Stripped token stream */
    const uint8_t *lit_buf, size_t lit_len,  /* Pre-decoded literals */
    uint8_t *op, size_t dst_cap, size_t *out_len, int off_bytes,
    const uint8_t *dst_base)
{
    const uint8_t *ip_end = ip + ip_len;
    uint8_t *op_start = op;
    uint8_t *op_end = op + dst_cap;
    size_t lit_pos = 0;

    while (ip < ip_end) {
        uint8_t token = *ip++;
        size_t ll = token >> 4;
        size_t mc = token & 0x0F;

        /* Extended literal length */
        if (__builtin_expect(ll == 15, 0))
            ll += read_ext_len(&ip, ip_end);

        /* Copy literals from pre-decoded buffer */
        if (__builtin_expect(lit_pos + ll > lit_len, 0)) return VV_ERR_CORRUPT;
        if (__builtin_expect(op + ll > op_end, 0)) return VV_ERR_OVERFLOW;
        if (ll > 0) {
            memcpy(op, lit_buf + lit_pos, ll);
            lit_pos += ll;
        }
        op += ll;

        /* End of block: last sequence has no match */
        if (ip >= ip_end) break;

        /* Offset */
        if (__builtin_expect(ip + off_bytes > ip_end, 0)) return VV_ERR_CORRUPT;
        uint32_t offset = (off_bytes == 3) ? ((uint32_t)ip[0] | ((uint32_t)ip[1]<<8) | ((uint32_t)ip[2]<<16)) : vv_read16(ip);
        ip += off_bytes;

        /* Match length */
        size_t mlen = mc + VV_MIN_MATCH;
        if (__builtin_expect(mc == 15, 0))
            mlen += read_ext_len(&ip, ip_end);

        /* Validate */
        if (__builtin_expect(offset == 0 || offset > (uint32_t)(op - dst_base), 0))
            return VV_ERR_CORRUPT;
        if (__builtin_expect(op + mlen > op_end, 0))
            return VV_ERR_OVERFLOW;

        /* Match copy */
        vv_copy_match(op, offset, mlen);
        op += mlen;
    }

    *out_len = (size_t)(op - op_start);
    return VV_OK;
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
    if (!src || !dst) return VV_ERR_PARAM;
    if (src_len < sizeof(vv_frame_header_t)) return VV_ERR_CORRUPT;

    const uint8_t *ip = src;
    const uint8_t *ip_end = src + src_len;

    vv_frame_header_t fh;
    memcpy(&fh, ip, sizeof(fh));
    ip += sizeof(fh);

    if (fh.magic != VV_MAGIC) return VV_ERR_BAD_MAGIC;
    if (fh.version != 1) return VV_ERR_CORRUPT;

    int has_checksum = (fh.flags & 1);
    int off_bytes = (fh.window_log > 16) ? 3 : 2;
    uint8_t *op = dst;

    for (;;) {
        if (ip + 4 > ip_end) return VV_ERR_CORRUPT;
        uint32_t bh_packed;
        memcpy(&bh_packed, ip, 4); ip += 4;

        vv_block_type_t btype = vv_bh_type(bh_packed);
        int is_last = vv_bh_last(bh_packed);
        uint32_t dsz = vv_bh_size(bh_packed);

        if (dsz > VV_MAX_BLOCK_SIZE) return VV_ERR_OVERFLOW;
        if ((size_t)(op - dst) + dsz > dst_cap) return VV_ERR_OVERFLOW;

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
            vv_error_t err = decode_block_tokens(ip, csz, op, dsz, &actual, off_bytes, dst);
            if (err != VV_OK) return err;
            if (actual != dsz) return VV_ERR_CORRUPT;
            ip += csz; op += dsz;
        } else if (btype == VV_BLOCK_ENTROPY) {
            /* Type 3: Entropy-coded literals + stripped LZ tokens
             * First byte after comp_size is the entropy tag:
             *   VV_ENTROPY_ANS ('A') or VV_ENTROPY_HUFFMAN ('H') */
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
                err = decode_block_ans(bdata, bdata_len, op, dsz, &actual, off_bytes, dst);
            } else if (tag == VV_ENTROPY_ANS4) {
                err = decode_block_ans4(bdata, bdata_len, op, dsz, &actual, off_bytes, dst);
            } else if (tag == VV_ENTROPY_CTX) {
                err = decode_block_ctx(bdata, bdata_len, op, dsz, &actual, off_bytes, dst);
            } else if (tag == VV_ENTROPY_SEQ) {
                /* Sequence coding: ANS on literals + ML + OF */
                err = vva_decode_sequences(bdata, bdata_len, op, dsz, &actual, dst);
                if (err != VV_OK) err = VV_ERR_CORRUPT;
            } else if (tag == VV_ENTROPY_HUFFMAN) {
                err = decode_block_huffman(bdata, bdata_len, op, dsz, &actual, off_bytes, dst);
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
        uint64_t computed = vv_xxh64(dst, (size_t)(op - dst), 0);
        if (computed != ff.checksum) return VV_ERR_CORRUPT;
    }

    return (int64_t)(op - dst);
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
