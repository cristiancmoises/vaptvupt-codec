/* VaptVupt amalgamation — single-file build for Zupt */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * VaptVupt — Cross-platform portability macros
 *
 * Provides unified abstractions for compiler intrinsics used throughout
 * the codebase. Supports GCC, Clang, MSVC, and Intel compilers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef VV_PLATFORM_H
#define VV_PLATFORM_H

#include <string.h>
#include <stdint.h>

/* ─── Branch prediction hints ─── */
#if defined(__GNUC__) || defined(__clang__)
  #define VV_LIKELY(x)    __builtin_expect(!!(x), 1)
  #define VV_UNLIKELY(x)  __builtin_expect(!!(x), 0)
#else
  #define VV_LIKELY(x)    (x)
  #define VV_UNLIKELY(x)  (x)
#endif

/* ─── Prefetch hint ─── */
#if defined(__GNUC__) || defined(__clang__)
  #define VV_PREFETCH(p)       __builtin_prefetch((p), 0, 1)
  #define VV_PREFETCH_RW(p)    __builtin_prefetch((p), 1, 1)
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  #include <emmintrin.h>
  #define VV_PREFETCH(p)       _mm_prefetch((const char*)(p), _MM_HINT_T1)
  #define VV_PREFETCH_RW(p)    _mm_prefetch((const char*)(p), _MM_HINT_T1)
#else
  #define VV_PREFETCH(p)       ((void)0)
  #define VV_PREFETCH_RW(p)    ((void)0)
#endif

/* ─── Always-inline / never-inline ─── */
#if defined(__GNUC__) || defined(__clang__)
  #define VV_ALWAYS_INLINE static inline __attribute__((always_inline))
  #define VV_NOINLINE      __attribute__((noinline))
#elif defined(_MSC_VER)
  #define VV_ALWAYS_INLINE static __forceinline
  #define VV_NOINLINE      __declspec(noinline)
#else
  #define VV_ALWAYS_INLINE static inline
  #define VV_NOINLINE
#endif

/* ─── Unused parameter suppression ─── */
#if defined(__GNUC__) || defined(__clang__)
  #define VV_UNUSED __attribute__((unused))
#else
  #define VV_UNUSED
#endif

/* ─── Portable unaligned load/store via memcpy (compiler optimizes to single instr) ─── */
static inline uint16_t vv_load16(const void *p) {
    uint16_t v; memcpy(&v, p, 2); return v;
}
static inline uint32_t vv_load32(const void *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline uint64_t vv_load64(const void *p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline void vv_store16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void vv_store32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void vv_store64(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* ─── Count trailing zeros (for hash/match optimization) ─── */
#if defined(__GNUC__) || defined(__clang__)
  static inline int vv_ctz32(uint32_t x) { return __builtin_ctz(x); }
  static inline int vv_ctz64(uint64_t x) { return __builtin_ctzll(x); }
#elif defined(_MSC_VER)
  #include <intrin.h>
  static inline int vv_ctz32(uint32_t x) {
      unsigned long idx; _BitScanForward(&idx, x); return (int)idx;
  }
  static inline int vv_ctz64(uint64_t x) {
  #if defined(_M_X64) || defined(_M_ARM64)
      unsigned long idx; _BitScanForward64(&idx, x); return (int)idx;
  #else
      uint32_t lo = (uint32_t)x;
      if (lo) return vv_ctz32(lo);
      return 32 + vv_ctz32((uint32_t)(x >> 32));
  #endif
  }
#else
  static inline int vv_ctz32(uint32_t x) {
      int n = 0; while (!(x & 1)) { x >>= 1; n++; } return n;
  }
  static inline int vv_ctz64(uint64_t x) {
      int n = 0; while (!(x & 1)) { x >>= 1; n++; } return n;
  }
#endif

/* ─── SIMD capability detection macros ─── */
#if defined(__AVX2__)
  #define VV_HAS_AVX2 1
#else
  #define VV_HAS_AVX2 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #define VV_HAS_SSE2 1
#else
  #define VV_HAS_SSE2 0
#endif

#if defined(__aarch64__) && defined(__ARM_NEON)
  #define VV_HAS_NEON 1
#else
  #define VV_HAS_NEON 0
#endif

#endif /* VV_PLATFORM_H */
/*
 * VaptVupt Codec — Next-generation lossless compression
 * Public API and data structures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Cristian.
 * Zero dependencies. Pure C11.
 */
#ifndef VAPTVUPT_H
#define VAPTVUPT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * VERSION & CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */

#define VV_VERSION_MAJOR  0
#define VV_VERSION_MINOR  1
#define VV_VERSION_PATCH  0
#define VV_VERSION_STRING "0.1.0"

#define VV_MAGIC          0x56560100u  /* "VV\x01\x00" */
#define VV_MAX_BLOCK_SIZE (1u << 20)   /* 1 MB per block */
#define VV_MIN_MATCH      4
#define VV_MAX_MATCH      65535
#define VV_MAX_LIT_RUN    65535
#define VV_MAX_OFFSET     (1u << 24)   /* 16 MB default window */

/* ═══════════════════════════════════════════════════════════════
 * ERROR CODES
 * ═══════════════════════════════════════════════════════════════ */

typedef enum {
    VV_OK            =  0,
    VV_ERR_IO        = -1,
    VV_ERR_CORRUPT   = -2,
    VV_ERR_NOMEM     = -3,
    VV_ERR_OVERFLOW  = -4,
    VV_ERR_BAD_MAGIC = -5,
    VV_ERR_PARAM     = -6,
} vv_error_t;

/* ═══════════════════════════════════════════════════════════════
 * COMPRESSION MODES
 * ═══════════════════════════════════════════════════════════════ */

typedef enum {
    VV_MODE_ULTRA_FAST = 0,  /* Speed priority: greedy parse, no entropy */
    VV_MODE_BALANCED   = 1,  /* Default: lazy parse + Huffman */
    VV_MODE_EXTREME    = 2,  /* Ratio priority: optimal parse + Huffman */
} vv_mode_t;

/* ═══════════════════════════════════════════════════════════════
 * BLOCK TYPES (2-bit field in block header)
 * ═══════════════════════════════════════════════════════════════ */

typedef enum {
    VV_BLOCK_RAW        = 0,  /* Uncompressed (stored) */
    VV_BLOCK_COMPRESSED = 1,  /* LZ + raw literals */
    VV_BLOCK_RLE        = 2,  /* Run-length (single byte) */
    VV_BLOCK_ENTROPY    = 3,  /* LZ + entropy-coded literals (ANS or Huffman) */
} vv_block_type_t;

/* Entropy sub-type tags (first byte of entropy section in type-3 blocks) */
#define VV_ENTROPY_HUFFMAN  0x48  /* 'H' — Huffman (v0.3-v0.4) */
#define VV_ENTROPY_ANS      0x41  /* 'A' — tANS single-stream (v0.5) */
#define VV_ENTROPY_ANS4     0x49  /* 'I' — tANS 4-way interleaved (v0.6+) */
#define VV_ENTROPY_CTX      0x43  /* 'C' — tANS order-1 context model (v0.7+) */
#define VV_ENTROPY_SEQ      0x53  /* 'S' — sequence coding: ANS on lits+ml+of (v0.8+) */
#define VV_ENTROPY_SEQ_V2   0x54  /* 'T' — same as 'S' but with min_match=3
                                   * for binary-data compression parity with
                                   * gzip-9. Shifts ml_base[] down by 1 across
                                   * all 36 codes; every other field unchanged.
                                   * Added in v2.33.0 (decode); encoder in a
                                   * future release. */

/* Block header accessors (2-bit type, 1-bit last, 21-bit size) */
static inline vv_block_type_t vv_bh_type(uint32_t h)  { return (vv_block_type_t)(h & 3); }
static inline int      vv_bh_last(uint32_t h)  { return (h >> 2) & 1; }
static inline uint32_t vv_bh_size(uint32_t h)  { return (h >> 3) & 0x1FFFFF; }
static inline uint32_t vv_bh_pack(vv_block_type_t t, int last, uint32_t sz) {
    return (uint32_t)t | ((uint32_t)last << 2) | (sz << 3);
}

/* ═══════════════════════════════════════════════════════════════
 * TOKEN TYPES (in the sequence stream)
 *
 * Each token is: [type:2][litlen:6] [optional litlen ext]
 *                [literal bytes]
 *                [matchlen ext] [offset bytes]
 *
 * The decoder reads a compact token byte, copies literals,
 * then copies a match. This is LZ4-like for speed.
 * ═══════════════════════════════════════════════════════════════ */

/* Token byte layout:
 *   Bits 7-4: literal_length (0-14, 15=extended)
 *   Bits 3-0: match_length - VV_MIN_MATCH (0-14, 15=extended)
 *
 * Followed by:
 *   [extended literal length varint, if litlen==15]
 *   [literal bytes]
 *   [offset: 2 bytes LE (or 3 bytes if high bit set)]
 *   [extended match length varint, if matchlen==15]
 */

/* ═══════════════════════════════════════════════════════════════
 * ON-DISK STRUCTURES
 * ═══════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)

/* Frame header: 16 bytes */
typedef struct {
    uint32_t magic;           /* VV_MAGIC */
    uint8_t  version;         /* Format version (1) */
    uint8_t  flags;           /* bit0: has_checksum, bit1: has_dict */
    uint8_t  mode_hint;       /* Compression mode used (informational) */
    uint8_t  window_log;      /* Window size = 1 << window_log */
    uint64_t content_size;    /* Uncompressed size (0 = unknown) */
} vv_frame_header_t;

/* Block header: 4 bytes */
typedef struct {
    /* Bits 0-1:   block_type (vv_block_type_t) */
    /* Bit  2:     last_block flag */
    /* Bits 3-23:  decompressed_size (max 1 MB) */
    /* Bits 24-31: reserved */
    uint32_t packed;
} vv_block_header_t;

/* Frame footer: 12 bytes */
typedef struct {
    uint64_t checksum;        /* XXH64 of decompressed content */
    uint32_t footer_magic;    /* 0x56564E44 = "VVND" */
} vv_frame_footer_t;

#pragma pack(pop)

/* Block header accessors defined above with block type enum */

/* ═══════════════════════════════════════════════════════════════
 * MATCHER STATE
 * ═══════════════════════════════════════════════════════════════ */

#define VV_HC_BITS    18
#define VV_HC_SIZE    (1u << VV_HC_BITS)

typedef struct {
    int32_t  table[VV_HC_SIZE];   /* Hash → most recent position */
    int32_t *chain;               /* Chain array (window_size entries) */
    uint32_t window_size;
    uint32_t chain_depth;         /* Max chain traversal (level-dependent) */
} vv_matcher_t;

/* ═══════════════════════════════════════════════════════════════
 * HUFFMAN TABLES (entropy coding)
 *
 * 256-symbol alphabet. Max code length 12 bits.
 * Decode table: 4096 entries × 2 bytes = 8 KB (fits in L1).
 * ═══════════════════════════════════════════════════════════════ */

#define VV_HUF_MAX_BITS   12
#define VV_HUF_TABLE_SIZE (1 << VV_HUF_MAX_BITS)

typedef struct {
    uint8_t  lengths[256];            /* Code lengths per symbol */
    uint16_t codes[256];              /* Canonical codes (for encoding) */
    /* Decode table: entry = (symbol << 8) | num_bits */
    uint16_t decode[VV_HUF_TABLE_SIZE];
} vv_huffman_t;

/* ═══════════════════════════════════════════════════════════════
 * ENCODER/DECODER OPTIONS
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    vv_mode_t mode;
    uint8_t   window_log;    /* 0 = auto (20 for balanced, 24 for extreme) */
    int       checksum;      /* 1 = compute XXH64 */
    int       verbose;
    int       format_v2;     /* 1 = produce 'T' tag blocks (min_match=3) for
                              *     better real-binary ratio. Requires decoder
                              *     v2.33.0+. Default 0 for back-compat. */
} vv_options_t;

static inline void vv_default_options(vv_options_t *o) {
    o->mode = VV_MODE_BALANCED;
    o->window_log = 0;
    o->checksum = 1;
    o->verbose = 0;
    o->format_v2 = 0;
}

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API — ONE-SHOT
 * ═══════════════════════════════════════════════════════════════ */

/* Compress src[0..src_len-1] into dst[0..dst_cap-1].
 * Returns compressed size, or negative error code. */
int64_t vv_compress(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap,
                    const vv_options_t *opts);

/* Decompress src[0..src_len-1] into dst[0..dst_cap-1].
 * Returns decompressed size, or negative error code. */
int64_t vv_decompress(const uint8_t *src, size_t src_len,
                      uint8_t *dst, size_t dst_cap);

/* Flags for vv_decompress_flags (bitmask) */
#define VV_DECOMPRESS_DEFAULT          0x0
#define VV_DECOMPRESS_SKIP_CHECKSUM    0x1   /* Skip XXH64 footer verification.
                                              *
                                              * Use when the caller has its own
                                              * integrity protection (e.g. AES-GCM
                                              * wrapping the compressed data, as in
                                              * Zupt backups). On RAW/random-data
                                              * inputs where XXH64 dominates decode
                                              * time, this flag delivers a ~2× speedup.
                                              *
                                              * SAFETY: only set when another layer
                                              * already detects tampering/corruption.
                                              * Without any integrity check, silent
                                              * data corruption can go undetected. */

/* Decompress with flags. Returns decompressed size, or negative error code.
 * Equivalent to vv_decompress() when flags == VV_DECOMPRESS_DEFAULT. */
int64_t vv_decompress_flags(const uint8_t *src, size_t src_len,
                            uint8_t *dst, size_t dst_cap,
                            uint32_t flags);

/* Compute upper bound on compressed size for src_len input bytes. */
size_t vv_compress_bound(size_t src_len);

/* ═══════════════════════════════════════════════════════════════
 * MULTI-THREADED COMPRESSION
 *
 * Compresses large inputs in parallel by splitting into independent
 * frames (each a valid .vv frame on its own — concatenated output
 * is a valid .vv file that vv_decompress handles natively as a
 * multi-frame stream).
 *
 * Requires the library to be built with VV_ENABLE_THREADS (and
 * linked with -lpthread on POSIX). If threads are not available,
 * the function falls back to sequential single-threaded encoding,
 * producing bit-identical output to vv_compress.
 *
 * Tradeoff: multi-frame output is ~0.5-2% larger than a single
 * vv_compress frame because cross-frame match history is lost. Use
 * for inputs ≥ 4 MB where parallel speedup outweighs the ratio cost.
 * ═══════════════════════════════════════════════════════════════ */

/* Compress src in parallel using up to nthreads worker threads.
 * If nthreads is 0, uses the number of online CPUs (or 1 if that
 * cannot be determined). If the library was built without threading,
 * this acts exactly like vv_compress (nthreads is ignored).
 *
 * chunk_size controls the frame split size — must be ≥ 1 MB for
 * reasonable compression ratio. If 0, defaults to 4 MB.
 *
 * Returns compressed size on success, negative error code on failure. */
int64_t vv_compress_mt(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap,
                       const vv_options_t *opts,
                       unsigned int nthreads,
                       size_t chunk_size);

/* Frame info extracted from the first 16 bytes of a compressed stream.
 * Populated by vv_get_frame_info(). */
typedef struct {
    uint8_t  version;         /* Format version */
    uint8_t  has_checksum;    /* Non-zero if frame has XXH64 footer */
    uint8_t  mode_hint;       /* Compression mode used (informational) */
    uint8_t  window_log;      /* Window size = 1 << window_log */
    uint64_t content_size;    /* Uncompressed size if known (0 = unknown) */
} vv_frame_info_t;

/* Parse the first 16 bytes of a compressed stream to extract frame
 * metadata. Requires src_len >= 16. Useful for pre-allocating the
 * output buffer when content_size is known (e.g., streams produced
 * by the one-shot vv_compress API always carry content_size).
 *
 * Returns VV_OK on success, negative error code on bad magic /
 * unsupported version / too-short input. */
int vv_get_frame_info(const uint8_t *src, size_t src_len,
                      vv_frame_info_t *info);

/* ═══════════════════════════════════════════════════════════════
 * STREAMING API — for large files, memory-constrained use, or
 * when the full input/output isn't known in advance.
 *
 * Compress:
 *   ctx = vv_cstream_create(&opts);
 *   for each chunk:   vv_cstream_compress_chunk(ctx, chunk, len, dst, dst_cap, &written, is_last);
 *   vv_cstream_destroy(ctx);
 *
 * Decompress:
 *   ctx = vv_dstream_create();
 *   for each incoming block: vv_dstream_decompress_chunk(ctx, src, len, dst, dst_cap, &read, &written);
 *   vv_dstream_destroy(ctx);
 *
 * Compression is block-at-a-time: caller accumulates source data in
 * chunks of up to VV_MAX_BLOCK_SIZE (1 MB). Each call to
 * vv_cstream_compress_chunk emits one compressed block (or the frame
 * header on the first call, and the frame footer on the last).
 *
 * Decompression accepts arbitrary byte chunks and emits decoded bytes
 * as blocks complete. Partial blocks are buffered internally.
 * ═══════════════════════════════════════════════════════════════ */

/* Opaque stream context types */
typedef struct vv_cstream_s vv_cstream_t;
typedef struct vv_dstream_s vv_dstream_t;

/* Create a new compression stream context.
 * Returns NULL on allocation failure.
 * If opts is NULL, uses default options (balanced mode, checksum=1).
 * The context holds the matcher state; cross-block rep-match history
 * and hash tables are preserved across chunks for optimal ratio. */
vv_cstream_t *vv_cstream_create(const vv_options_t *opts);

/* Reset a compression stream for reuse. Clears the matcher state,
 * rep-match offsets, checksum accumulator, and emission flag so the
 * context can be used to compress a new independent frame.
 * Scratch buffers are preserved — this is the fast path for
 * per-file compression (e.g., backup tools compressing many small
 * files), avoiding per-file allocation cost.
 *
 * If opts is NULL, reuses the options from the last create/reset.
 * If opts is non-NULL, applies new options but window_log cannot
 * change (would require re-allocating matcher tables). */
int vv_cstream_reset(vv_cstream_t *ctx, const vv_options_t *opts);

/* Compress one chunk of source into dst. chunk_len must be ≤
 * VV_MAX_BLOCK_SIZE (1 MB). Set is_last=1 on the final call to emit
 * the frame footer (checksum if enabled).
 *
 * Writes at most dst_cap bytes to dst; sets *written to the actual
 * number of bytes emitted. Caller must ensure dst_cap ≥
 * vv_compress_bound(chunk_len) + 24 (frame header + footer).
 *
 * On the first call, the frame header is emitted before the first
 * block. On the last call, the frame footer (if checksum enabled) is
 * emitted after the final block.
 *
 * Returns VV_OK (0) on success, negative error code on failure. */
int vv_cstream_compress_chunk(vv_cstream_t *ctx,
                              const uint8_t *chunk, size_t chunk_len,
                              uint8_t *dst, size_t dst_cap,
                              size_t *written, int is_last);

/* Destroy a compression stream context and free all resources. */
void vv_cstream_destroy(vv_cstream_t *ctx);

/* Create a new decompression stream context.
 * Returns NULL on allocation failure. */
vv_dstream_t *vv_dstream_create(void);

/* Reset a decompression stream for reuse. Clears state so the same
 * context can decompress another independent frame. Internal buffer
 * is preserved (but emptied), avoiding per-frame allocation cost. */
int vv_dstream_reset(vv_dstream_t *ctx);

/* Decompress a chunk of input. src may contain partial or multiple
 * blocks; internal buffer holds incomplete blocks until enough input
 * is available.
 *
 * IMPORTANT API CONTRACT:
 *   - `dst` MUST be the same stable buffer base across all calls for
 *     a single frame. The decoder tracks its own output position
 *     inside `dst` and requires it not to move between calls.
 *   - `dst_cap` MUST be large enough to hold the fully-decoded
 *     content of the current frame (the decoder does not support
 *     partial-output-then-resume semantics across a block boundary).
 *   - `*written` is set to the CUMULATIVE total bytes written into
 *     `dst` so far, NOT the delta for this call. If you need the
 *     per-call delta, subtract the previous value.
 *   - `*consumed` is per-call: how many `src` bytes were processed
 *     this call.
 *
 * Writing pattern:
 *     size_t total_written = 0;
 *     while (!done) {
 *         rc = vv_dstream_decompress_chunk(ds, chunk, chunk_len,
 *                                          dst, dst_cap,       // stable
 *                                          &consumed, &written);
 *         total_written = written;   // NOT += written
 *         ...
 *     }
 *
 * Returns VV_OK (0) if more input is needed, 1 if the frame ended
 * successfully, or negative error code on failure. */
int vv_dstream_decompress_chunk(vv_dstream_t *ctx,
                                const uint8_t *src, size_t src_len,
                                uint8_t *dst, size_t dst_cap,
                                size_t *consumed, size_t *written);

/* Destroy a decompression stream context and free all resources. */
void vv_dstream_destroy(vv_dstream_t *ctx);

/* ═══════════════════════════════════════════════════════════════
 * INTERNAL HELPERS (shared across modules)
 * ═══════════════════════════════════════════════════════════════ */

/* XXH64 hash (simplified, for checksum) */
uint64_t vv_xxh64(const void *data, size_t len, uint64_t seed);

/* Streaming XXH64: init + update + finalize for when the input isn't
 * contiguous in memory. Must produce the same 64-bit hash as a
 * single-shot vv_xxh64() over the concatenated input. */
typedef struct {
    uint64_t v1, v2, v3, v4;
    uint64_t total_len;
    uint64_t seed;
    uint8_t  buf[32];
    size_t   buf_len;
} vv_xxh64_state_t;

void vv_xxh64_init(vv_xxh64_state_t *s, uint64_t seed);
void vv_xxh64_update(vv_xxh64_state_t *s, const void *data, size_t len);
uint64_t vv_xxh64_finalize(const vv_xxh64_state_t *s);

/* Hash function for matcher */
static inline uint32_t vv_hash4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - VV_HC_BITS);
}

/* Read/write little-endian helpers */
static inline uint16_t vv_read16(const uint8_t *p) {
    uint16_t v; memcpy(&v, p, 2); return v;
}
static inline uint32_t vv_read32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, 4); return v;
}
static inline void vv_write16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}
static inline void vv_write32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* ═══════════════════════════════════════════════════════════════
 * SIMD COPY HELPERS (declared here, defined in vv_simd.c)
 * ═══════════════════════════════════════════════════════════════ */

/* Copy exactly n bytes, may over-read/write by up to 32 bytes.
 * Caller must ensure sufficient slack in destination. */
void vv_copy_fast(uint8_t *dst, const uint8_t *src, size_t n);

/* Copy match with overlap handling (offset may be < copy length). */
void vv_copy_match(uint8_t *dst, uint32_t offset, size_t length);

#ifdef __cplusplus
}
#endif
#endif /* VAPTVUPT_H */
/*
 * VaptVupt — tANS Entropy Codec (v2: sparse header + 4-way interleaved)
 *
 * Standalone: define VV_ANS_STANDALONE to use without VaptVupt.
 * ZUPT-COMPAT: this header has zero VaptVupt dependencies when standalone.
 *
 * v0.6 changes:
 *   - Adaptive sparse/dense header (Item 1): 3× smaller on typical data
 *   - 4-way interleaved encode/decode (Item 2): ~2.5× faster decode
 */
#ifndef VV_ANS_H
#define VV_ANS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VVA_TABLE_LOG    12
#define VVA_TABLE_SIZE   (1 << VVA_TABLE_LOG)   /* 4096 */
#define VVA_MAX_SYMBOL   256

/* Header format discriminators */
#define VVA_HDR_SINGLE   0x01  /* Single symbol: 0-bit encoding */
#define VVA_HDR_SPARSE   0x02  /* ≤32 active symbols: (sym,freq) pairs */
#define VVA_HDR_DENSE    0x03  /* >32 active symbols: max_sym + freq array */
/* ZUPT-COMPAT: v0.5 legacy format detected by first byte being 0x00-0xFF
 * without matching any HDR_* code — fall back to old read path. */
#define VVA_HDR_LEGACY   0x00  /* v0.5 format: [max_sym] [2B×(max_sym+1)] */

#ifdef VV_ANS_STANDALONE
typedef enum {
    VVA_OK            =  0,
    VVA_ERR_IO        = -1,
    VVA_ERR_CORRUPT   = -2,
    VVA_ERR_NOMEM     = -3,
    VVA_ERR_OVERFLOW  = -4,
    VVA_ERR_PARAM     = -6,
} vva_error_t;
#else
typedef vv_error_t vva_error_t;
#define VVA_OK            VV_OK
#define VVA_ERR_CORRUPT   VV_ERR_CORRUPT
#define VVA_ERR_NOMEM     VV_ERR_NOMEM
#define VVA_ERR_OVERFLOW  VV_ERR_OVERFLOW
#define VVA_ERR_PARAM     VV_ERR_PARAM
#endif

typedef struct {
    uint8_t  symbol;
    uint8_t  nbits;
    uint16_t baseline;
} vva_dec_entry_t;

/* ═══ Public API ═══ */

/* Single-stream encode/decode (tag 'A', backward compat with v0.5) */
vva_error_t vva_encode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap, size_t *dst_len);

vva_error_t vva_decode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap,
                       size_t num_literals, size_t *src_consumed);

/* 4-way interleaved encode/decode (tag 'I', v0.6+) */
vva_error_t vva_encode4(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap, size_t *dst_len);

vva_error_t vva_decode4(const uint8_t *src, size_t src_len,
                        uint8_t *dst, size_t dst_cap,
                        size_t num_literals, size_t *src_consumed);

/* Order-1 context model encode/decode (tag 'C', v0.7+)
 * Uses 256 ANS tables — one per previous byte. Contexts with too few
 * observations inherit from the global table. 4 MB decode memory. */
vva_error_t vva_encode_ctx(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap, size_t *dst_len);

vva_error_t vva_decode_ctx(const uint8_t *src, size_t src_len,
                           uint8_t *dst, size_t dst_cap,
                           size_t num_literals, size_t *src_consumed);

/* ═══ Sequence coding (tag 'S', v0.8+) ═══
 * ZUPT-COMPAT: available when VV_ANS_STANDALONE is defined.
 *
 * Encodes an LZ token stream using 3 ANS tables: literals, match-length
 * codes (36 symbols), and offset codes (24 symbols). Replaces raw varint
 * storage of match metadata, saving 8-15% on typical data.
 *
 * Input token format (from LZ engine):
 *   [token: litlen:4|matchlen:4] [litlen_ext] [literal_bytes] [2B offset LE] [matchlen_ext]
 * Output: [3 table headers] [4B seq_count] [4B lit_count] [ANS bitstream]
 */

#define VVA_ML_CODES  36   /* Match length code count */
#define VVA_OF_CODES  27   /* Offset code count: 3 rep + 24 explicit */
#define VVA_LL_CODES  36   /* Literal-run length code count (covers 0-65536+) */

vva_error_t vva_encode_sequences(const uint8_t *tokens, size_t tok_len,
                                  uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                  int off_bytes);

/* Format-v2 variant: encodes match-length codes using ml_base_v2
 * (min_match=3). Used for 'T' tag blocks. Added v2.34.0. */
vva_error_t vva_encode_sequences_v2(const uint8_t *tokens, size_t tok_len,
                                     uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                     int off_bytes);

vva_error_t vva_decode_sequences(const uint8_t *src, size_t src_len,
                                  uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                  const uint8_t *dst_base);

/* Format-v2 variant: same wire payload as vva_decode_sequences but
 * interprets match-length codes with a table shifted down by 1
 * (min_match=3 instead of 4). Produced by tag 'T' (VV_ENTROPY_SEQ_V2)
 * blocks; closes the ~10% binary-compression gap vs gzip-9. Added
 * v2.33.0. */
vva_error_t vva_decode_sequences_v2(const uint8_t *src, size_t src_len,
                                     uint8_t *dst, size_t dst_cap, size_t *dst_len,
                                     const uint8_t *dst_base);

static inline size_t vva_bound(size_t src_len) {
    /* Context model header can be up to ~10KB, seq coding adds 3 table headers */
    return 12288 + (src_len * 15 + 7) / 8 + 16;
}

#ifdef __cplusplus
}
#endif
#endif /* VV_ANS_H */
/*
 * VaptVupt — Canonical Huffman Codec
 *
 * Standalone header: can be used independently with VV_HUFFMAN_STANDALONE.
 * Designed for embedding in Zupt or any other LZ codec.
 *
 * API:
 *   vvh_encode() — compress raw literals into Huffman bitstream
 *   vvh_decode() — decompress Huffman bitstream back to raw literals
 *
 * Format:
 *   [1B max_symbol] [packed nibble code lengths] [LSB-first bitstream]
 *
 * Performance targets:
 *   Encode: ≥ 150 MB/s   Decode: ≥ 800 MB/s   (x86-64, -O2)
 */
#ifndef VV_HUFFMAN_H
#define VV_HUFFMAN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════ */

#define VVH_SYMBOLS        256
#define VVH_MAX_CODE_LEN   15
#define VVH_DECODE_BITS    12
#define VVH_DECODE_SIZE    (1 << VVH_DECODE_BITS)  /* 4096 entries */

/* ═══════════════════════════════════════════════════════════════
 * ERROR CODES (compatible with vv_error_t when not standalone)
 * ═══════════════════════════════════════════════════════════════ */

#ifdef VV_HUFFMAN_STANDALONE
typedef enum {
    VVH_OK          =  0,
    VVH_ERR_CORRUPT = -2,
    VVH_ERR_NOMEM   = -3,
    VVH_ERR_OVERFLOW= -4,
} vvh_error_t;
#else
typedef vv_error_t vvh_error_t;
#define VVH_OK           VV_OK
#define VVH_ERR_CORRUPT  VV_ERR_CORRUPT
#define VVH_ERR_NOMEM    VV_ERR_NOMEM
#define VVH_ERR_OVERFLOW VV_ERR_OVERFLOW
#endif

/* ═══════════════════════════════════════════════════════════════
 * ENCODE TABLE (used by encoder only)
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  lengths[VVH_SYMBOLS];    /* Code length per symbol (0 = absent) */
    uint16_t codes[VVH_SYMBOLS];      /* Bit-reversed canonical codes (LSB-first) */
} vvh_enc_table_t;

/* ═══════════════════════════════════════════════════════════════
 * DECODE TABLE (used by decoder only)
 *
 * 12-bit lookup: 4096 entries × 4 bytes = 16 KB (L1-resident).
 * Entry: bits [7:0] = symbol, bits [11:8] = code length.
 * Symbols with code length > 12 use a slow path.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t table[VVH_DECODE_SIZE];  /* Fast lookup (codes ≤ 12 bits) */
    /* Slow table for codes 13-15 bits (max 256 entries) */
    uint16_t slow_code[VVH_SYMBOLS];  /* Bit-reversed code */
    uint8_t  slow_len[VVH_SYMBOLS];   /* Code length */
    uint8_t  slow_sym[VVH_SYMBOLS];   /* Symbol value */
    int      slow_count;              /* Number of slow-path symbols */
} vvh_dec_table_t;

/* ═══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Encode raw literal bytes into Huffman bitstream.
 *
 * src[0..src_len-1]  — raw literal bytes
 * dst[0..dst_cap-1]  — output buffer (header + bitstream)
 * *dst_len           — on success, set to actual compressed size
 *
 * Returns VVH_OK on success, or VVH_ERR_OVERFLOW if dst too small.
 * If compressed size >= src_len, returns VVH_ERR_OVERFLOW (incompressible).
 */
vvh_error_t vvh_encode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap, size_t *dst_len);

/*
 * Decode Huffman bitstream back to raw literal bytes.
 *
 * src[0..src_len-1]   — compressed data (header + bitstream)
 * dst[0..dst_cap-1]   — output buffer for decoded literals
 * num_literals        — expected number of decoded symbols
 * *src_consumed       — on success, bytes consumed from src
 *
 * Returns VVH_OK on success, or error code.
 */
vvh_error_t vvh_decode(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap,
                       size_t num_literals, size_t *src_consumed);

/*
 * Upper bound on compressed size for src_len literal bytes.
 */
static inline size_t vvh_bound(size_t src_len) {
    /* header (129 max) + bitstream (15 bits/symbol worst case) + slack */
    return 129 + (src_len * 15 + 7) / 8 + 8;
}

#ifdef __cplusplus
}
#endif
#endif /* VV_HUFFMAN_H */
/*
 * VaptVupt — Zupt Integration API
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Cristian.
 *
 * ZUPT-COMPAT: This is the API that Zupt calls. It wraps the internal
 * VaptVupt API with sensible defaults for backup workloads:
 *   - Checksum always enabled (data integrity is critical for backups)
 *   - Adaptive window selection (auto-detect optimal wlog per file)
 *   - Level maps to mode: 1=fast, 5=balanced, 9=extreme
 *
 * Usage:
 *   size_t bound = vvz_compress_bound(src_len);
 *   uint8_t *dst = malloc(bound);
 *   int64_t csz = vvz_compress(src, src_len, dst, bound, 5);
 *   int64_t dsz = vvz_decompress(dst, csz, out, out_cap);
 */
#ifndef VAPTVUPT_API_H
#define VAPTVUPT_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compress src into dst. Returns compressed size or negative error code.
 * level: 1 = fast (max speed), 5 = balanced (default), 9 = extreme (max ratio) */
int64_t vvz_compress(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap, int level);

/* Decompress src into dst. Returns decompressed size or negative error code. */
int64_t vvz_decompress(const uint8_t *src, size_t src_len,
                       uint8_t *dst, size_t dst_cap);

/* Upper bound on compressed size for a given input length. */
size_t vvz_compress_bound(size_t src_len);

#ifdef __cplusplus
}
#endif
#endif /* VAPTVUPT_API_H */
