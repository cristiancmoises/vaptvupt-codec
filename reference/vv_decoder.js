/*
 * VaptVupt reference decoder — JavaScript implementation.
 *
 * Pure JS, zero dependencies, runs in Node.js (v14+) and browsers
 * that support BigInt + Uint8Array. Implements decoding for:
 *   - RAW, RLE, and COMPRESSED block types (FORMAT.md §3.1-3.3)
 *   - ENTROPY tag 'S' (VV_ENTROPY_SEQ) — the tag produced by the
 *     current VaptVupt encoder — including tANS literal decoding
 *     (single-stream and 4-way interleaved), sequence decoding
 *     with rep-match offset history, and cross-block dict carry
 *   - Multi-frame streams (§6)
 *   - XXH64 footer verification (§4) via BigInt
 *
 * Since v2.29.0, the JS decoder covers 100% of output produced by
 * the current encoder. Legacy ENTROPY tags H/A/I/C (from format
 * v0.3-v0.7) throw NotImplementedError — those aren't emitted by
 * modern encoders and exist in the C decoder only for back-compat.
 *
 * Primary use case: **browser-side reading of VaptVupt archives
 * without shipping a WebAssembly build of the C codec**. The full
 * codec surface that matters for real archives is now covered.
 *
 * Usage (Node):
 *     const vv = require('./vv_decoder.js');
 *     const bytes = fs.readFileSync('file.vv');
 *     const decoded = vv.decompress(bytes);
 *
 * Usage (browser):
 *     <script src="vv_decoder.js"></script>
 *     const decoded = VaptVupt.decompress(frameBytes);
 *
 * Self-test (Node):
 *     node reference/vv_decoder.test.js
 */

(function (root, factory) {
    if (typeof module === 'object' && module.exports) {
        module.exports = factory();
    } else {
        root.VaptVupt = factory();
    }
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    // ─────────────────────────────────────────────────────────────
    // Constants from FORMAT.md
    // ─────────────────────────────────────────────────────────────

    const VV_MAGIC         = 0x56560100;
    const VV_FOOTER_MAGIC  = 0x56564E44;
    const VV_VERSION       = 1;
    const VV_MIN_MATCH     = 4;
    const VV_MAX_BLOCK_SIZE = 1 << 20;

    const BLOCK_RAW        = 0;
    const BLOCK_COMPRESSED = 1;
    const BLOCK_RLE        = 2;
    const BLOCK_ENTROPY    = 3;

    // ─────────────────────────────────────────────────────────────
    // XXH64 — seed=0 only, BigInt arithmetic.
    // Mirrors the Python reference which mirrors the C reference.
    // ─────────────────────────────────────────────────────────────

    const PRIME64_1 = 0x9E3779B185EBCA87n;
    const PRIME64_2 = 0xC2B2AE3D27D4EB4Fn;
    const PRIME64_3 = 0x165667B19E3779F9n;
    const PRIME64_4 = 0x85EBCA77C2B2AE63n;
    const PRIME64_5 = 0x27D4EB2F165667C5n;
    const MASK64 = (1n << 64n) - 1n;

    function rotl64(x, n) {
        x = x & MASK64;
        return ((x << BigInt(n)) | (x >> BigInt(64 - n))) & MASK64;
    }

    function round64(acc, lane) {
        acc = (acc + (lane * PRIME64_2)) & MASK64;
        acc = rotl64(acc, 31);
        return (acc * PRIME64_1) & MASK64;
    }

    function merge64(acc, lane) {
        const rounded = round64(0n, lane);
        acc = (acc ^ rounded) & MASK64;
        return ((acc * PRIME64_1) + PRIME64_4) & MASK64;
    }

    function readU64LE(buf, pos) {
        let v = 0n;
        for (let i = 0; i < 8; i++) {
            v |= BigInt(buf[pos + i]) << BigInt(i * 8);
        }
        return v;
    }

    function readU32LE(buf, pos) {
        return (buf[pos]
              | (buf[pos + 1] << 8)
              | (buf[pos + 2] << 16)
              | (buf[pos + 3] << 24)) >>> 0;  // unsigned
    }

    function xxh64(data) {
        const n = data.length;
        let h64;
        let i = 0;
        if (n >= 32) {
            let v1 = (PRIME64_1 + PRIME64_2) & MASK64;
            let v2 = PRIME64_2;
            let v3 = 0n;
            let v4 = (-PRIME64_1) & MASK64;
            while (i + 32 <= n) {
                v1 = round64(v1, readU64LE(data, i));
                v2 = round64(v2, readU64LE(data, i + 8));
                v3 = round64(v3, readU64LE(data, i + 16));
                v4 = round64(v4, readU64LE(data, i + 24));
                i += 32;
            }
            h64 = (rotl64(v1, 1) + rotl64(v2, 7)
                 + rotl64(v3, 12) + rotl64(v4, 18)) & MASK64;
            h64 = merge64(h64, v1);
            h64 = merge64(h64, v2);
            h64 = merge64(h64, v3);
            h64 = merge64(h64, v4);
        } else {
            h64 = PRIME64_5;
        }
        h64 = (h64 + BigInt(n)) & MASK64;

        while (i + 8 <= n) {
            const lane = readU64LE(data, i);
            h64 = (h64 ^ round64(0n, lane)) & MASK64;
            h64 = (rotl64(h64, 27) * PRIME64_1 + PRIME64_4) & MASK64;
            i += 8;
        }
        while (i + 4 <= n) {
            const lane = BigInt(readU32LE(data, i));
            h64 = (h64 ^ ((lane * PRIME64_1) & MASK64)) & MASK64;
            h64 = (rotl64(h64, 23) * PRIME64_2 + PRIME64_3) & MASK64;
            i += 4;
        }
        while (i < n) {
            const b = BigInt(data[i]);
            h64 = (h64 ^ ((b * PRIME64_5) & MASK64)) & MASK64;
            h64 = (rotl64(h64, 11) * PRIME64_1) & MASK64;
            i += 1;
        }
        h64 ^= h64 >> 33n;
        h64 = (h64 * PRIME64_2) & MASK64;
        h64 ^= h64 >> 29n;
        h64 = (h64 * PRIME64_3) & MASK64;
        h64 ^= h64 >> 32n;
        return h64 & MASK64;
    }

    // ─────────────────────────────────────────────────────────────
    // Errors
    // ─────────────────────────────────────────────────────────────

    class CorruptError extends Error {
        constructor(msg) { super(msg); this.name = 'CorruptError'; }
    }

    class NotImplementedError extends Error {
        constructor(msg) { super(msg); this.name = 'NotImplementedError'; }
    }

    // ─────────────────────────────────────────────────────────────
    // Parser primitives
    // ─────────────────────────────────────────────────────────────

    function needBytes(buf, pos, n) {
        if (pos + n > buf.length) {
            throw new CorruptError(
                `need ${n} bytes at pos ${pos}, only ${buf.length - pos} left`);
        }
    }

    function readU24LE(buf, pos) {
        return buf[pos]
             | (buf[pos + 1] << 8)
             | (buf[pos + 2] << 16);
    }

    /**
     * FORMAT.md §5.1 — lz4-style byte-sum varint.
     * Returns [value, new_pos].
     */
    function readExtLen(buf, pos) {
        let val = 0;
        while (pos < buf.length) {
            const b = buf[pos];
            pos += 1;
            val += b;
            if (b < 255) return [val, pos];
        }
        throw new CorruptError('varint extends past end of input');
    }

    // ─────────────────────────────────────────────────────────────
    // ANS primitives — mirror reference/vv_ans.py and src/vv_ans.c
    // ─────────────────────────────────────────────────────────────

    const ANS_LOG = 12;
    const ANS_L   = 1 << ANS_LOG;  // 4096
    const NSYM    = 256;

    const VVA_HDR_SINGLE = 0x01;
    const VVA_HDR_SPARSE = 0x02;
    const VVA_HDR_DENSE  = 0x03;

    /**
     * Read an ANS frequency-table header (src/vv_ans.c::read_hdr_v2).
     * Returns [normArray, newPos] where normArray is an Int32Array(256).
     */
    function readHdr(buf, pos) {
        if (pos >= buf.length) {
            throw new CorruptError('ANS header truncated');
        }
        const norm = new Int32Array(NSYM);  // all zero
        const fmt = buf[pos];

        if (fmt === VVA_HDR_SINGLE) {
            if (pos + 2 > buf.length) throw new CorruptError('HDR_SINGLE truncated');
            norm[buf[pos + 1]] = ANS_L;
            return [norm, pos + 2];
        }
        if (fmt === VVA_HDR_SPARSE) {
            if (pos + 2 > buf.length) throw new CorruptError('HDR_SPARSE truncated');
            const count = buf[pos + 1];
            const endPos = pos + 2 + 3 * count;
            if (endPos > buf.length) throw new CorruptError('HDR_SPARSE payload truncated');
            let p = pos + 2;
            for (let i = 0; i < count; i++) {
                const sym = buf[p]; p += 1;
                const freq = buf[p] | (buf[p + 1] << 8); p += 2;
                norm[sym] = freq;
            }
            return [norm, endPos];
        }
        if (fmt === VVA_HDR_DENSE) {
            if (pos + 2 > buf.length) throw new CorruptError('HDR_DENSE truncated');
            const maxSym = buf[pos + 1];
            const endPos = pos + 2 + 2 * (maxSym + 1);
            if (endPos > buf.length) throw new CorruptError('HDR_DENSE payload truncated');
            for (let i = 0; i <= maxSym; i++) {
                norm[i] = buf[pos + 2 + 2 * i] | (buf[pos + 2 + 2 * i + 1] << 8);
            }
            return [norm, endPos];
        }

        // Legacy format: first byte = max_sym, then freqs
        const maxSym = fmt;
        const endPos = pos + 1 + 2 * (maxSym + 1);
        if (endPos > buf.length) {
            throw new CorruptError('legacy ANS header truncated or unrecognized');
        }
        for (let i = 0; i <= maxSym; i++) {
            norm[i] = buf[pos + 1 + 2 * i] | (buf[pos + 1 + 2 * i + 1] << 8);
        }
        return [norm, endPos];
    }

    /**
     * Distribute symbols across the ANS state space using the
     * zstd/FSE-style fast-spread rule.
     * Returns Uint8Array(ANS_L) of spread symbols.
     */
    function spreadSymbols(norm) {
        const sp = new Uint8Array(ANS_L);
        const step = (ANS_L >>> 1) + (ANS_L >>> 3) + 3;
        let p = 0;
        for (let s = 0; s < NSYM; s++) {
            const f = norm[s];
            for (let i = 0; i < f; i++) {
                sp[p] = s;
                p = (p + step) & (ANS_L - 1);
            }
        }
        return sp;
    }

    function ilog2_(v) {
        let r = 0;
        while (v >>> 1) { v >>>= 1; r++; }
        return r;
    }

    /**
     * Build the ANS decode table.
     * Returns three parallel arrays: symbols (Uint8Array), nbits
     * (Uint8Array), baselines (Int32Array). Packed for fast lookup.
     */
    function buildDec(norm, sp) {
        const symbols  = new Uint8Array(ANS_L);
        const nbits    = new Uint8Array(ANS_L);
        const baselines = new Int32Array(ANS_L);
        const occ = new Int32Array(NSYM);
        for (let x = 0; x < ANS_L; x++) {
            const s = sp[x];
            const f = norm[s];
            const k = occ[s]++;
            symbols[x] = s;
            if (f === 0 || f === ANS_L) {
                nbits[x] = 0;
                baselines[x] = 0;
                continue;
            }
            const flg = ilog2_(f);
            const nbMax = ANS_LOG - flg;
            const lowCount = (1 << (flg + 1)) - f;
            if (k < lowCount) {
                nbits[x] = nbMax;
                baselines[x] = k << nbMax;
            } else {
                nbits[x] = nbMax - 1;
                baselines[x] = (lowCount << nbMax) + ((k - lowCount) << (nbMax - 1));
            }
        }
        return { symbols, nbits, baselines };
    }

    /**
     * LSB-first bit reader backed by a byte slice. Uses a BigInt
     * accumulator to avoid JS's 53-bit Number precision trap at
     * large bit counts. Mirror of src/vv_ans.c::ans_br_*.
     */
    class AnsBitReader {
        constructor(src) {
            this.s = src;
            this.p = 0;
            this.a = 0n;  // BigInt accumulator
            this.n = 0;   // bits currently in accumulator
        }
        fill() {
            while (this.n <= 56 && this.p < this.s.length) {
                this.a |= BigInt(this.s[this.p]) << BigInt(this.n);
                this.n += 8;
                this.p += 1;
            }
        }
        read(nb) {
            if (nb === 0) return 0;
            if (this.n < nb) this.fill();
            const mask = (1n << BigInt(nb)) - 1n;
            const v = Number(this.a & mask);
            this.a >>= BigInt(nb);
            this.n -= nb;
            return v;
        }
    }

    /**
     * Single-stream ANS decoder. Returns [Uint8Array, bytesConsumed].
     * Used when 'S' tag sub-format lit_fmt == 2.
     */
    function vvaDecode(src, numLiterals) {
        if (numLiterals === 0) return [new Uint8Array(0), 0];

        const [norm, hdr] = readHdr(src, 0);
        let np = 0;
        let single = -1;
        for (let i = 0; i < NSYM; i++) {
            if (norm[i]) { np++; single = i; }
        }
        if (np === 0) throw new CorruptError('ANS norm has no nonzero symbols');
        if (np === 1) {
            const out = new Uint8Array(numLiterals);
            out.fill(single);
            return [out, hdr];
        }

        let total = 0;
        for (let i = 0; i < NSYM; i++) total += norm[i];
        if (total !== ANS_L) {
            throw new CorruptError(`ANS norm sum ${total} != ANS_L`);
        }

        const sp = spreadSymbols(norm);
        const dec = buildDec(norm, sp);

        if (hdr + 2 > src.length) throw new CorruptError('ANS state truncated');
        let state = src[hdr] | (src[hdr + 1] << 8);
        if (state >= ANS_L) throw new CorruptError(`initial state ${state} >= ANS_L`);

        const rdr = new AnsBitReader(src.subarray(hdr + 2));
        rdr.fill();

        const out = new Uint8Array(numLiterals);
        for (let i = 0; i < numLiterals; i++) {
            if (rdr.n < ANS_LOG) rdr.fill();
            out[i] = dec.symbols[state];
            const bits = rdr.read(dec.nbits[state]);
            state = dec.baselines[state] + bits;
            if (state >= ANS_L) {
                throw new CorruptError(`state ${state} >= ANS_L (corrupt)`);
            }
        }

        let consumed = hdr + 2 + rdr.p;
        const leftover = rdr.n >>> 3;
        if (consumed >= leftover) consumed -= leftover;
        return [out, consumed];
    }

    /**
     * 4-way interleaved ANS decoder. Used for literal sub-block
     * when 'S' tag lit_fmt == 1. Mirror of vva_decode4 in C.
     */
    function vvaDecode4(src, numLiterals) {
        if (numLiterals === 0) return [new Uint8Array(0), 0];

        const [norm, hdr] = readHdr(src, 0);
        let np = 0;
        let single = -1;
        for (let i = 0; i < NSYM; i++) {
            if (norm[i]) { np++; single = i; }
        }
        if (np === 0) throw new CorruptError('ANS4 norm has no nonzero symbols');
        if (np === 1) {
            const out = new Uint8Array(numLiterals);
            out.fill(single);
            return [out, hdr];
        }

        let total = 0;
        for (let i = 0; i < NSYM; i++) total += norm[i];
        if (total !== ANS_L) {
            throw new CorruptError(`ANS4 norm sum ${total} != ANS_L`);
        }

        const sp = spreadSymbols(norm);
        const dec = buildDec(norm, sp);

        // Read 4 states (2B each) + 4 bitstream sizes (4B each)
        if (hdr + 8 + 16 > src.length) {
            throw new CorruptError('ANS4 states/sizes truncated');
        }
        let p = hdr;
        const states = new Int32Array(4);
        for (let i = 0; i < 4; i++) {
            const s = src[p] | (src[p + 1] << 8);
            if (s >= ANS_L) throw new CorruptError(`ANS4 state ${s} >= ANS_L`);
            states[i] = s;
            p += 2;
        }
        const bsizes = new Int32Array(4);
        for (let i = 0; i < 4; i++) {
            bsizes[i] = (src[p] | (src[p + 1] << 8)
                       | (src[p + 2] << 16) | (src[p + 3] << 24)) >>> 0;
            p += 4;
        }

        // 4 bit readers over contiguous slices
        const readers = [];
        for (let i = 0; i < 4; i++) {
            if (p + bsizes[i] > src.length) {
                throw new CorruptError(`ANS4 bitstream ${i} truncated`);
            }
            const r = new AnsBitReader(src.subarray(p, p + bsizes[i]));
            r.fill();
            readers.push(r);
            p += bsizes[i];
        }

        const out = new Uint8Array(numLiterals);
        const fullQuads = Math.floor(numLiterals / 4);
        for (let q = 0; q < fullQuads; q++) {
            const base = q * 4;
            for (let lane = 0; lane < 4; lane++) {
                const r = readers[lane];
                if (r.n < ANS_LOG) r.fill();
                const st = states[lane];
                out[base + lane] = dec.symbols[st];
                states[lane] = dec.baselines[st] + r.read(dec.nbits[st]);
                if (states[lane] >= ANS_L) {
                    throw new CorruptError('ANS4 state overflow');
                }
            }
        }
        // Tail
        for (let i = fullQuads * 4; i < numLiterals; i++) {
            const lane = i & 3;
            const r = readers[lane];
            if (r.n < ANS_LOG) r.fill();
            const st = states[lane];
            out[i] = dec.symbols[st];
            states[lane] = dec.baselines[st] + r.read(dec.nbits[st]);
            if (states[lane] >= ANS_L) {
                throw new CorruptError('ANS4 state overflow');
            }
        }

        return [out, p];
    }

    // ─── LL / ML / OF code tables — must match src/vv_ans.c exactly ───
    const ML_BASE = [
        4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 22, 24, 28, 32, 40, 48, 64, 96, 128, 192, 256, 384, 512, 1024, 2048,
        4096, 8192, 16384, 32768
    ];
    // Format-v2 match-length table for 'T' tag (VV_ENTROPY_SEQ_V2).
    // Every entry shifted down by 1 so code 0 represents length 3
    // (instead of 4 in v1). Extra-bits table is unchanged — step
    // sizes are preserved, only the baseline shifts. Added v2.33.0.
    const ML_BASE_V2 = ML_BASE.map(b => b - 1);
    const ML_EXTRA = [
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6, 7, 7, 9, 10, 11,
        12, 13, 14, 15
    ];
    const OF_EXTRA = [
        0, 0, 0,  // rep codes: 0 extra bits
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23
    ];
    const LL_BASE = [
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
        16, 18, 20, 24, 28, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384,
        32768, 49152, 57344, 61440
    ];
    const LL_EXTRA = [
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        1, 1, 2, 2, 2, 4, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        14, 13, 12, 12
    ];

    function mlDecode(code, extra) { return ML_BASE[code] + extra; }
    function llDecode(code, extra) { return LL_BASE[code] + extra; }
    function ofDecode(code, extra) {
        if (code < 3) return 0;  // rep-match; caller resolves
        return (1 << (code - 3)) + extra;
    }

    function buildDecFromNorm(norm) {
        let np = 0;
        for (let i = 0; i < NSYM; i++) if (norm[i] > 0) np++;
        if (np === 0) throw new CorruptError('ANS norm has no nonzero symbols');
        let total = 0;
        for (let i = 0; i < NSYM; i++) total += norm[i];
        if (total !== ANS_L) {
            throw new CorruptError(`ANS norm sum ${total} != ANS_L`);
        }
        const sp = spreadSymbols(norm);
        return buildDec(norm, sp);
    }

    // ─────────────────────────────────────────────────────────────────
    // Single-stream Huffman decoder (`lit_fmt = 3`).
    //
    // Sprint 117 (v2.47.9): ported from `src/vv_huffman.c` to close
    // the JS half of `AUDIT.md` Section 8 item 6. Wire format:
    //
    //   [1B max_sym]
    //   [⌈(max_sym + 2) / 2⌉ bytes of nibble-packed code lengths]
    //   [bitstream of canonical Huffman codes, LSB-first]
    //
    // Codes are canonical (sorted by length then symbol value) but
    // stored bit-reversed for LSB-first extraction.
    //
    // Constants must mirror `include/vv_huffman.h`:
    //   VVH_SYMBOLS      = 256
    //   VVH_MAX_CODE_LEN = 15
    //
    // The C reference uses a 4096-entry fast-path lookup table; this
    // JS reference uses a linear scan (O(n) per symbol). Slow but
    // trivially correct — production decode is the C reference.
    // ─────────────────────────────────────────────────────────────────

    const VVH_SYMBOLS = 256;
    const VVH_MAX_CODE_LEN = 15;

    function huffmanReadHeader(src) {
        if (src.length < 1) throw new CorruptError('Huffman header: empty buffer');
        const maxSym = src[0];
        const hdrSize = 1 + ((maxSym + 2) >>> 1);
        if (hdrSize > src.length) {
            throw new CorruptError(
                `Huffman header: max_sym=${maxSym} requires ${hdrSize} ` +
                `header bytes, only ${src.length} available`);
        }
        const lengths = new Uint8Array(VVH_SYMBOLS);
        for (let i = 0; i <= maxSym; i += 2) {
            const packed = src[1 + (i >>> 1)];
            lengths[i] = packed >>> 4;
            if (i + 1 <= maxSym) lengths[i + 1] = packed & 0x0F;
        }
        return [lengths, hdrSize];
    }

    function huffmanReverseBits(value, n) {
        let result = 0;
        for (let i = 0; i < n; i++) {
            result = (result << 1) | (value & 1);
            value >>>= 1;
        }
        return result >>> 0;
    }

    function huffmanAssignCanonical(lengths) {
        // Count symbols at each length
        const blCount = new Uint32Array(VVH_MAX_CODE_LEN + 1);
        for (let i = 0; i < VVH_SYMBOLS; i++) {
            const ln = lengths[i];
            if (ln > 0 && ln <= VVH_MAX_CODE_LEN) blCount[ln]++;
        }
        // First code at each length (MSB-first canonical)
        const nextCode = new Uint32Array(VVH_MAX_CODE_LEN + 1);
        let code = 0;
        for (let bits = 1; bits <= VVH_MAX_CODE_LEN; bits++) {
            code = (code + blCount[bits - 1]) << 1;
            nextCode[bits] = code >>> 0;
        }
        // Assign codes in symbol order
        const codes = new Uint32Array(VVH_SYMBOLS);
        for (let i = 0; i < VVH_SYMBOLS; i++) {
            const ln = lengths[i];
            if (ln > 0) {
                codes[i] = nextCode[ln];
                nextCode[ln] = (nextCode[ln] + 1) >>> 0;
            }
        }
        return codes;
    }

    function huffmanBuildEntries(lengths) {
        const canonical = huffmanAssignCanonical(lengths);
        const entries = [];
        for (let sym = 0; sym < VVH_SYMBOLS; sym++) {
            const ln = lengths[sym];
            if (ln === 0) continue;
            if (ln > VVH_MAX_CODE_LEN) {
                throw new CorruptError(
                    `Huffman code length ${ln} exceeds max ${VVH_MAX_CODE_LEN}`);
            }
            const rev = huffmanReverseBits(canonical[sym], ln);
            entries.push({ rev: rev, len: ln, sym: sym });
        }
        if (entries.length === 0) {
            throw new CorruptError('Huffman: no symbols with nonzero length');
        }
        // Sort shortest codes first so linear scan finds them quickly.
        entries.sort((a, b) => a.len - b.len || a.rev - b.rev);
        return entries;
    }

    /**
     * LSB-first bit reader for Huffman bitstream. Independent of the
     * MSB-first `AnsBitReader`. Mirrors `br_t` in `src/vv_huffman.c`.
     */
    class HuffmanBitReader {
        constructor(src) {
            this.s = src;
            this.p = 0;
            this.a = 0n;  // BigInt accumulator (matches C 64-bit u64)
            this.n = 0;   // bits in accumulator
        }
        refill() {
            while (this.n <= 56 && this.p < this.s.length) {
                this.a |= BigInt(this.s[this.p]) << BigInt(this.n);
                this.n += 8;
                this.p += 1;
            }
        }
        peek(n) {
            if (n === 0) return 0;
            const mask = (1n << BigInt(n)) - 1n;
            return Number(this.a & mask);
        }
        consume(n) {
            this.a >>= BigInt(n);
            this.n -= n;
        }
    }

    /**
     * Decode `numLiterals` Huffman-coded bytes from the input.
     * Returns [Uint8Array, srcConsumed]. Mirrors `vvh_decode` in
     * `src/vv_huffman.c`.
     */
    function vvhDecode(src, numLiterals) {
        if (numLiterals === 0) return [new Uint8Array(0), 0];

        const [lengths, hdrSize] = huffmanReadHeader(src);

        let hasSym = false;
        for (let i = 0; i < VVH_SYMBOLS; i++) {
            if (lengths[i] > 0) { hasSym = true; break; }
        }
        if (!hasSym) throw new CorruptError('Huffman: empty code-length table');

        const entries = huffmanBuildEntries(lengths);

        const reader = new HuffmanBitReader(src.subarray(hdrSize));
        reader.refill();

        const out = new Uint8Array(numLiterals);
        for (let i = 0; i < numLiterals; i++) {
            if (reader.n < VVH_MAX_CODE_LEN) reader.refill();

            let matched = false;
            for (const e of entries) {
                if (reader.n < e.len) {
                    if (reader.p >= reader.s.length) {
                        throw new CorruptError(
                            `Huffman: bitstream exhausted at literal ` +
                            `${i}/${numLiterals}`);
                    }
                    reader.refill();
                }
                const mask = (1 << e.len) - 1;
                if ((reader.peek(e.len) & mask) === e.rev) {
                    reader.consume(e.len);
                    out[i] = e.sym;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                throw new CorruptError(
                    `Huffman: no canonical code matched at literal ` +
                    `${i}/${numLiterals}`);
            }
        }

        // src_consumed: hdr + bytes the reader has fetched, minus
        // any over-read bytes still buffered in the accumulator.
        let srcConsumed = hdrSize + reader.p;
        const over = reader.n >>> 3;
        if (srcConsumed >= over) srcConsumed -= over;
        return [out, srcConsumed];
    }

    // Decode one canonical-Huffman symbol from a reader using a shared
    // entries table (shared by the 4-stream decoder).
    function huffmanDecodeOne(reader, entries, ctx) {
        if (reader.n < VVH_MAX_CODE_LEN) reader.refill();
        for (const e of entries) {
            if (reader.n < e.len) {
                if (reader.p >= reader.s.length) {
                    throw new CorruptError(`Huffman4: bitstream exhausted (${ctx})`);
                }
                reader.refill();
            }
            const mask = (1 << e.len) - 1;
            if ((reader.peek(e.len) & mask) === e.rev) {
                reader.consume(e.len);
                return e.sym;
            }
        }
        throw new CorruptError(`Huffman4: no canonical code matched (${ctx})`);
    }

    /**
     * Decode `numLiterals` bytes from a 4-stream interleaved Huffman
     * block (lit_fmt = 4, FORMAT.md §3.4.1). Mirrors `vvh_decode4` in
     * `src/vv_huffman.c` and `reference/vv_huffman.py::vvh_decode4`.
     * Returns [Uint8Array, srcConsumed].
     */
    function vvhDecode4(src, numLiterals) {
        if (numLiterals === 0) return [new Uint8Array(0), 0];

        const [lengths, hdrSize] = huffmanReadHeader(src);
        let hasSym = false;
        for (let i = 0; i < VVH_SYMBOLS; i++) {
            if (lengths[i] > 0) { hasSym = true; break; }
        }
        if (!hasSym) throw new CorruptError('Huffman4: empty code-length table');

        if (hdrSize + 9 > src.length) {
            throw new CorruptError('Huffman4: stream-size header truncated');
        }
        const sh = hdrSize;
        const s1 = (src[sh] | (src[sh + 1] << 8) | (src[sh + 2] << 16)) >>> 0;
        const s2 = (src[sh + 3] | (src[sh + 4] << 8) | (src[sh + 5] << 16)) >>> 0;
        const s3 = (src[sh + 6] | (src[sh + 7] << 8) | (src[sh + 8] << 16)) >>> 0;

        const streamsOff = hdrSize + 9;
        const streamsTotal = src.length - streamsOff;
        if (s1 > streamsTotal || s2 > streamsTotal - s1 ||
            s3 > streamsTotal - s1 - s2) {
            throw new CorruptError('Huffman4: stream sizes exceed payload');
        }
        const s0 = streamsTotal - s1 - s2 - s3;
        if (numLiterals >= 4 && (s0 === 0 || s1 === 0 || s2 === 0 || s3 === 0)) {
            throw new CorruptError('Huffman4: zero-length stream with >=4 literals');
        }

        const entries = huffmanBuildEntries(lengths);

        const b = streamsOff;
        const readers = [
            new HuffmanBitReader(src.subarray(b, b + s0)),
            new HuffmanBitReader(src.subarray(b + s0, b + s0 + s1)),
            new HuffmanBitReader(src.subarray(b + s0 + s1, b + s0 + s1 + s2)),
            new HuffmanBitReader(src.subarray(b + s0 + s1 + s2, b + s0 + s1 + s2 + s3)),
        ];
        for (const r of readers) r.refill();

        const out = new Uint8Array(numLiterals);
        const q = Math.floor(numLiterals / 4);
        let idx = 0;
        for (let i = 0; i < q; i++) {
            for (let lane = 0; lane < 4; lane++) {
                out[idx + lane] = huffmanDecodeOne(readers[lane], entries, `lane${lane}`);
            }
            idx += 4;
        }
        const tail = numLiterals - q * 4;
        for (let lane = 0; lane < tail; lane++) {
            out[idx] = huffmanDecodeOne(readers[lane], entries, `tail${lane}`);
            idx += 1;
        }

        return [out, streamsOff + s0 + s1 + s2 + s3];
    }

    /**
     * Decode a full 'S' tag (VV_ENTROPY_SEQ) block payload.
     *
     * src:   Uint8Array starting at the byte AFTER the tag
     *        (the bdata slice — starts with [4B total_lits] [1B lit_fmt]
     *        [4B lit_enc_len] ...).
     * out:   the growing frame output array (Array<number>), with
     *        `out.length` used as the current global output cursor
     *        for match references. Cross-block dict carry supported.
     *
     * Appends the decoded bytes to `out`. Returns number of bytes
     * appended (equivalent to dsz from the block header).
     *
     * mlBaseTab: optional — ML_BASE (default, 'S' tag min_match=4)
     *            or ML_BASE_V2 ('T' tag min_match=3). Added v2.35.0
     *            for JS-side 'T' tag support; v1 callers omit it.
     */
    function vvaDecodeSequences(src, out, mlBaseTab) {
        const mlTab = mlBaseTab || ML_BASE;
        let p = 0;
        const end = src.length;

        if (p + 9 > end) throw new CorruptError("'S' lit section header truncated");
        const totalLits = (src[p] | (src[p + 1] << 8)
                         | (src[p + 2] << 16) | (src[p + 3] << 24)) >>> 0;
        p += 4;
        const litFmt = src[p]; p += 1;
        const litEncLen = (src[p] | (src[p + 1] << 8)
                         | (src[p + 2] << 16) | (src[p + 3] << 24)) >>> 0;
        p += 4;
        if (p + litEncLen > end) throw new CorruptError("'S' lit payload truncated");

        // Decode literals based on lit_fmt
        let litBuf;
        if (totalLits === 0 || litEncLen === 0) {
            litBuf = new Uint8Array(0);
        } else if (litFmt === 0) {
            if (litEncLen < totalLits) {
                throw new CorruptError("'S' raw lit: enc_len < total_lits");
            }
            litBuf = src.subarray(p, p + totalLits);
        } else if (litFmt === 1) {
            const [buf] = vvaDecode4(src.subarray(p, p + litEncLen), totalLits);
            litBuf = buf;
        } else if (litFmt === 2) {
            const [buf] = vvaDecode(src.subarray(p, p + litEncLen), totalLits);
            litBuf = buf;
        } else if (litFmt === 3) {
            // HUFFMAN (added v2.46.0). Single-stream Huffman literal coding.
            // Sprint 117 (v2.47.9): ported from src/vv_huffman.c.
            const [buf] = vvhDecode(src.subarray(p, p + litEncLen), totalLits);
            litBuf = buf;
        } else if (litFmt === 4) {
            // HUFFMAN4 (added v2.47.0). 4-stream interleaved Huffman.
            // Ported in v2.65.4 (Sprint 134) — the default literal
            // format for blocks with >=1024 literals, so the reference
            // decoder now covers default C encoder output.
            const [buf] = vvhDecode4(src.subarray(p, p + litEncLen), totalLits);
            litBuf = buf;
        } else {
            throw new CorruptError(`'S' unknown lit_fmt ${litFmt}`);
        }
        p += litEncLen;

        // Match count
        if (p + 4 > end) throw new CorruptError("'S' match_count truncated");
        const matchCount = (src[p] | (src[p + 1] << 8)
                          | (src[p + 2] << 16) | (src[p + 3] << 24)) >>> 0;
        p += 4;

        // Three ANS headers (ML, OF, LL)
        function readHdrBlock() {
            if (p + 2 > end) throw new CorruptError("'S' header size truncated");
            const sz = src[p] | (src[p + 1] << 8); p += 2;
            if (p + sz > end) throw new CorruptError("'S' header body truncated");
            if (sz === 0) {
                p += 0;
                return new Int32Array(NSYM);
            }
            const [norm] = readHdr(src.subarray(p, p + sz), 0);
            p += sz;
            return norm;
        }
        const normML = readHdrBlock();
        const normOF = readHdrBlock();
        const normLL = readHdrBlock();

        // Initial states
        if (p + 6 > end) throw new CorruptError("'S' initial states truncated");
        let stateML = src[p] | (src[p + 1] << 8); p += 2;
        let stateOF = src[p] | (src[p + 1] << 8); p += 2;
        let stateLL = src[p] | (src[p + 1] << 8); p += 2;

        // Sequence bitstream
        if (p + 4 > end) throw new CorruptError("'S' seq_bs_len truncated");
        const seqBsLen = (src[p] | (src[p + 1] << 8)
                        | (src[p + 2] << 16) | (src[p + 3] << 24)) >>> 0;
        p += 4;
        if (p + seqBsLen > end) throw new CorruptError("'S' seq bitstream truncated");

        const decLL = buildDecFromNorm(normLL);
        let decML = null, decOF = null;
        if (matchCount > 0) {
            decML = buildDecFromNorm(normML);
            decOF = buildDecFromNorm(normOF);
        }

        const rdr = new AnsBitReader(src.subarray(p, p + seqBsLen));
        rdr.fill();
        p += seqBsLen;

        const baseLenAtStart = out.length;
        let litPos = 0;
        let matchesDecoded = 0;
        const decRep = [0, 0, 0];

        while (litPos < totalLits || matchesDecoded < matchCount) {
            rdr.fill();
            if (stateLL >= ANS_L || stateOF >= ANS_L || stateML >= ANS_L) {
                throw new CorruptError("'S' state overflow");
            }

            const llSym = decLL.symbols[stateLL];
            const llNb  = decLL.nbits[stateLL];
            const llBase = decLL.baselines[stateLL];
            let ofSym = 0, ofNb = 0, ofBase = 0;
            let mlSym = 0, mlNb = 0, mlBase = 0;
            if (matchesDecoded < matchCount) {
                ofSym = decOF.symbols[stateOF];
                ofNb  = decOF.nbits[stateOF];
                ofBase = decOF.baselines[stateOF];
                mlSym = decML.symbols[stateML];
                mlNb  = decML.nbits[stateML];
                mlBase = decML.baselines[stateML];
            }

            // LL
            stateLL = llBase + rdr.read(llNb);
            const llExtra = rdr.read(LL_EXTRA[llSym]);
            const litlen = llDecode(llSym, llExtra);
            if (litPos + litlen > totalLits) {
                throw new CorruptError("'S' literal overflow");
            }
            for (let i = 0; i < litlen; i++) {
                out.push(litBuf[litPos + i]);
            }
            litPos += litlen;

            if (matchesDecoded >= matchCount) break;

            // OF
            rdr.fill();
            stateOF = ofBase + rdr.read(ofNb);
            let offset;
            if (ofSym < 3) {
                offset = decRep[ofSym];
            } else {
                const ofExtra = rdr.read(OF_EXTRA[ofSym]);
                offset = ofDecode(ofSym, ofExtra);
            }
            if (offset !== 0 && offset !== decRep[0]) {
                decRep[2] = decRep[1];
                decRep[1] = decRep[0];
                decRep[0] = offset;
            }

            // ML
            rdr.fill();
            stateML = mlBase + rdr.read(mlNb);
            const mlExtra = rdr.read(ML_EXTRA[mlSym]);
            const matchlen = mlTab[mlSym] + mlExtra;

            const currentTotal = out.length;
            if (offset === 0 || offset > currentTotal) {
                throw new CorruptError(`'S' invalid offset ${offset}`);
            }
            // Match copy; self-referential for offset < matchlen
            const matchSrc = currentTotal - offset;
            for (let j = 0; j < matchlen; j++) {
                out.push(out[matchSrc + j]);
            }

            matchesDecoded++;
        }

        return out.length - baseLenAtStart;
    }

    // ─────────────────────────────────────────────────────────────
    // COMPRESSED block decoder (FORMAT.md §3.2, §5)
    // ─────────────────────────────────────────────────────────────

    function decodeCompressedBlock(buf, pos, csz, dsz, offBytes, dstBaseOffset, out) {
        const end = pos + csz;
        const initialLen = out.length;

        while (out.length - initialLen < dsz) {
            if (pos >= end) {
                throw new CorruptError('token stream ended before block completion');
            }

            const token = buf[pos]; pos += 1;
            let ll = token >>> 4;
            const mc = token & 0x0F;

            if (ll === 15) {
                const r = readExtLen(buf, pos);
                ll += r[0]; pos = r[1];
            }

            if (ll > 0) {
                needBytes(buf, pos, ll);
                for (let j = 0; j < ll; j++) out.push(buf[pos + j]);
                pos += ll;
            }

            if (out.length - initialLen >= dsz) break;

            let offset;
            if (offBytes === 2) {
                needBytes(buf, pos, 2);
                offset = buf[pos] | (buf[pos + 1] << 8);
                pos += 2;
            } else {
                needBytes(buf, pos, 3);
                offset = readU24LE(buf, pos);
                pos += 3;
            }

            let mlen = mc + VV_MIN_MATCH;
            if (mc === 15) {
                const r = readExtLen(buf, pos);
                mlen += r[0]; pos = r[1];
            }

            if (offset === 0) throw new CorruptError('offset of 0 is invalid');
            const curPos = out.length;
            let matchSrcIdx = curPos - offset;
            if (matchSrcIdx < dstBaseOffset) {
                throw new CorruptError(
                    `match offset ${offset} reaches before frame start`);
            }

            // Self-overlapping copy must be byte-by-byte for offset < mlen
            if (offset >= mlen) {
                for (let j = 0; j < mlen; j++) {
                    out.push(out[matchSrcIdx + j]);
                }
            } else {
                for (let j = 0; j < mlen; j++) {
                    out.push(out[matchSrcIdx]);
                    matchSrcIdx += 1;
                }
            }
        }

        return end;
    }

    // ─────────────────────────────────────────────────────────────
    // Single-frame decoder
    // ─────────────────────────────────────────────────────────────

    function decompressFrame(buf, pos) {
        if (buf.length - pos < 16) {
            throw new CorruptError('frame header truncated (need 16 bytes)');
        }

        const magic = readU32LE(buf, pos); pos += 4;
        if (magic !== VV_MAGIC) {
            throw new CorruptError(
                `bad magic 0x${magic.toString(16).padStart(8, '0')}`);
        }

        const version = buf[pos]; pos += 1;
        if (version !== VV_VERSION) {
            throw new CorruptError(`unsupported version ${version}`);
        }

        const flags = buf[pos]; pos += 1;
        const hasChecksum = (flags & 0x01) !== 0;
        // reserved bits ignored per C reference

        pos += 1; // mode_hint (informational)
        const windowLog = buf[pos]; pos += 1;
        if (windowLog < 10 || windowLog > 24) {
            throw new CorruptError(`invalid window_log ${windowLog} (expected 10..24)`);
        }
        const offBytes = windowLog <= 16 ? 2 : 3;

        const contentSize = readU64LE(buf, pos); pos += 8;
        // contentSize is informational (matches C reference behavior)

        // Accumulate this frame's decoded bytes into `out`.
        const out = [];
        const frameStartIdx = 0;
        let last = false;
        while (!last) {
            needBytes(buf, pos, 4);
            const bh = readU32LE(buf, pos); pos += 4;
            const btype = bh & 3;
            last = ((bh >>> 2) & 1) !== 0;
            const dsz = (bh >>> 3) & 0x1FFFFF;

            if (dsz > VV_MAX_BLOCK_SIZE) {
                throw new CorruptError(
                    `block size ${dsz} exceeds max ${VV_MAX_BLOCK_SIZE}`);
            }

            if (btype === BLOCK_RAW) {
                needBytes(buf, pos, dsz);
                for (let j = 0; j < dsz; j++) out.push(buf[pos + j]);
                pos += dsz;

            } else if (btype === BLOCK_RLE) {
                needBytes(buf, pos, 1);
                const byteVal = buf[pos]; pos += 1;
                for (let j = 0; j < dsz; j++) out.push(byteVal);

            } else if (btype === BLOCK_COMPRESSED) {
                needBytes(buf, pos, 3);
                const csz = readU24LE(buf, pos); pos += 3;
                needBytes(buf, pos, csz);
                const blockEnd = pos + csz;
                const newPos = decodeCompressedBlock(
                    buf, pos, csz, dsz, offBytes, frameStartIdx, out);
                if (newPos > blockEnd) {
                    throw new CorruptError(
                        `COMPRESSED block consumed more than csz`);
                }
                pos = blockEnd;

            } else if (btype === BLOCK_ENTROPY) {
                needBytes(buf, pos, 3);
                const csz = readU24LE(buf, pos); pos += 3;
                if (csz < 1) {
                    throw new CorruptError('entropy block too small for tag');
                }
                needBytes(buf, pos, csz);
                const tag = buf[pos];
                const tagChr = (tag >= 32 && tag < 127)
                    ? String.fromCharCode(tag) : '?';

                if (tag === 0x53) {  // ENTROPY_SEQ — 'S' tag
                    // FORMAT.md §3.4: 'S' tag is self-contained. The
                    // payload after the tag byte follows the layout
                    // implemented in vvaDecodeSequences. Match offsets
                    // can reach into prior blocks in the same frame
                    // (cross-block dict carry — the `out` array holds
                    // the whole frame's decoded content so far).
                    const payload = buf.subarray(pos + 1, pos + csz);
                    vvaDecodeSequences(payload, out);
                    pos += csz;
                } else if (tag === 0x54) {  // ENTROPY_SEQ_V2 — 'T' tag
                    // Format v2: same wire payload as 'S' but decoded
                    // with ML_BASE_V2 so code 0 represents match length
                    // 3 instead of 4. Added v2.33.0 (decoder) /
                    // v2.35.0 (encoder + hash3 + JS decoder support).
                    const payload = buf.subarray(pos + 1, pos + csz);
                    vvaDecodeSequences(payload, out, ML_BASE_V2);
                    pos += csz;
                } else {
                    throw new NotImplementedError(
                        `ENTROPY block (tag '${tagChr}' = 0x${tag.toString(16)}) `
                        + `not implemented in JS reference decoder. Supported: `
                        + `'S' (VV_ENTROPY_SEQ) and 'T' (VV_ENTROPY_SEQ_V2). `
                        + `Legacy tags 'H'/'A'/'I'/'C' are decode-only in the `
                        + `C reference and aren't produced by modern encoders.`);
                }

            } else {
                throw new CorruptError(`unknown block type ${btype}`);
            }
        }

        if (hasChecksum) {
            if (buf.length - pos < 12) {
                throw new CorruptError('frame footer truncated');
            }
            const expected = readU64LE(buf, pos); pos += 8;
            const footerMagic = readU32LE(buf, pos); pos += 4;
            if (footerMagic !== VV_FOOTER_MAGIC) {
                throw new CorruptError(
                    `bad footer magic 0x${footerMagic.toString(16)}`);
            }
            const frameBytes = Uint8Array.from(out.slice(frameStartIdx));
            const actual = xxh64(frameBytes);
            if (actual !== expected) {
                throw new CorruptError(
                    `checksum mismatch: got 0x${actual.toString(16)}, `
                    + `expected 0x${expected.toString(16)}`);
            }
        }

        const decoded = Uint8Array.from(out);
        return { decoded, newPos: pos };
    }

    // ─────────────────────────────────────────────────────────────
    // Public: multi-frame decode
    // ─────────────────────────────────────────────────────────────

    function decompress(buf) {
        // Accept Buffer / Uint8Array / array-of-numbers
        if (!buf || buf.length === 0) {
            throw new CorruptError('empty input');
        }
        if (!(buf instanceof Uint8Array) && !Array.isArray(buf)
            && (typeof Buffer === 'undefined' || !(buf instanceof Buffer))) {
            throw new TypeError('expected Uint8Array or Buffer');
        }
        // Normalize to Uint8Array
        const data = buf instanceof Uint8Array ? buf : Uint8Array.from(buf);

        const chunks = [];
        let pos = 0;
        let totalLen = 0;
        while (pos < data.length) {
            const r = decompressFrame(data, pos);
            chunks.push(r.decoded);
            totalLen += r.decoded.length;
            pos = r.newPos;
        }
        // Concatenate Uint8Arrays
        const out = new Uint8Array(totalLen);
        let offset = 0;
        for (const c of chunks) {
            out.set(c, offset);
            offset += c.length;
        }
        return out;
    }

    return {
        decompress,
        xxh64,
        CorruptError,
        NotImplementedError,
        // Exposed for testing
        _constants: {
            VV_MAGIC, VV_FOOTER_MAGIC, VV_VERSION, VV_MIN_MATCH,
            BLOCK_RAW, BLOCK_COMPRESSED, BLOCK_RLE, BLOCK_ENTROPY,
        },
    };
}));
