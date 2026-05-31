#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
VaptVupt — pure-Python single-stream Huffman decoder (`lit_fmt = 3`).

This module ports `vvh_decode` from `src/vv_huffman.c` to Python,
closing part of the reference-decoder coverage gap documented in
`AUDIT.md` Section 8 item 6 and `README.md` "Reference decoder
coverage gap".

Sprint 114 (v2.47.6): adds support for `lit_fmt = 3` (HUFFMAN —
single-stream Huffman literal coding, added to the wire format in
v2.46.0). Does NOT add support for `lit_fmt = 4` (HUFFMAN4 —
4-stream interleaved Huffman, added v2.47.0); that remains as
documented future work.

Wire format
-----------
A `lit_fmt = 3` literal section consists of:

    [1B max_sym]
    [⌈(max_sym + 2) / 2⌉ bytes of nibble-packed code lengths]
    [bitstream of canonical Huffman codes, LSB-first]

The header packs code lengths 4 bits each; for symbol pair (i, i+1):

    packed[i/2] = (lengths[i] << 4) | (lengths[i+1] & 0x0F)

Codes are canonical (sorted by length then symbol value) but are
stored bit-reversed in the bitstream so the LSB-first reader can
extract them with a simple right-shift after peek.

Constants mirror the C reference:

    VVH_SYMBOLS      = 256   (alphabet size)
    VVH_MAX_CODE_LEN = 15    (depth-limited)

Reference: `src/vv_huffman.c` in the C source, functions
`vvh_decode`, `read_header`, `assign_canonical_codes`, and
`build_dec_table`.
"""

from __future__ import annotations

# ─────────────────────────────────────────────────────────────────
# Constants — must match include/vv_huffman.h
# ─────────────────────────────────────────────────────────────────

VVH_SYMBOLS = 256
VVH_MAX_CODE_LEN = 15


class HuffmanError(Exception):
    """Raised on malformed Huffman header or bitstream."""


# ─────────────────────────────────────────────────────────────────
# Header decoding
# ─────────────────────────────────────────────────────────────────


def read_header(src: bytes, src_len: int) -> tuple[list[int], int]:
    """Decode the 4-bit-packed code-length header.

    Returns (lengths, hdr_size) where lengths is a 256-element list
    of code lengths (0 = symbol absent) and hdr_size is the number
    of bytes consumed.

    Raises HuffmanError on truncated or invalid header.
    """
    if src_len < 1:
        raise HuffmanError("Huffman header: empty buffer")

    max_sym = src[0]
    hdr_size = 1 + (max_sym + 2) // 2
    if hdr_size > src_len:
        raise HuffmanError(
            f"Huffman header: max_sym={max_sym} requires {hdr_size} "
            f"header bytes, only {src_len} available"
        )

    lengths = [0] * VVH_SYMBOLS
    i = 0
    while i <= max_sym:
        packed = src[1 + i // 2]
        lengths[i] = packed >> 4
        if i + 1 <= max_sym:
            lengths[i + 1] = packed & 0x0F
        i += 2

    return lengths, hdr_size


# ─────────────────────────────────────────────────────────────────
# Canonical-code assignment
# ─────────────────────────────────────────────────────────────────


def assign_canonical_codes(lengths: list[int]) -> list[int]:
    """Assign MSB-first canonical Huffman codes for the given lengths.

    Codes are returned as integers in the range [0, 2^len). They are
    NOT yet bit-reversed for LSB-first storage; the caller must do
    that step before populating the decode table.

    Mirrors `assign_canonical_codes` in `src/vv_huffman.c`.
    """
    bl_count = [0] * (VVH_MAX_CODE_LEN + 1)
    for i in range(VVH_SYMBOLS):
        ln = lengths[i]
        if 0 < ln <= VVH_MAX_CODE_LEN:
            bl_count[ln] += 1

    next_code = [0] * (VVH_MAX_CODE_LEN + 1)
    code = 0
    for bits in range(1, VVH_MAX_CODE_LEN + 1):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code

    codes = [0] * VVH_SYMBOLS
    for i in range(VVH_SYMBOLS):
        ln = lengths[i]
        if ln > 0:
            codes[i] = next_code[ln]
            next_code[ln] += 1

    return codes


def reverse_bits(value: int, n_bits: int) -> int:
    """Reverse the low `n_bits` bits of `value`."""
    result = 0
    for _ in range(n_bits):
        result = (result << 1) | (value & 1)
        value >>= 1
    return result


# ─────────────────────────────────────────────────────────────────
# Decode-table construction (linear-scan implementation)
#
# The C reference uses a 4096-entry fast-path table for codes ≤ 12 bits
# plus a slow-path linear scan for codes > 12 bits. For the Python
# reference we use a simple {(code, length): symbol} map with linear
# scan ordered shortest-length first. This is much slower (O(n) per
# symbol vs O(1)) but trivially correct, which is the point of a
# reference implementation. Production uses the C decoder.
# ─────────────────────────────────────────────────────────────────


def build_decode_codes(lengths: list[int]) -> list[tuple[int, int, int]]:
    """Build a list of (reversed_code, length, symbol) entries sorted
    by length ascending. The decoder peeks `length` bits and matches
    against `reversed_code`; the first match wins (canonical-code
    property guarantees uniqueness).
    """
    canonical = assign_canonical_codes(lengths)
    entries: list[tuple[int, int, int]] = []
    for sym in range(VVH_SYMBOLS):
        ln = lengths[sym]
        if ln == 0:
            continue
        if ln > VVH_MAX_CODE_LEN:
            raise HuffmanError(
                f"Huffman code length {ln} exceeds max {VVH_MAX_CODE_LEN}"
            )
        rev = reverse_bits(canonical[sym], ln)
        entries.append((rev, ln, sym))

    if not entries:
        raise HuffmanError("Huffman: no symbols with nonzero length")

    # Sort by length so the linear scan finds short codes first.
    entries.sort(key=lambda e: (e[1], e[0]))
    return entries


# ─────────────────────────────────────────────────────────────────
# Bitstream reader (LSB-first, mirrors C `br_t`)
# ─────────────────────────────────────────────────────────────────


class BitReader:
    """LSB-first bit reader over a bytes-like buffer.

    Mirrors `br_t` in `src/vv_huffman.c`. The accumulator holds up
    to 64 bits; refill loads bytes one at a time when the accumulator
    has room (≤ 56 bits used).
    """

    __slots__ = ("bits", "nbits", "src", "pos", "len")

    def __init__(self, src: bytes, length: int):
        self.bits = 0
        self.nbits = 0
        self.src = src
        self.pos = 0
        self.len = length

    def refill(self) -> None:
        while self.nbits <= 56 and self.pos < self.len:
            self.bits |= self.src[self.pos] << self.nbits
            self.pos += 1
            self.nbits += 8

    def peek(self, n: int) -> int:
        return self.bits & ((1 << n) - 1)

    def consume(self, n: int) -> None:
        self.bits >>= n
        self.nbits -= n


# ─────────────────────────────────────────────────────────────────
# Top-level decode
# ─────────────────────────────────────────────────────────────────


def vvh_decode(src: bytes, src_len: int, num_literals: int) -> tuple[bytes, int]:
    """Decode `num_literals` Huffman-coded bytes from `src[0:src_len]`.

    Returns (decoded_bytes, src_consumed). Raises HuffmanError on
    malformed input.

    Mirrors `vvh_decode` in `src/vv_huffman.c`.
    """
    if num_literals == 0:
        return b"", 0

    lengths, hdr_size = read_header(src, src_len)

    if not any(ln > 0 for ln in lengths):
        raise HuffmanError("Huffman: empty code-length table")

    entries = build_decode_codes(lengths)

    reader = BitReader(src[hdr_size:src_len], src_len - hdr_size)
    reader.refill()

    out = bytearray(num_literals)
    for i in range(num_literals):
        if reader.nbits < VVH_MAX_CODE_LEN:
            reader.refill()

        # Linear scan, shortest-length first. Canonical codes are
        # uniquely decodable so the first match is always correct.
        matched = False
        for rev_code, ln, sym in entries:
            if reader.nbits < ln:
                # Not enough bits for this code — bail (would need
                # refill, but if pos is at end we'd fall through to
                # the no-match path below).
                if reader.pos >= reader.len:
                    raise HuffmanError(
                        f"Huffman: bitstream exhausted at literal {i}/"
                        f"{num_literals}"
                    )
                reader.refill()
            mask = (1 << ln) - 1
            if (reader.bits & mask) == rev_code:
                reader.consume(ln)
                out[i] = sym
                matched = True
                break

        if not matched:
            raise HuffmanError(
                f"Huffman: no canonical code matched at literal {i}/"
                f"{num_literals}"
            )

    # src_consumed = hdr_size + bytes consumed by reader
    src_consumed = hdr_size + reader.pos
    # The reader may have over-read by (nbits / 8) bytes that are
    # still buffered. C reference subtracts those.
    over = reader.nbits // 8
    if src_consumed >= over:
        src_consumed -= over

    return bytes(out), src_consumed


# ─────────────────────────────────────────────────────────────────
# Self-test
# ─────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    # Build a tiny Huffman example by hand and verify round-trip.
    # Symbols 'A' (frequent), 'B' (medium), 'C' (rare) with hand-
    # picked code lengths {A:1, B:2, C:2}. Canonical codes:
    #   A: 1-bit  → code 0    (binary: 0)
    #   B: 2-bit  → code 10   (binary: 10)
    #   C: 2-bit  → code 11   (binary: 11)
    lengths = [0] * VVH_SYMBOLS
    lengths[ord("A")] = 1
    lengths[ord("B")] = 2
    lengths[ord("C")] = 2

    codes = assign_canonical_codes(lengths)
    print(f"A canonical code: {codes[ord('A')]:0{lengths[ord('A')]}b}")
    print(f"B canonical code: {codes[ord('B')]:0{lengths[ord('B')]}b}")
    print(f"C canonical code: {codes[ord('C')]:0{lengths[ord('C')]}b}")

    print("Self-test for `vvh_decode` requires a real .vv frame —")
    print("see the round-trip integration in `vv_decoder.test.py`.")
