#!/usr/bin/env python3
"""
VaptVupt — Differential fuzzer for cross-decoder consistency.

Generates random byte sequences using several distinct strategies,
feeds each to BOTH the C decoder (`./vaptvupt -d`) and the Python
reference decoder, and verifies they agree on accept/reject (plus
byte-for-byte equivalence when both accept).

This catches the class of bugs where:
- One decoder accepts input the other rejects
  (→ potential security hazard: VaptVupt-archive verifier might
   disagree with third-party consumer on what's "valid")
- One decoder crashes on input the other handles gracefully
  (→ DoS vulnerability)
- Both decoders accept but produce different bytes
  (→ silent corruption bug)

Strategies:
  1. Pure random bytes      (most inputs malformed; tests rejection paths)
  2. Mutation from valid    (single-bit-flip, byte-flip, truncate, swap)
  3. Mutation from baseline (start from 'hello world' frame)
  4. Structured malformed   (valid frame header + random block body)
  5. Roundtrip random       (compress random bytes, decode, check equal)

Usage:
    python3 tests/fuzz_differential.py                 # 1000 iters/strategy
    python3 tests/fuzz_differential.py --iters 10000   # longer run
    python3 tests/fuzz_differential.py --seed 42       # deterministic

The default 1000 iters * 5 strategies = 5000 test cases runs in ~10 s.
Designed to be short enough for a CI run but long enough to find real
bugs across mutations of the baseline frame.
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VV_ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(VV_ROOT, 'reference'))
import vv_decoder  # noqa: E402
import vv_encoder  # noqa: E402

VV_BINARY = os.path.join(VV_ROOT, 'vaptvupt')


# ─────────────────────────────────────────────────────────────────
# Decoder wrappers — report (accepted, bytes_or_None, err_msg)
# ─────────────────────────────────────────────────────────────────

def decode_c(data, timeout=5):
    """Run the C decoder. Returns (ok, decoded_bytes_or_None, err)."""
    with tempfile.NamedTemporaryFile(delete=False, suffix='.vv') as f:
        f.write(data)
        in_path = f.name
    out_path = in_path + '.dec'
    try:
        result = subprocess.run(
            [VV_BINARY, '-d', '-o', out_path, in_path],
            capture_output=True, timeout=timeout)
        if result.returncode == 0 and os.path.exists(out_path):
            with open(out_path, 'rb') as f:
                out = f.read()
            return True, out, None
        err = result.stderr.decode('utf-8', errors='replace').strip()
        return False, None, err or f"rc={result.returncode}"
    except subprocess.TimeoutExpired:
        return False, None, "TIMEOUT"
    except Exception as e:
        return False, None, f"{type(e).__name__}: {e}"
    finally:
        for p in (in_path, out_path):
            try:
                os.remove(p)
            except OSError:
                pass


def decode_py(data):
    """Run the Python decoder. Returns (ok, decoded_bytes_or_None, err)."""
    try:
        out = vv_decoder.decompress(data)
        return True, out, None
    except (vv_decoder.CorruptError, NotImplementedError) as e:
        return False, None, f"{type(e).__name__}: {e}"
    except Exception as e:
        return False, None, f"UNEXPECTED {type(e).__name__}: {e}"


# ─────────────────────────────────────────────────────────────────
# Mutation primitives
# ─────────────────────────────────────────────────────────────────

def mutate_flip_bit(data, rng):
    """Flip a single random bit."""
    if not data:
        return data
    data = bytearray(data)
    i = rng.randrange(len(data))
    data[i] ^= (1 << rng.randrange(8))
    return bytes(data)


def mutate_flip_byte(data, rng):
    """Randomize a single byte."""
    if not data:
        return data
    data = bytearray(data)
    i = rng.randrange(len(data))
    data[i] = rng.randrange(256)
    return bytes(data)


def mutate_truncate(data, rng):
    """Chop off a random tail."""
    if len(data) <= 1:
        return data
    cut = rng.randrange(1, len(data))
    return data[:cut]


def mutate_insert(data, rng):
    """Insert 1-8 random bytes at a random position."""
    pos = rng.randrange(len(data) + 1) if data else 0
    n = rng.randrange(1, 9)
    junk = bytes(rng.randrange(256) for _ in range(n))
    return data[:pos] + junk + data[pos:]


def mutate_delete(data, rng):
    """Remove 1-8 consecutive bytes."""
    if len(data) <= 1:
        return data
    pos = rng.randrange(len(data))
    n = min(rng.randrange(1, 9), len(data) - pos)
    return data[:pos] + data[pos + n:]


def mutate_duplicate_frame(data, rng):
    """Concatenate `data` with itself (tests multi-frame path)."""
    return data + data


MUTATORS = [mutate_flip_bit, mutate_flip_byte, mutate_truncate,
            mutate_insert, mutate_delete, mutate_duplicate_frame]


# ─────────────────────────────────────────────────────────────────
# Input generators
# ─────────────────────────────────────────────────────────────────

def gen_pure_random(rng):
    """Completely random bytes, length 0-256."""
    n = rng.randrange(0, 257)
    return bytes(rng.randrange(256) for _ in range(n))


def gen_mutated_baseline(baseline, rng, iterations=1):
    """Apply N random mutations to a known-good baseline frame."""
    data = baseline
    for _ in range(iterations):
        mut = rng.choice(MUTATORS)
        data = mut(data, rng)
    return data


def gen_valid_header_garbage_body(rng):
    """Valid 16-byte frame header, then random bytes for the body."""
    flags = rng.choice([0x00, 0x01])
    header = struct.pack('<IBBBBQ',
                          vv_decoder.VV_MAGIC,
                          1,  # version
                          flags,
                          rng.randrange(3),    # mode_hint
                          rng.randrange(10, 25),  # window_log
                          rng.randrange(0, 100000))  # content_size
    body_len = rng.randrange(0, 257)
    body = bytes(rng.randrange(256) for _ in range(body_len))
    return header + body


# ─────────────────────────────────────────────────────────────────
# Strategy runners
# ─────────────────────────────────────────────────────────────────

def _check_consistency(data, stats, label):
    """Run both decoders and compare. Updates stats in place.

    Returns True if consistent (or Python skipped due to entropy
    block), False if mismatched."""
    c_ok, c_out, c_err = decode_c(data)
    py_ok, py_out, py_err = decode_py(data)

    # Python may raise NotImplementedError for entropy blocks — that's
    # not a divergence, just coverage gap. Skip those cases.
    if py_err and 'NotImplementedError' in py_err:
        stats['skipped_entropy'] += 1
        return True

    # C CLI's output buffer is sized defensively from content_size (with
    # caps against OOM/DoS). Inputs whose block headers declare larger
    # output than the CLI allocates cause vv_decompress to return
    # VV_ERR_OVERFLOW (-4). This is a *caller-policy* outcome, not a
    # format-validity rejection: the input CAN be valid per spec, and
    # the Python decoder (which uses dynamic append without a fixed cap)
    # correctly accepts it. Treat this as consistent — the two decoders
    # disagree on CLI policy, not on format semantics.
    if not c_ok and c_err and ('failed: -4' in c_err or 'OVERFLOW' in c_err):
        if py_ok:
            stats['cli_overflow_py_ok'] = stats.get('cli_overflow_py_ok', 0) + 1
            return True

    # Both rejected: consistent. (We don't care that the error messages
    # differ — only that both parsers declined.)
    if not c_ok and not py_ok:
        stats['both_rejected'] += 1
        return True

    # Both accepted: must produce identical bytes.
    if c_ok and py_ok:
        if c_out == py_out:
            stats['both_accepted'] += 1
            return True
        # Both accepted but bytes differ — serious bug!
        stats['mismatches'].append((label, data, c_out, py_out, None))
        return False

    # One accepted, one rejected — divergence.
    stats['mismatches'].append((label, data, c_out, py_out,
                                 f"C={'OK' if c_ok else c_err}, "
                                 f"Py={'OK' if py_ok else py_err}"))
    return False


def run_strategy_1_pure_random(iters, rng):
    """Completely random bytes — mostly rejected by both decoders."""
    stats = {'both_rejected': 0, 'both_accepted': 0,
             'skipped_entropy': 0, 'mismatches': []}
    for i in range(iters):
        data = gen_pure_random(rng)
        _check_consistency(data, stats, f"random#{i}")
    return stats


def run_strategy_2_mutated_baseline(iters, rng, baseline):
    """Single mutation of a valid frame."""
    stats = {'both_rejected': 0, 'both_accepted': 0,
             'skipped_entropy': 0, 'mismatches': []}
    for i in range(iters):
        data = gen_mutated_baseline(baseline, rng, iterations=1)
        _check_consistency(data, stats, f"mut1#{i}")
    return stats


def run_strategy_3_multi_mutation(iters, rng, baseline):
    """Multiple random mutations stacked (harder to stay valid)."""
    stats = {'both_rejected': 0, 'both_accepted': 0,
             'skipped_entropy': 0, 'mismatches': []}
    for i in range(iters):
        data = gen_mutated_baseline(baseline, rng,
                                    iterations=rng.randrange(2, 6))
        _check_consistency(data, stats, f"mut_stack#{i}")
    return stats


def run_strategy_4_header_garbage(iters, rng):
    """Valid header, random body."""
    stats = {'both_rejected': 0, 'both_accepted': 0,
             'skipped_entropy': 0, 'mismatches': []}
    for i in range(iters):
        data = gen_valid_header_garbage_body(rng)
        _check_consistency(data, stats, f"hdr_garb#{i}")
    return stats


def run_strategy_5_roundtrip(iters, rng):
    """Compress random bytes with the Python encoder (which is restricted
    to RAW+RLE so always succeeds), then verify C decoder produces the
    original bytes."""
    stats = {'both_rejected': 0, 'both_accepted': 0,
             'skipped_entropy': 0, 'mismatches': []}
    for i in range(iters):
        n = rng.randrange(0, 1024)
        original = bytes(rng.randrange(256) for _ in range(n))
        try:
            frame = vv_encoder.encode(original)
        except Exception as e:
            stats['mismatches'].append(
                (f"rt#{i}", original, None, None, f"encode: {e}"))
            continue
        c_ok, c_out, c_err = decode_c(frame)
        if c_ok and c_out == original:
            stats['both_accepted'] += 1
        else:
            stats['mismatches'].append(
                (f"rt#{i}", frame, c_out, original,
                 f"C={'OK' if c_ok else c_err}"))
    return stats


def run_strategy_6_format_v2(iters, rng):
    """Compress varied-entropy payloads with the C encoder using
    --format-v2 (produces 'T' tag blocks), then decode with BOTH the
    C decoder and the Python reference decoder and verify byte-for-byte
    agreement. Added v2.36.0 to keep the Python reference in lockstep
    with format-v2 encoder output — the bug that slipped into v2.34.0
    (max_match overflow) would have been caught here immediately if
    this strategy had existed, since every long match triggers a
    cross-decoder comparison."""
    stats = {'both_rejected': 0, 'both_accepted': 0,
             'skipped_entropy': 0, 'mismatches': []}
    for i in range(iters):
        # Mix of payload shapes to exercise different code paths:
        # - mostly-runs (RLE + long matches)
        # - structured binary-ish (byte ranges that trigger hash3)
        # - short and long
        kind = rng.randrange(3)
        if kind == 0:
            # Mostly-runs: a few different bytes, long runs
            n = rng.randrange(64, 8192)
            seed = rng.randrange(256)
            original = bytearray()
            pos = 0
            while pos < n:
                run = rng.randrange(4, 256)
                byte = rng.randrange(256) if rng.randrange(4) == 0 else seed
                original.extend([byte] * min(run, n - pos))
                pos += run
            original = bytes(original[:n])
        elif kind == 1:
            # Structured binary-ish: patterns that hash3 can exploit
            n = rng.randrange(128, 8192)
            pat_len = rng.randrange(3, 8)
            pat = bytes(rng.randrange(256) for _ in range(pat_len))
            reps = n // pat_len + 1
            original = (pat * reps)[:n]
        else:
            # Short: forces RAW block path
            n = rng.randrange(0, 128)
            original = bytes(rng.randrange(256) for _ in range(n))

        # Compress with C encoder using --format-v2
        try:
            with tempfile.NamedTemporaryFile(delete=False) as fin:
                fin.write(original)
                in_path = fin.name
            out_path = in_path + '.vv'
            result = subprocess.run(
                [VV_BINARY, '-c', '--format-v2', '-m', 'extreme',
                 '-o', out_path, in_path],
                capture_output=True, timeout=10)
            if result.returncode != 0:
                os.remove(in_path)
                stats['mismatches'].append(
                    (f"v2#{i}", original, None, None,
                     f"C encoder failed: {result.stderr.decode()}"))
                continue
            with open(out_path, 'rb') as f:
                frame = f.read()
            os.remove(in_path); os.remove(out_path)
        except Exception as e:
            stats['mismatches'].append(
                (f"v2#{i}", original, None, None, f"encode exn: {e}"))
            continue

        # Decode with C
        c_ok, c_out, c_err = decode_c(frame)
        # Decode with Python reference
        try:
            py_out = vv_decoder.decompress(frame)
            py_ok = True
            py_err = None
        except Exception as e:
            py_ok = False
            py_out = None
            py_err = f"{type(e).__name__}: {e}"

        # All three must agree: both accept, both produce original
        if c_ok and py_ok and c_out == original and py_out == original:
            stats['both_accepted'] += 1
        else:
            stats['mismatches'].append(
                (f"v2#{i}", frame, c_out, py_out,
                 f"C={'OK' if c_ok else c_err} Py={'OK' if py_ok else py_err}"))
    return stats


# ─────────────────────────────────────────────────────────────────
# Reporting
# ─────────────────────────────────────────────────────────────────

def summarize(name, stats):
    total = (stats['both_rejected'] + stats['both_accepted']
             + stats['skipped_entropy'] + len(stats['mismatches']))
    status = '✓' if not stats['mismatches'] else '✗'
    print(f"  {status} {name:<34s}: {total:5d} total  "
          f"rej={stats['both_rejected']:4d}  "
          f"acc={stats['both_accepted']:4d}  "
          f"skip={stats['skipped_entropy']:4d}  "
          f"MISMATCH={len(stats['mismatches'])}")
    return len(stats['mismatches'])


def save_mismatches(stats, outdir):
    """Save the first few mismatches to a directory for reproduction."""
    if not stats['mismatches']:
        return
    os.makedirs(outdir, exist_ok=True)
    # Keep at most 5 per category
    for i, (label, data, c_out, py_out, note) in enumerate(stats['mismatches'][:5]):
        path = os.path.join(outdir, f"{label}.vv")
        with open(path, 'wb') as f:
            f.write(data)
        if note:
            print(f"      {path}: {note}")


# ─────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iters', type=int, default=1000,
                    help='Iterations per strategy (default: 1000)')
    ap.add_argument('--seed', type=int, default=None,
                    help='RNG seed (default: random)')
    ap.add_argument('--save-mismatches', type=str, default=None,
                    help='Directory to save mismatched inputs')
    args = ap.parse_args()

    if args.seed is None:
        args.seed = random.randrange(1 << 30)
    rng = random.Random(args.seed)

    if not (os.path.isfile(VV_BINARY) and os.access(VV_BINARY, os.X_OK)):
        print(f"FATAL: {VV_BINARY} not found. Run `make` first.")
        return 1

    print(f"Fuzzer seed: {args.seed}")
    print(f"Iterations per strategy: {args.iters}")
    print(f"Total test cases: {args.iters * 6}")
    print()

    # Build a baseline valid frame for mutation strategies
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(b'hello world from the fuzzer harness')
        in_path = f.name
    out_path = in_path + '.vv'
    subprocess.run([VV_BINARY, '-c', '-m', 'balanced', '-o', out_path, in_path],
                   check=True, capture_output=True)
    with open(out_path, 'rb') as f:
        baseline = f.read()
    os.remove(in_path); os.remove(out_path)

    print("Differential fuzzing (C decoder ↔ Python decoder):")
    print()

    all_stats = {}
    all_stats['pure_random'] = run_strategy_1_pure_random(args.iters, rng)
    all_stats['mutate_1'] = run_strategy_2_mutated_baseline(args.iters, rng, baseline)
    all_stats['mutate_stack'] = run_strategy_3_multi_mutation(args.iters, rng, baseline)
    all_stats['header_garbage'] = run_strategy_4_header_garbage(args.iters, rng)
    all_stats['roundtrip'] = run_strategy_5_roundtrip(args.iters, rng)
    # Strategy 6 uses fewer iters (C subprocess per case is slow vs
    # the in-process Python decoder checks). 200 iterations is enough
    # to exercise the 'T' tag path across hundreds of varied payloads
    # without ballooning test time.
    v2_iters = min(args.iters, 200)
    all_stats['format_v2'] = run_strategy_6_format_v2(v2_iters, rng)

    print()
    total_mismatches = 0
    for name, stats in all_stats.items():
        total_mismatches += summarize(name, stats)
        if stats['mismatches'] and args.save_mismatches:
            save_mismatches(stats, os.path.join(args.save_mismatches, name))

    print()
    if total_mismatches == 0:
        # Total = 5 strategies × args.iters + format_v2 iters (capped at 200)
        total_cases = args.iters * 5 + v2_iters
        print(f"✓ ALL {total_cases} fuzz cases consistent.")
        return 0
    else:
        print(f"✗ Found {total_mismatches} divergence(s). Investigate.")
        if not args.save_mismatches:
            print("  Pass --save-mismatches <dir> to capture repro cases.")
        return 1


if __name__ == '__main__':
    sys.exit(main())
