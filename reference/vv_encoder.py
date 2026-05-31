#!/usr/bin/env python3
"""
VaptVupt reference encoder — Python implementation.

Implements compression to a valid VaptVupt frame using only the
simplest block types: RAW (stored) and RLE (run-length).

Scope intentionally limited:
  - This encoder produces NO matches and NO entropy coding.
  - It WILL produce larger output than the C encoder for compressible
    data — but the output IS a valid .vv frame readable by ANY
    decoder (vv_decompress, the Python decoder, etc.).
  - The purpose is to prove the encoder side of FORMAT.md by having
    two independent encoders both produce valid frames.

The encoder picks RLE for runs ≥ 32 bytes (where RLE saves more than
the 5-byte block-header overhead pays back), and RAW for everything
else.

Round-trip validation:
    Python encode → C decode  : proves encoder produces valid frames
    C encode      → Python decode : (already proven by the decoder)

Usage:
    python3 vv_encoder.py <input> [output.vv]
    python3 vv_encoder.py --self-test
"""

import os
import struct
import sys

# Make XXH64 importable from the decoder module (single source of truth).
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vv_decoder  # noqa: E402

# Constants from FORMAT.md (must match decoder's set)
VV_MAGIC          = vv_decoder.VV_MAGIC
VV_FOOTER_MAGIC   = vv_decoder.VV_FOOTER_MAGIC
VV_VERSION        = vv_decoder.VV_VERSION
VV_MAX_BLOCK_SIZE = vv_decoder.VV_MAX_BLOCK_SIZE

BLOCK_RAW = vv_decoder.BLOCK_RAW
BLOCK_RLE = vv_decoder.BLOCK_RLE


# ─────────────────────────────────────────────────────────────────
# Block header encoding (FORMAT.md §3)
# ─────────────────────────────────────────────────────────────────

def pack_block_header(btype, last, dsz):
    """Pack 4-byte block header per FORMAT.md §3."""
    if not (0 <= btype <= 3):
        raise ValueError(f"btype out of range: {btype}")
    if dsz > 0x1FFFFF:
        raise ValueError(f"dsz {dsz} exceeds 21-bit field max")
    bh = (btype & 3) | ((1 if last else 0) << 2) | ((dsz & 0x1FFFFF) << 3)
    return struct.pack('<I', bh)


# ─────────────────────────────────────────────────────────────────
# RLE detection: find runs of identical bytes
# ─────────────────────────────────────────────────────────────────

# An RLE block costs 4 (block header) + 1 (RLE byte) = 5 bytes overhead.
# A RAW block costs 4 (block header) bytes overhead.
# So RLE is profitable when run length > 5 + 4 = 9 bytes (saves output).
# We use 32 as a conservative threshold to avoid producing tiny RLE
# blocks that fragment the output.
RLE_MIN_LENGTH = 32


def find_runs(data):
    """Find non-overlapping runs of identical bytes ≥ RLE_MIN_LENGTH.

    Yields (start, length, byte) tuples, in order. Bytes between runs
    are implicit RAW spans (not yielded).
    """
    n = len(data)
    i = 0
    while i < n:
        # Look for a run starting at i
        j = i
        while j + 1 < n and data[j + 1] == data[i]:
            j += 1
        run_len = j - i + 1
        if run_len >= RLE_MIN_LENGTH:
            yield (i, run_len, data[i])
        i = j + 1


def split_blocks(data):
    """Split data into a sequence of (btype, payload) blocks.

    Each block payload is the raw bytes for a RAW block, or a single
    byte plus a length for an RLE block.
    """
    runs = list(find_runs(data))
    if not runs:
        # No qualifying runs: one big RAW block (or several if > BLOCK_MAX)
        for chunk_start in range(0, len(data), VV_MAX_BLOCK_SIZE):
            chunk = data[chunk_start:chunk_start + VV_MAX_BLOCK_SIZE]
            yield (BLOCK_RAW, chunk)
        return

    cursor = 0
    for run_start, run_len, run_byte in runs:
        # Emit a RAW block for any data before this run
        if run_start > cursor:
            raw_data = data[cursor:run_start]
            for chunk_start in range(0, len(raw_data), VV_MAX_BLOCK_SIZE):
                chunk = raw_data[chunk_start:chunk_start + VV_MAX_BLOCK_SIZE]
                yield (BLOCK_RAW, chunk)
        # Emit RLE block(s); split if longer than max block size
        remaining = run_len
        while remaining > 0:
            sub = min(remaining, VV_MAX_BLOCK_SIZE)
            yield (BLOCK_RLE, (run_byte, sub))
            remaining -= sub
        cursor = run_start + run_len

    # Tail RAW after the last run
    if cursor < len(data):
        raw_data = data[cursor:]
        for chunk_start in range(0, len(raw_data), VV_MAX_BLOCK_SIZE):
            chunk = raw_data[chunk_start:chunk_start + VV_MAX_BLOCK_SIZE]
            yield (BLOCK_RAW, chunk)


# ─────────────────────────────────────────────────────────────────
# Frame encoder
# ─────────────────────────────────────────────────────────────────

def encode(data, mode_hint=1, window_log=16, checksum=True):
    """Encode `data` as a single VaptVupt frame.

    Args:
        data: bytes-like to compress.
        mode_hint: informational byte stored in the frame header.
        window_log: stored in frame header. We don't emit any matches,
            so window_log only affects whether a downstream COMPRESSED
            block (which we don't produce) would use 2-byte or 3-byte
            offsets. 16 is a safe default.
        checksum: include the 12-byte XXH64 footer (recommended).

    Returns: bytes — a complete .vv frame.
    """
    data = bytes(data)
    n = len(data)

    # ─── Frame header (FORMAT.md §2) ───
    flags = 0x01 if checksum else 0x00
    header = struct.pack('<IBBBB Q',
                          VV_MAGIC, VV_VERSION, flags,
                          mode_hint & 0xFF, window_log & 0xFF, n)
    # Note: struct format 'IBBBB Q' aligns Q to 8 — but our fields are
    # already laid out 4+1+1+1+1 = 8 bytes followed by Q at offset 8.
    # struct adds no padding for '<' (little-endian, no alignment).
    assert len(header) == 16, f"header is {len(header)} bytes"

    # ─── Blocks (FORMAT.md §3) ───
    blocks_out = bytearray()
    block_list = list(split_blocks(data))
    if not block_list:
        # Special case: empty input. Emit one RAW block with dsz=0.
        block_list = [(BLOCK_RAW, b'')]

    for i, (btype, payload) in enumerate(block_list):
        is_last = (i == len(block_list) - 1)
        if btype == BLOCK_RAW:
            blocks_out += pack_block_header(BLOCK_RAW, is_last, len(payload))
            blocks_out += payload
        elif btype == BLOCK_RLE:
            byte_val, run_len = payload
            blocks_out += pack_block_header(BLOCK_RLE, is_last, run_len)
            blocks_out += bytes([byte_val])
        else:
            raise NotImplementedError(
                f"Python reference encoder only supports RAW and RLE; "
                f"asked for block type {btype}")

    # ─── Footer (FORMAT.md §4) ───
    footer = b''
    if checksum:
        cs = vv_decoder.xxh64(data, seed=0)
        footer = struct.pack('<QI', cs, VV_FOOTER_MAGIC)
        assert len(footer) == 12

    return bytes(header) + bytes(blocks_out) + footer


# ─────────────────────────────────────────────────────────────────
# Self-test: round-trip Python-encoded data through both decoders
# ─────────────────────────────────────────────────────────────────

def self_test():
    """Encode various inputs in pure Python, then decode them with BOTH
    the Python reference decoder AND the C reference binary. Both must
    return the original bytes exactly."""
    import subprocess
    import tempfile

    # Find the C binary
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
        print(f"FAIL: vaptvupt binary not found; tried: {candidates}",
              file=sys.stderr)
        return 1

    cases = [
        ("empty", b""),
        ("1 byte", b"X"),
        ("hello world", b"hello world from python encoder"),
        ("256-byte ramp", bytes(range(256))),
        ("64-byte run", b"A" * 64),
        ("RLE-ideal: 100 zeros", b"\x00" * 100),
        ("RLE then raw", b"\xCC" * 50 + b"hello world"),
        ("Raw then RLE", b"hello world" + b"\xCC" * 50),
        ("Raw + RLE + Raw", b"abc" + b"X" * 64 + b"def"),
        ("Multiple RLEs", b"A" * 50 + b"BCD" + b"E" * 50 + b"FGH" + b"I" * 50),
        ("4KB no runs",
            bytes((i * 17 + (i >> 3)) & 0xFF for i in range(4096))),
        ("32 KB mostly zeros (block-size split)", b"\x00" * 32768),
        ("1 MB single byte (block-size cap)", b"\xAB" * 1_000_000),
    ]

    failures = 0
    successes = 0
    for name, data in cases:
        # Encode in Python
        try:
            frame = encode(data)
        except Exception as e:
            print(f"  FAIL  {name}: encode error: {e}")
            failures += 1
            continue

        # Decode with the Python reference decoder
        try:
            decoded_py = vv_decoder.decompress(frame)
        except Exception as e:
            print(f"  FAIL  {name}: Py-decoded raised {type(e).__name__}: {e}")
            failures += 1
            continue
        if decoded_py != data:
            print(f"  FAIL  {name}: Py decoder produced {len(decoded_py)} "
                  f"bytes, expected {len(data)}")
            failures += 1
            continue

        # Decode with the C binary
        with tempfile.NamedTemporaryFile(delete=False, suffix='.vv') as f_in:
            f_in.write(frame)
            in_path = f_in.name
        out_path = in_path + '.dec'
        try:
            result = subprocess.run(
                [binary, '-d', '-o', out_path, in_path],
                capture_output=True, timeout=15)
            if result.returncode != 0:
                print(f"  FAIL  {name}: C decoder rejected our frame: "
                      f"{result.stderr.decode('utf-8', errors='replace').strip()}")
                failures += 1
                continue
            with open(out_path, 'rb') as f:
                decoded_c = f.read()
            if decoded_c != data:
                print(f"  FAIL  {name}: C decoder produced {len(decoded_c)} "
                      f"bytes, expected {len(data)}")
                failures += 1
                continue
            print(f"  PASS  {name} ({len(data)} → {len(frame)} bytes)")
            successes += 1
        finally:
            try:
                os.remove(in_path)
                if os.path.exists(out_path):
                    os.remove(out_path)
            except OSError:
                pass

    print()
    print(f"Results: {successes} passed, {failures} failed")
    print(f"(All cases round-trip through BOTH the Python decoder AND "
          f"the C decoder, proving the Python encoder produces wire-"
          f"compatible frames.)")
    return 0 if failures == 0 else 1


# ─────────────────────────────────────────────────────────────────
# CLI
# ─────────────────────────────────────────────────────────────────

def main(argv):
    if len(argv) < 2:
        print(f"Usage: {argv[0]} <input> [output.vv]", file=sys.stderr)
        print(f"       {argv[0]} --self-test", file=sys.stderr)
        return 2

    if argv[1] == '--self-test':
        return self_test()

    in_path = argv[1]
    out_path = argv[2] if len(argv) > 2 else in_path + '.vv'

    with open(in_path, 'rb') as f:
        data = f.read()
    frame = encode(data)
    with open(out_path, 'wb') as f:
        f.write(frame)
    ratio = len(data) / max(len(frame), 1)
    print(f"Encoded {len(data)} → {len(frame)} bytes ({ratio:.2f}:1) → {out_path}",
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
