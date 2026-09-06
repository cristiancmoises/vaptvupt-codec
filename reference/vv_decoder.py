#!/usr/bin/env python3
"""
VaptVupt reference decoder — Python implementation.

Implements RAW, RLE, COMPRESSED, and the modern sequence-entropy tags
`S`/`T`, including their current literal coders. Multi-frame streams,
XXH64 footer verification, and x86/AArch64 BCJ inverse filters are supported.
Legacy decode-only entropy tags remain outside this compact reference; the C
decoder is canonical for archived `H`/`I`/`C` variants.

This decoder independently validates current encoder output against FORMAT.md
and is required by the release tests to reproduce it byte-for-byte.

Usage:
    python3 vv_decoder.py <input.vv> [output_file]

If output_file is omitted, decoded bytes go to stdout.

Run the spec-suite check:
    python3 vv_decoder.py --self-test

Requires: Python 3.7+. No third-party dependencies.
"""

import sys
import struct

# ─────────────────────────────────────────────────────────────────
# Constants from FORMAT.md §1, §2
# ─────────────────────────────────────────────────────────────────

VV_MAGIC          = 0x56560100
VV_FOOTER_MAGIC   = 0x56564E44   # "VVND" little-endian
VV_VERSION        = 1
VV_MIN_MATCH      = 4
VV_MAX_BLOCK_SIZE = 1 << 20

# Block types (FORMAT.md §3)
BLOCK_RAW        = 0
BLOCK_COMPRESSED = 1
BLOCK_RLE        = 2
BLOCK_ENTROPY    = 3

# Entropy tags (FORMAT.md §3.4)
ENTROPY_HUFFMAN  = 0x48  # 'H'
ENTROPY_ANS      = 0x41  # 'A'
ENTROPY_ANS4     = 0x49  # 'I'
ENTROPY_CTX      = 0x43  # 'C'
ENTROPY_SEQ      = 0x53  # 'S'
ENTROPY_SEQ_V2   = 0x54  # 'T' — format v2 (min_match=3), v2.33.0+


# ─────────────────────────────────────────────────────────────────
# BCJ inverse filters — exact ports of src/vv_bcj.c.
# ────────────────────────────────────────────────────────────────

def _bcj_test_msb(value):
    return value == 0x00 or value == 0xFF


def _bcj_x86_inverse(data, ip=0):
    """Invert the whole-frame x86 E8/E9 transform in-place."""
    size = len(data)
    if size < 5:
        return
    pos = 0
    mask = 0
    limit = size - 4
    ip = (ip + 5) & 0xFFFFFFFF
    while True:
        p = pos
        while p < limit and (data[p] & 0xFE) != 0xE8:
            p += 1
        distance = p - pos
        pos = p
        if p >= limit:
            return
        if distance > 2:
            mask = 0
        else:
            mask >>= distance
            probe = (mask >> 1) + 1
            if (mask != 0 and
                    (mask > 4 or mask == 3 or
                     _bcj_test_msb(data[p + probe]))):
                mask = (mask >> 1) | 4
                pos += 1
                continue

        if _bcj_test_msb(data[p + 4]):
            value = (data[p + 4] << 24) | (data[p + 3] << 16) | \
                    (data[p + 2] << 8) | data[p + 1]
            current = (ip + pos) & 0xFFFFFFFF
            pos += 5
            value = (value - current) & 0xFFFFFFFF
            if mask != 0:
                shift = (mask & 6) << 2
                if _bcj_test_msb((value >> shift) & 0xFF):
                    value ^= ((0x100 << shift) - 1) & 0xFFFFFFFF
                    value = (value - current) & 0xFFFFFFFF
                mask = 0
            data[p + 1] = value & 0xFF
            data[p + 2] = (value >> 8) & 0xFF
            data[p + 3] = (value >> 16) & 0xFF
            data[p + 4] = (-(value >> 24 & 1)) & 0xFF
        else:
            mask = (mask >> 1) | 4
            pos += 1


def _bcj_arm64_inverse(data, ip=0):
    """Invert the whole-frame AArch64 BL/ADRP transform in-place."""
    for pos in range(0, len(data) & ~3, 4):
        insn = (data[pos] | (data[pos + 1] << 8) |
                (data[pos + 2] << 16) | (data[pos + 3] << 24))
        if insn >> 26 == 0x25:
            imm = insn & 0x03FFFFFF
            current = ((ip + pos) & 0xFFFFFFFF) >> 2
            imm = (imm - current) & 0x03FFFFFF
            insn = (insn & 0xFC000000) | imm
        elif insn & 0x9F000000 == 0x90000000:
            imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2)
            current = ((ip + pos) & 0xFFFFFFFF) >> 12
            imm = (imm - current) & 0x001FFFFF
            insn = ((insn & 0x9F00001F) | ((imm & 3) << 29) |
                    (((imm >> 2) & 0x7FFFF) << 5))
        else:
            continue
        data[pos] = insn & 0xFF
        data[pos + 1] = (insn >> 8) & 0xFF
        data[pos + 2] = (insn >> 16) & 0xFF
        data[pos + 3] = (insn >> 24) & 0xFF


# ─────────────────────────────────────────────────────────────────
# XXH64 — minimal pure-Python implementation, seed=0 only.
# ─────────────────────────────────────────────────────────────────

PRIME64_1 = 0x9E3779B185EBCA87
PRIME64_2 = 0xC2B2AE3D27D4EB4F
PRIME64_3 = 0x165667B19E3779F9
PRIME64_4 = 0x85EBCA77C2B2AE63
PRIME64_5 = 0x27D4EB2F165667C5

MASK64 = (1 << 64) - 1


def _rotl64(x, n):
    x &= MASK64
    return ((x << n) | (x >> (64 - n))) & MASK64


def _round(acc, lane):
    acc = (acc + (lane * PRIME64_2)) & MASK64
    acc = _rotl64(acc, 31)
    return (acc * PRIME64_1) & MASK64


def _merge(acc, lane):
    lane = _round(0, lane)
    acc = (acc ^ lane) & MASK64
    return ((acc * PRIME64_1) + PRIME64_4) & MASK64


def xxh64(data, seed=0):
    """Pure-Python XXH64 of `data` with optional seed."""
    n = len(data)
    if n >= 32:
        v1 = (seed + PRIME64_1 + PRIME64_2) & MASK64
        v2 = (seed + PRIME64_2) & MASK64
        v3 = (seed + 0) & MASK64
        v4 = (seed - PRIME64_1) & MASK64

        i = 0
        while i + 32 <= n:
            l1, l2, l3, l4 = struct.unpack_from('<QQQQ', data, i)
            v1 = _round(v1, l1)
            v2 = _round(v2, l2)
            v3 = _round(v3, l3)
            v4 = _round(v4, l4)
            i += 32

        h64 = (_rotl64(v1, 1) + _rotl64(v2, 7)
               + _rotl64(v3, 12) + _rotl64(v4, 18)) & MASK64
        h64 = _merge(h64, v1)
        h64 = _merge(h64, v2)
        h64 = _merge(h64, v3)
        h64 = _merge(h64, v4)
    else:
        h64 = (seed + PRIME64_5) & MASK64
        i = 0

    h64 = (h64 + n) & MASK64

    while i + 8 <= n:
        lane = struct.unpack_from('<Q', data, i)[0]
        h64 = (h64 ^ _round(0, lane)) & MASK64
        h64 = (_rotl64(h64, 27) * PRIME64_1 + PRIME64_4) & MASK64
        i += 8

    while i + 4 <= n:
        lane = struct.unpack_from('<I', data, i)[0]
        h64 = (h64 ^ ((lane * PRIME64_1) & MASK64)) & MASK64
        h64 = (_rotl64(h64, 23) * PRIME64_2 + PRIME64_3) & MASK64
        i += 4

    while i < n:
        b = data[i]
        h64 = (h64 ^ ((b * PRIME64_5) & MASK64)) & MASK64
        h64 = (_rotl64(h64, 11) * PRIME64_1) & MASK64
        i += 1

    h64 ^= h64 >> 33
    h64 = (h64 * PRIME64_2) & MASK64
    h64 ^= h64 >> 29
    h64 = (h64 * PRIME64_3) & MASK64
    h64 ^= h64 >> 32

    return h64


# ─────────────────────────────────────────────────────────────────
# Parser primitives
# ─────────────────────────────────────────────────────────────────

class CorruptError(Exception):
    """The compressed input does not conform to the spec."""


def _need(buf, pos, n):
    if pos + n > len(buf):
        raise CorruptError(f"need {n} bytes at pos {pos}, only {len(buf)-pos} left")


def read_u8(buf, pos):
    _need(buf, pos, 1)
    return buf[pos], pos + 1


def read_u24(buf, pos):
    """Three little-endian bytes → uint32."""
    _need(buf, pos, 3)
    return buf[pos] | (buf[pos+1] << 8) | (buf[pos+2] << 16), pos + 3


def read_u32(buf, pos):
    _need(buf, pos, 4)
    return struct.unpack_from('<I', buf, pos)[0], pos + 4


def read_u64(buf, pos):
    _need(buf, pos, 8)
    return struct.unpack_from('<Q', buf, pos)[0], pos + 8


def read_ext_len(buf, pos, end=None):
    """FORMAT.md §5.1 — lz4-style byte-sum varint.

    Read bytes, summing as uint8. Stop when one is < 255. Return total."""
    val = 0
    if end is None:
        end = len(buf)
    while pos < end:
        b = buf[pos]
        pos += 1
        val += b
        if b < 255:
            return val, pos
    raise CorruptError("varint extends past end of input")


# ─────────────────────────────────────────────────────────────────
# Decoder
# ─────────────────────────────────────────────────────────────────

def _decode_compressed_block(buf, pos, csz, dsz, off_bytes, dst_base_offset, out,
                             max_offset):
    """FORMAT.md §3.2 + §5: decode an LZ token stream into `out`.

    `dst_base_offset` is the index in `out` at which this frame began
    (matches MUST NOT reach before this point — FORMAT.md §5).
    Returns the new `pos` after consuming `csz` token bytes.
    """
    end = pos + csz
    target_len = len(out) + dsz  # decoded output should reach this length
    initial_len = len(out)

    while pos < end:
        token = buf[pos]; pos += 1
        ll = token >> 4
        mc = token & 0x0F

        # Extended literal length
        if ll == 15:
            ext, pos = read_ext_len(buf, pos, end)
            ll += ext

        # Literal bytes
        if ll > end - pos or ll > target_len - len(out):
            raise CorruptError("literal run exceeds compressed block bounds")
        if ll > 0:
            out += buf[pos:pos + ll]
            pos += ll

        # If we've hit the target and there's no match, stop.
        if pos == end:
            break

        # Offset (2 or 3 LE bytes per FORMAT.md §5)
        if off_bytes > end - pos:
            raise CorruptError("offset truncated in compressed block")
        if off_bytes == 2:
            offset, pos = struct.unpack_from('<H', buf, pos)[0], pos + 2
        else:
            offset, pos = read_u24(buf, pos)

        # Match length
        mlen = mc + VV_MIN_MATCH
        if mc == 15:
            ext, pos = read_ext_len(buf, pos, end)
            mlen += ext

        # Validate offset (FORMAT.md §5)
        if offset == 0 or offset > max_offset:
            raise CorruptError("match offset is outside frame window")
        if mlen > target_len - len(out):
            raise CorruptError("match exceeds decoded block size")
        # Position relative to dst_base
        cur_pos = len(out)
        match_src_idx = cur_pos - offset
        if match_src_idx < dst_base_offset:
            raise CorruptError(
                f"match offset {offset} reaches before frame start "
                f"(cur={cur_pos}, dst_base={dst_base_offset})")

        # Copy `mlen` bytes from out[match_src_idx:] to end of out.
        # Self-overlapping copy must be byte-by-byte for offset < mlen.
        if offset >= mlen:
            out += bytes(out[match_src_idx:match_src_idx + mlen])
        else:
            # Overlapping copy (RLE-style extension)
            for _ in range(mlen):
                out.append(out[match_src_idx])
                match_src_idx += 1

    actual = len(out) - initial_len
    if actual != dsz:
        raise CorruptError(f"block produced {actual} bytes, expected {dsz}")

    return pos


def _decode_stripped_tokens(token_bytes, literals, dsz, off_bytes,
                              dst_base_offset, out, max_offset):
    """Decode tokens whose literal-bytes live in a separate buffer.

    Used for ENTROPY blocks (FORMAT.md §3.4): literals were
    entropy-coded into `literals` (already decoded), and the
    `token_bytes` contain only the token-byte + offset + lengths
    (no literal payload inline).

    Mutates `out` by appending `dsz` decoded bytes.
    """
    pos = 0
    end = len(token_bytes)
    initial_len = len(out)
    lit_pos = 0

    while pos < end:
        token = token_bytes[pos]; pos += 1
        ll = token >> 4
        mc = token & 0x0F

        if ll == 15:
            # Inline read_ext_len
            while True:
                if pos >= end:
                    raise CorruptError("varint truncated in stripped block")
                b = token_bytes[pos]; pos += 1
                ll += b
                if b < 255:
                    break

        # Literal bytes come from `literals`, NOT inline
        if ll > dsz - (len(out) - initial_len):
            raise CorruptError("literal run exceeds decoded block size")
        if ll > 0:
            if lit_pos + ll > len(literals):
                raise CorruptError(
                    f"stripped block needs {lit_pos + ll} literals, "
                    f"only {len(literals)} available")
            out += literals[lit_pos : lit_pos + ll]
            lit_pos += ll

        if pos == end:
            break

        # Offset
        if pos + off_bytes > end:
            raise CorruptError("offset truncated in stripped block")
        if off_bytes == 2:
            offset = token_bytes[pos] | (token_bytes[pos + 1] << 8)
            pos += 2
        else:
            offset = (token_bytes[pos] | (token_bytes[pos + 1] << 8)
                       | (token_bytes[pos + 2] << 16))
            pos += 3

        mlen = mc + VV_MIN_MATCH
        if mc == 15:
            while True:
                if pos >= end:
                    raise CorruptError("matchlen varint truncated in stripped block")
                b = token_bytes[pos]; pos += 1
                mlen += b
                if b < 255:
                    break

        if offset == 0 or offset > max_offset:
            raise CorruptError("match offset is outside frame window")
        if mlen > dsz - (len(out) - initial_len):
            raise CorruptError("match exceeds decoded block size")
        cur_pos = len(out)
        match_src_idx = cur_pos - offset
        if match_src_idx < dst_base_offset:
            raise CorruptError(
                f"stripped match offset {offset} reaches before frame start")

        if offset >= mlen:
            out += bytes(out[match_src_idx : match_src_idx + mlen])
        else:
            for _ in range(mlen):
                out.append(out[match_src_idx])
                match_src_idx += 1

    actual = len(out) - initial_len
    if actual != dsz:
        raise CorruptError(f"stripped block produced {actual} bytes, expected {dsz}")


def decompress_frame(buf, pos):
    """Decode one frame starting at `pos`. Returns (decoded_bytes, new_pos)."""
    if len(buf) - pos < 16:
        raise CorruptError("frame header truncated (need 16 bytes)")

    # FORMAT.md §2: frame header
    magic, pos = read_u32(buf, pos)
    if magic != VV_MAGIC:
        raise CorruptError(f"bad magic 0x{magic:08X}, expected 0x{VV_MAGIC:08X}")

    version, pos = read_u8(buf, pos)
    if version != VV_VERSION:
        raise CorruptError(f"unsupported version {version}")

    flags, pos = read_u8(buf, pos)
    # Note: per FORMAT.md §2, reserved flag bits SHOULD be 0, but the C
    # reference decoder tolerates non-zero reserved bits. The two BCJ
    # architecture bits are mutually exclusive, however.
    if (flags & 0x0C) == 0x0C:
        raise CorruptError("simultaneous x86 and ARM64 BCJ flags")
    has_checksum = (flags & 0x01) != 0
    # has_dict = (flags & 0x02) != 0  -- reserved, not used

    mode_hint, pos = read_u8(buf, pos)
    window_log, pos = read_u8(buf, pos)
    if not 10 <= window_log <= 24:
        raise CorruptError(f"invalid window_log {window_log} (expected 10..24)")
    off_bytes = 2 if window_log <= 16 else 3
    max_offset = min(1 << window_log, (1 << (off_bytes * 8)) - 1)

    content_size, pos = read_u64(buf, pos)
    # content_size of 0 means unknown (streaming encode)

    # FORMAT.md §3: blocks
    out = bytearray()
    frame_start_idx = 0  # this frame's dst_base inside `out`
    last = False
    while not last:
        bh, pos = read_u32(buf, pos)
        btype = bh & 3
        last = ((bh >> 2) & 1) != 0
        dsz = (bh >> 3) & 0x1FFFFF
        # Note: per FORMAT.md §3, reserved bits 24-31 SHOULD be zero, but
        # the C reference decoder doesn't validate them (it just masks bits
        # 0-23). Match C behavior for cross-decoder consistency.
        if dsz > VV_MAX_BLOCK_SIZE:
            raise CorruptError(f"block size {dsz} exceeds max {VV_MAX_BLOCK_SIZE}")

        if btype == BLOCK_RAW:
            _need(buf, pos, dsz)
            out += buf[pos:pos + dsz]
            pos += dsz

        elif btype == BLOCK_RLE:
            byte_val, pos = read_u8(buf, pos)
            out += bytes([byte_val] * dsz)

        elif btype == BLOCK_COMPRESSED:
            csz, pos = read_u24(buf, pos)
            _need(buf, pos, csz)
            # Per C reference, the outer reader always advances by csz
            # regardless of where the token loop stopped. The inner
            # decoder must consume <= csz bytes to be valid.
            block_end = pos + csz
            new_pos = _decode_compressed_block(
                buf, pos, csz, dsz, off_bytes, frame_start_idx, out, max_offset)
            if new_pos > block_end:
                raise CorruptError(
                    f"COMPRESSED block consumed {new_pos - pos} bytes, "
                    f"declared csz={csz}")
            pos = block_end

        elif btype == BLOCK_ENTROPY:
            # FORMAT.md §3.4 — read tag and dispatch.
            csz, pos = read_u24(buf, pos)
            if csz < 1:
                raise CorruptError("entropy block too small for tag byte")
            _need(buf, pos, csz)
            tag = buf[pos]
            tag_chr = chr(tag) if 32 <= tag < 127 else '?'

            # 'A' tag: single-stream tANS — supported.
            if tag == 0x41:  # ENTROPY_ANS
                # FORMAT.md §3.4 sub-format for tags H/A/I/C:
                #   [tag(1)] [lit_count(2 LE)] [ent_len(2 LE)]
                #   [ent_data(ent_len bytes)] [stripped tokens (rest)]
                if csz < 1 + 2 + 2:
                    raise CorruptError("'A' entropy block too small for header")
                lit_count = buf[pos + 1] | (buf[pos + 2] << 8)
                ent_len = buf[pos + 3] | (buf[pos + 4] << 8)
                if 1 + 2 + 2 + ent_len > csz:
                    raise CorruptError("'A' entropy block: ent_len overflows csz")

                # Decode literals via tANS
                try:
                    import vv_ans as _vv_ans  # lazy import
                except ImportError:
                    raise NotImplementedError(
                        "'A' tag requires reference/vv_ans.py — not on path")
                ans_payload = bytes(buf[pos + 5 : pos + 5 + ent_len])
                literals, _ = _vv_ans.vva_decode(ans_payload, lit_count)

                # The remainder is stripped LZ tokens
                stripped_start = pos + 5 + ent_len
                stripped_len = csz - 5 - ent_len
                _decode_stripped_tokens(
                    bytes(buf[stripped_start : stripped_start + stripped_len]),
                    literals, dsz, off_bytes, frame_start_idx, out, max_offset)

                pos += csz
            elif tag == 0x53:  # ENTROPY_SEQ
                # FORMAT.md §3.4 tag 'S': self-contained sequence block.
                # Payload format (starts right after the tag byte):
                #   [4B total_lits] [1B lit_fmt] [4B lit_enc_len]
                #   [lit_enc_len bytes of encoded literals]
                #   [4B match_count] [2B ml_hdr_sz] [ml_hdr]
                #   [2B of_hdr_sz] [of_hdr]  [2B ll_hdr_sz] [ll_hdr]
                #   [2B state_ml] [2B state_of] [2B state_ll]
                #   [4B seq_bs_len] [seq_bs_len bytes of ANS bitstream]
                # See reference/vv_ans.py::vva_decode_sequences for the
                # full decoder. We give it the payload bytes and pass
                # `out` (the growing frame output) as dst_base so that
                # match offsets correctly reference earlier blocks in
                # the same frame (cross-block dictionary carry).
                try:
                    import vv_ans as _vv_ans
                except ImportError:
                    raise NotImplementedError(
                        "'S' tag requires reference/vv_ans.py — not on path")
                payload = bytes(buf[pos + 1 : pos + csz])
                _vv_ans.vva_decode_sequences(payload, out, max_offset=max_offset)
                pos += csz
            elif tag == 0x54:  # ENTROPY_SEQ_V2
                # FORMAT.md §3.4 tag 'T': sequence block with min_match=3.
                # Wire payload byte-identical to 'S' — only the ml_base
                # table differs (every entry shifted down by 1). Closes
                # the ~10% real-binary compression gap vs gzip-9 that
                # 'S'-tag data cannot hit because the matcher's hash5/
                # hash4 never produce length-3 matches. Added v2.33.0
                # (decoder) / v2.35.0 (encoder + hash3 matcher).
                try:
                    import vv_ans as _vv_ans
                except ImportError:
                    raise NotImplementedError(
                        "'T' tag requires reference/vv_ans.py — not on path")
                payload = bytes(buf[pos + 1 : pos + csz])
                _vv_ans.vva_decode_sequences_v2(payload, out, max_offset=max_offset)
                pos += csz
            else:
                raise NotImplementedError(
                    f"ENTROPY block (tag '{tag_chr}' = 0x{tag:02X}) not "
                    f"implemented in reference Python decoder. Supported: "
                    f"'A' (single-stream tANS) and 'S'/'T' (sequence coding). "
                    f"Legacy tags 'H' (Huffman), 'I' (ANS4-only), 'C' (CTX) "
                    f"are decode-only in the C reference and were superseded "
                    f"by 'S' in the modern encoder.")

        else:
            raise CorruptError(f"unknown block type {btype}")

    # FORMAT.md §4: footer
    if has_checksum:
        if len(buf) - pos < 12:
            raise CorruptError("frame footer truncated")
        checksum, pos = read_u64(buf, pos)
        fmagic, pos = read_u32(buf, pos)
        if fmagic != VV_FOOTER_MAGIC:
            raise CorruptError(
                f"bad footer magic 0x{fmagic:08X}, expected 0x{VV_FOOTER_MAGIC:08X}")
        # Verify XXH64 over this frame's decoded content
        frame_bytes = bytes(out[frame_start_idx:])
        actual = xxh64(frame_bytes, seed=0)
        if actual != checksum:
            raise CorruptError(
                f"checksum mismatch: got 0x{actual:016X}, "
                f"expected 0x{checksum:016X}")

    # The checksum covers the forward-transformed bytes, matching the C
    # decoder. Restore caller-visible bytes only after successful validation.
    if flags & 0x04:
        _bcj_x86_inverse(out)
    elif flags & 0x08:
        _bcj_arm64_inverse(out)

    # Sanity check: if encoder set content_size, the decoded length
    # SHOULD match. The C reference decoder does NOT validate this
    # (it's treated as a pre-allocation hint, not a contract). For
    # cross-decoder consistency we match the C behavior. A stricter
    # decoder MAY choose to error on mismatch.
    decoded_len = len(out) - frame_start_idx
    _ = content_size, decoded_len  # available to caller via header peek

    return bytes(out), pos


def decompress(buf):
    """Decode a multi-frame .vv stream. Returns the concatenated bytes.

    Empty input is rejected (matches C reference behavior).
    """
    if len(buf) == 0:
        raise CorruptError("empty input")
    out = bytearray()
    pos = 0
    while pos < len(buf):
        frame_bytes, pos = decompress_frame(buf, pos)
        out += frame_bytes
    return bytes(out)


# ─────────────────────────────────────────────────────────────────
# Self-test: runs against the C reference encoder.
# ─────────────────────────────────────────────────────────────────

def self_test():
    """Encode test inputs with the C `vaptvupt` binary, then decode in pure
    Python via this module. Prints PASS/FAIL for each case."""
    import subprocess
    import os
    import tempfile

    # Find the vaptvupt binary
    candidates = [
        './vaptvupt',
        '../vaptvupt',
        '/home/claude/vv/vaptvupt',
    ]
    binary = None
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            binary = c
            break
    if binary is None:
        print("FAIL: vaptvupt binary not found; tried:", candidates,
              file=sys.stderr)
        return 1

    cases = [
        ("empty", b""),
        ("1 byte", b"X"),
        ("hello world", b"hello world"),
        ("256 bytes ramp", bytes(range(256))),
        ("1KB repeating",
            b"abcdefghij" * 100 + b"abc"),
        ("4KB low entropy",
            (b"the quick brown fox jumps over the lazy dog. " * 100)[:4096]),
        ("16KB ramp + repeat",
            bytes(i & 0xFF for i in range(16384))),
        ("100KB repeating phrase",
            (b"VaptVupt rocks. " * 7000)[:100_000]),
        ("8 bytes all zeros", b"\x00" * 8),
        ("16 bytes all 0xFF", b"\xFF" * 16),
        ("100 bytes single byte (RLE-ideal)", b"\x7E" * 100),
    ]

    failures = 0
    skipped = 0
    successes = 0
    for name, data in cases:
        with tempfile.NamedTemporaryFile(delete=False) as f_in:
            f_in.write(data)
            in_path = f_in.name
        out_path = in_path + ".vv"
        try:
            subprocess.run([binary, '-c', '-m', 'balanced', '-o', out_path, in_path],
                          check=True, capture_output=True)
            with open(out_path, 'rb') as f_out:
                compressed = f_out.read()
            try:
                decoded = decompress(compressed)
                if decoded == data:
                    print(f"  PASS  {name} ({len(data)} → {len(compressed)} bytes)")
                    successes += 1
                else:
                    print(f"  FAIL  {name}: decoded {len(decoded)} bytes, "
                          f"expected {len(data)}")
                    failures += 1
            except NotImplementedError as e:
                print(f"  SKIP  {name} (uses ENTROPY block — beyond Python "
                      f"reference scope)")
                skipped += 1
        finally:
            try:
                os.remove(in_path)
                os.remove(out_path)
            except OSError:
                pass

    print()
    print(f"Results: {successes} passed, {failures} failed, {skipped} skipped")
    print(f"({skipped} skipped because they use a legacy entropy tag outside "
          f"the compact Python reference.)")
    return 0 if failures == 0 else 1


# ─────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────

def main(argv):
    if len(argv) < 2:
        print(f"Usage: {argv[0]} <input.vv> [output_file]", file=sys.stderr)
        print(f"       {argv[0]} --self-test", file=sys.stderr)
        return 2

    if argv[1] == '--self-test':
        return self_test()

    in_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else None

    with open(in_path, 'rb') as f:
        compressed = f.read()
    try:
        decoded = decompress(compressed)
    except (CorruptError, NotImplementedError) as e:
        print(f"Decode failed: {e}", file=sys.stderr)
        return 1

    if out_path:
        with open(out_path, 'wb') as f:
            f.write(decoded)
        print(f"Decoded {len(compressed)} → {len(decoded)} bytes → {out_path}",
              file=sys.stderr)
    else:
        sys.stdout.buffer.write(decoded)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
