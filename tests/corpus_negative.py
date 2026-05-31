#!/usr/bin/env python3
"""
VaptVupt — Negative Test Corpus Generator and Cross-Decoder Test Runner.

This script:
  1. Generates a directory of deliberately-malformed .vv files
     covering specific spec violations (bad magic, truncated frames,
     corrupted headers, invalid block types, etc.)
  2. Runs each file through BOTH the C reference decoder
     (`./vaptvupt -d`) AND the Python reference decoder.
  3. Verifies they reject identically.

This catches an entire class of cross-implementation bugs where one
decoder might accept malformed input that another rejects (a security
hazard for Zupt's archive verification flow).

Usage:
    python3 tests/corpus_negative.py             # Generate + run all tests
    python3 tests/corpus_negative.py --gen-only  # Generate corpus, no run
"""

import os
import sys
import struct
import subprocess
import shutil
import argparse

# Make the python reference decoder importable
HERE = os.path.dirname(os.path.abspath(__file__))
VV_ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(VV_ROOT, 'reference'))
import vv_decoder  # noqa: E402

CORPUS_DIR = os.path.join(VV_ROOT, 'tests', 'corpus_bad')
VV_BINARY  = os.path.join(VV_ROOT, 'vaptvupt')


# ─────────────────────────────────────────────────────────────────
# Build a valid baseline frame using the C encoder (input we'll mutate)
# ─────────────────────────────────────────────────────────────────

def build_baseline_frame():
    """Compress 'hello world' with the C encoder, returns bytes."""
    if not os.path.isfile(VV_BINARY) or not os.access(VV_BINARY, os.X_OK):
        sys.exit(f"FATAL: {VV_BINARY} not found or not executable. "
                 "Run `make` first.")
    in_path = '/tmp/_vv_baseline_in'
    out_path = '/tmp/_vv_baseline.vv'
    with open(in_path, 'wb') as f:
        f.write(b'hello world from VaptVupt')
    subprocess.run([VV_BINARY, '-c', '-m', 'balanced', '-o', out_path, in_path],
                   check=True, capture_output=True)
    with open(out_path, 'rb') as f:
        data = f.read()
    os.remove(in_path)
    os.remove(out_path)
    return data


# ─────────────────────────────────────────────────────────────────
# Corpus generators — each returns (filename, mutated_bytes)
# ─────────────────────────────────────────────────────────────────

def gen_corpus(baseline):
    """Yield (label, bytes) pairs of malformed inputs."""
    yield 'empty.vv', b''
    yield '1byte.vv', b'\x00'
    yield '15bytes_too_short_for_header.vv', b'\x00' * 15

    # Magic mutations
    bad_magic_1 = bytearray(baseline)
    bad_magic_1[0] = 0xFF
    yield 'magic_byte0_corrupted.vv', bytes(bad_magic_1)

    bad_magic_2 = bytearray(baseline)
    bad_magic_2[3] = 0xFF
    yield 'magic_byte3_corrupted.vv', bytes(bad_magic_2)

    # Version mutations
    bad_version = bytearray(baseline)
    bad_version[4] = 2
    yield 'version_2_unknown.vv', bytes(bad_version)

    bad_version_zero = bytearray(baseline)
    bad_version_zero[4] = 0
    yield 'version_0_unknown.vv', bytes(bad_version_zero)

    bad_version_max = bytearray(baseline)
    bad_version_max[4] = 0xFF
    yield 'version_FF_unknown.vv', bytes(bad_version_max)

    # Reserved flag bits set
    bad_flags = bytearray(baseline)
    bad_flags[5] |= 0x80
    yield 'flags_reserved_bit7_set.vv', bytes(bad_flags)

    bad_flags2 = bytearray(baseline)
    bad_flags2[5] |= 0x04
    yield 'flags_reserved_bit2_set.vv', bytes(bad_flags2)

    # Window log out of range
    bad_wlog_low = bytearray(baseline)
    bad_wlog_low[7] = 9   # below allowed [10..27]
    yield 'window_log_9_too_low.vv', bytes(bad_wlog_low)

    bad_wlog_high = bytearray(baseline)
    bad_wlog_high[7] = 28  # above allowed [10..27]
    yield 'window_log_28_too_high.vv', bytes(bad_wlog_high)

    bad_wlog_zero = bytearray(baseline)
    bad_wlog_zero[7] = 0
    yield 'window_log_0_invalid.vv', bytes(bad_wlog_zero)

    # Truncations at every meaningful boundary
    yield 'trunc_after_4byte_magic.vv', baseline[:4]
    yield 'trunc_inside_header.vv', baseline[:10]
    yield 'trunc_at_header_boundary.vv', baseline[:16]
    yield 'trunc_inside_block_header.vv', baseline[:18]
    yield 'trunc_inside_block_data.vv', baseline[:25]
    yield 'trunc_one_byte_short_of_footer.vv', baseline[:-1]
    yield 'trunc_in_middle_of_footer.vv', baseline[:-6]

    # Footer magic mutations (only fires for has_checksum frames)
    if (baseline[5] & 0x01):
        bad_footer = bytearray(baseline)
        bad_footer[-4] = 0xAA
        yield 'footer_magic_byte0_corrupted.vv', bytes(bad_footer)

        bad_footer2 = bytearray(baseline)
        bad_footer2[-1] = 0xAA
        yield 'footer_magic_byte3_corrupted.vv', bytes(bad_footer2)

        # Corrupt one byte of the checksum
        bad_chksum = bytearray(baseline)
        bad_chksum[-12] ^= 0x01  # flip lowest checksum bit
        yield 'footer_checksum_corrupted.vv', bytes(bad_chksum)

    # Block header reserved bits set (bits 24-31 must be zero)
    bad_blkh = bytearray(baseline)
    bh = struct.unpack_from('<I', bad_blkh, 16)[0]
    struct.pack_into('<I', bad_blkh, 16, bh | 0x01000000)
    yield 'block_header_reserved_bit24_set.vv', bytes(bad_blkh)

    # Block-type RAW: claim huge dsz that doesn't fit
    huge_blk = bytearray(baseline)
    # block header at offset 16: type (bits 0-1)=0 (RAW), last=1 (bit 2),
    # dsz at bits 3-23 = absurdly large
    new_bh = 0 | (1 << 2) | (0x100000 << 3)  # RAW, last, dsz=1MB
    struct.pack_into('<I', huge_blk, 16, new_bh)
    yield 'block_dsz_lies_about_size.vv', bytes(huge_blk)

    # Two valid frames concatenated, then garbage — must reject the garbage
    yield 'two_frames_then_garbage.vv', baseline + baseline + b'\xFF\xFF\xFF'

    # Concatenated identical frames (valid case for sanity)
    yield 'two_valid_frames.vv', baseline + baseline


# ─────────────────────────────────────────────────────────────────
# Cross-decoder test runner
# ─────────────────────────────────────────────────────────────────

def run_c_decoder(path):
    """Returns (success_bool, err_msg). success_bool=True if decoded OK."""
    out_path = path + '.dec'
    try:
        result = subprocess.run(
            [VV_BINARY, '-d', '-o', out_path, path],
            capture_output=True, timeout=10
        )
        ok = (result.returncode == 0 and os.path.exists(out_path))
        if os.path.exists(out_path):
            os.remove(out_path)
        if ok:
            return True, None
        else:
            err = result.stderr.decode('utf-8', errors='replace').strip()
            return False, err or f"returncode={result.returncode}"
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def run_py_decoder(path):
    """Returns (success_bool, err_msg)."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
        vv_decoder.decompress(data)
        return True, None
    except (vv_decoder.CorruptError, NotImplementedError) as e:
        return False, f"{type(e).__name__}: {e}"
    except Exception as e:
        return False, f"UNEXPECTED {type(e).__name__}: {e}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen-only', action='store_true',
                    help='Only generate the corpus, do not run tests')
    args = ap.parse_args()

    # Clean and regenerate the corpus directory
    if os.path.isdir(CORPUS_DIR):
        shutil.rmtree(CORPUS_DIR)
    os.makedirs(CORPUS_DIR)

    print(f"Building baseline frame via {VV_BINARY}...")
    baseline = build_baseline_frame()
    print(f"Baseline frame: {len(baseline)} bytes")

    paths = []
    for name, data in gen_corpus(baseline):
        out = os.path.join(CORPUS_DIR, name)
        with open(out, 'wb') as f:
            f.write(data)
        paths.append((name, out, len(data)))

    print(f"\nGenerated {len(paths)} corpus files in {CORPUS_DIR}")

    if args.gen_only:
        return 0

    # Cross-decoder check: each file must produce the same accept/reject
    # result in both decoders. Mismatches are bugs.
    print("\nCross-decoder verification:")
    print(f"  {'file':<46s}  {'C':<8s} {'Py':<8s} {'consistent?'}")
    print(f"  {'-' * 46}  {'-' * 8} {'-' * 8} {'-' * 12}")
    matches = 0
    mismatches = 0
    skipped = 0
    py_notimpl = 0  # Python returns NotImplementedError for entropy blocks
    for name, path, size in paths:
        c_ok, c_err = run_c_decoder(path)
        py_ok, py_err = run_py_decoder(path)

        # If Python failed with NotImplementedError, the file is
        # well-formed enough to reach an entropy block — Python can't
        # judge, so skip the cross-check (the C result is authoritative).
        if py_err and 'NotImplementedError' in py_err:
            skipped += 1
            py_notimpl += 1
            print(f"  {name:<46s}  {'OK' if c_ok else 'FAIL':<8s} "
                  f"{'NIE':<8s} (skip — entropy block)")
            continue

        consistent = (c_ok == py_ok)
        marker = '✓' if consistent else '✗ MISMATCH'
        c_str = 'OK' if c_ok else 'FAIL'
        py_str = 'OK' if py_ok else 'FAIL'
        print(f"  {name:<46s}  {c_str:<8s} {py_str:<8s} {marker}")
        if consistent:
            matches += 1
        else:
            mismatches += 1
            # Show the error messages to aid debugging
            if c_err:
                print(f"      C error : {c_err[:80]}")
            if py_err:
                print(f"      Py error: {py_err[:80]}")

    print()
    print(f"Results: {matches} consistent, {mismatches} mismatched, "
          f"{skipped} skipped (Python NotImplementedError)")
    return 0 if mismatches == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
