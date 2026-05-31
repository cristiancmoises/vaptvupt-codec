#!/usr/bin/env python3
"""
VaptVupt — Decode-speed regression gate.

Complements `bench_gate.py` (which tracks ratio regressions). This
script measures decode throughput across a set of fixtures and fails
when any fixture's speed drops by more than `tolerance_pct` from the
committed baseline (`tests/speed_baseline.json`).

Speed is noisier than ratio — same code can vary 5-15% run-to-run
based on CPU caches, load, frequency scaling, etc. The default
tolerance is therefore 20% (one-sided): only regressions >20% fail
the gate.

The script:
  1. Compresses each fixture with `-m balanced` and stores the .vv.
  2. Runs decode N times per fixture (default: 15), records throughputs.
  3. Uses the MEDIAN throughput (robust to outliers) for comparison.
  4. Compares against baseline.

Measuring decode-speed accurately requires:
  - A sufficiently large input (≥ ~1MB for cache-hot timing)
  - Multiple runs to smooth variance
  - Not paging from disk during timing (input is in RAM already)

Usage:
    python3 tests/speed_gate.py                    # check against baseline
    python3 tests/speed_gate.py --update           # regen baseline
    python3 tests/speed_gate.py --iters 30         # more samples
    python3 tests/speed_gate.py --tolerance 15     # stricter (15%)
"""

import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
VV_ROOT = os.path.dirname(HERE)
VV_BINARY = os.path.join(VV_ROOT, 'vaptvupt')
BASELINE_PATH = os.path.join(HERE, 'speed_baseline.json')


# ─────────────────────────────────────────────────────────────────
# Fixtures — larger than bench_gate to get stable speed measurements.
# Reuse generators from bench_gate where possible.
# ─────────────────────────────────────────────────────────────────

def _seeded(seed):
    return random.Random(seed)


def gen_text_1mb():
    rng = _seeded(42)
    words = ('the quick brown fox jumps over the lazy dog lorem ipsum '
             'dolor sit amet consectetur adipiscing elit').split()
    return ' '.join(rng.choice(words) for _ in range(150000)).encode('utf-8')


def gen_json_1mb():
    rng = _seeded(7)
    items = [{'id': i, 'name': f'record_{i}',
              'value': rng.random(), 'tag': f'tag_{i % 50}',
              'active': rng.choice([True, False])}
             for i in range(15000)]
    return json.dumps(items).encode('utf-8')


def gen_repeating_1mb():
    return (b'the quick brown fox jumps over the lazy dog. ' * 24000)[:1024000]


def gen_binary_1mb():
    return bytes(((i * 17 + (i >> 3)) & 0xFF) for i in range(1024000))


def gen_random_1mb():
    rng = _seeded(1)
    return bytes(rng.randrange(256) for _ in range(1024000))


def gen_source_1mb():
    template = '''
static int func_{n}(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap, size_t *out_len) {{
    if (!src || !dst) return VV_ERR_PARAM;
    if (src_len > dst_cap) return VV_ERR_OVERFLOW;
    for (size_t i = 0; i < src_len; i++) dst[i] = src[i] ^ 0x{x:02X};
    *out_len = src_len;
    return VV_OK;
}}
'''
    out = ''
    i = 0
    while len(out) < 1024000:
        out += template.format(n=i, x=i & 0xFF)
        i += 1
    return out[:1024000].encode('utf-8')


FIXTURES = [
    ('text-1MB',      gen_text_1mb),
    ('json-1MB',      gen_json_1mb),
    ('repeating-1MB', gen_repeating_1mb),
    ('binary-1MB',    gen_binary_1mb),
    ('random-1MB',    gen_random_1mb),
    ('source-1MB',    gen_source_1mb),
]


# ─────────────────────────────────────────────────────────────────
# Measurement
# ─────────────────────────────────────────────────────────────────

def compress_to_vv(data):
    """Returns path to a temp .vv file. Caller must remove."""
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
        in_path = f.name
    out_path = in_path + '.vv'
    subprocess.run([VV_BINARY, '-c', '-m', 'balanced', '-o', out_path, in_path],
                   check=True, capture_output=True)
    os.remove(in_path)
    return out_path


def measure_decode_speed(vv_path, iters):
    """Run `vaptvupt -d` `iters` times, return list of MB/s throughputs.

    Parses the CLI's "(X.Y MB/s)" output on each run."""
    speeds = []
    dec_path = vv_path + '.dec'
    for _ in range(iters):
        result = subprocess.run(
            [VV_BINARY, '-d', '-o', dec_path, vv_path],
            capture_output=True, check=True)
        # CLI writes status to STDERR, not stdout.
        line = result.stderr.decode('utf-8', errors='replace').strip()
        # Format: "Decompressed X → Y bytes (Z.Z MB/s)"
        try:
            mbps = float(line.split('(')[1].split(' MB/s')[0])
            speeds.append(mbps)
        except (IndexError, ValueError):
            pass
    try:
        os.remove(dec_path)
    except OSError:
        pass
    return speeds


def measure_fixture(name, gen_fn, iters, warmup):
    """Compress, warm up, measure."""
    data = gen_fn()
    vv_path = compress_to_vv(data)
    try:
        compressed_size = os.path.getsize(vv_path)
        # Warmup (discard results)
        if warmup > 0:
            measure_decode_speed(vv_path, warmup)
        speeds = measure_decode_speed(vv_path, iters)
        if not speeds:
            return None
        return {
            'input_size': len(data),
            'compressed_size': compressed_size,
            'median_mbps': statistics.median(speeds),
            'min_mbps': min(speeds),
            'max_mbps': max(speeds),
            'stdev_mbps': statistics.stdev(speeds) if len(speeds) > 1 else 0.0,
            'samples': len(speeds),
        }
    finally:
        try:
            os.remove(vv_path)
        except OSError:
            pass


# ─────────────────────────────────────────────────────────────────
# Reporting
# ─────────────────────────────────────────────────────────────────

def print_results(current):
    print(f"\n  {'fixture':<15s} {'input':>8s} {'comp':>8s} "
          f"{'median':>8s} {'min':>8s} {'max':>8s} {'stdev':>8s}")
    print('  ' + '-' * 65)
    for name, rec in current.items():
        if rec is None:
            print(f"  {name:<15s}  (measurement failed)")
            continue
        print(f"  {name:<15s} {rec['input_size']:>8d} "
              f"{rec['compressed_size']:>8d} "
              f"{rec['median_mbps']:>8.1f} "
              f"{rec['min_mbps']:>8.1f} "
              f"{rec['max_mbps']:>8.1f} "
              f"{rec['stdev_mbps']:>8.2f}")


def check_regressions(current, baseline, tolerance_pct):
    """Return list of regression messages."""
    regressions = []
    for name, cur in current.items():
        if cur is None or name not in baseline:
            continue
        base = baseline[name]
        cur_median = cur['median_mbps']
        base_median = base['median_mbps']
        # Regression = current is slower than baseline by more than tolerance_pct.
        slowdown_pct = (base_median - cur_median) / base_median * 100
        if slowdown_pct > tolerance_pct:
            regressions.append(
                f"{name}: {cur_median:.1f} MB/s, baseline {base_median:.1f} MB/s "
                f"({slowdown_pct:.1f}% slower — tolerance {tolerance_pct}%)")
    return regressions


# ─────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--update', action='store_true',
                    help='Regenerate baseline from current results')
    ap.add_argument('--iters', type=int, default=15,
                    help='Number of decode samples per fixture (default: 15)')
    ap.add_argument('--warmup', type=int, default=3,
                    help='Discarded warmup iterations (default: 3)')
    ap.add_argument('--tolerance', type=float, default=20.0,
                    help='Allowed slowdown percentage (default: 20.0)')
    args = ap.parse_args()

    if not (os.path.isfile(VV_BINARY) and os.access(VV_BINARY, os.X_OK)):
        sys.exit(f"FATAL: {VV_BINARY} not found. Run `make` first.")

    print(f"Measuring decode speed on {len(FIXTURES)} fixtures "
          f"({args.iters} samples each + {args.warmup} warmup)...")
    t0 = time.time()
    current = {name: measure_fixture(name, fn, args.iters, args.warmup)
               for name, fn in FIXTURES}
    elapsed = time.time() - t0
    print_results(current)
    print(f"\n  Total measurement time: {elapsed:.1f}s")

    if args.update:
        with open(BASELINE_PATH, 'w') as f:
            json.dump(current, f, indent=2, sort_keys=True)
            f.write('\n')
        print(f"\n  Baseline updated → {BASELINE_PATH}")
        print(f"  NOTE: decode speed varies by hardware. Commit this")
        print(f"  only if running on the canonical benchmarking machine.")
        return 0

    if not os.path.isfile(BASELINE_PATH):
        print(f"\n  No baseline at {BASELINE_PATH}. Run with --update.")
        return 0

    with open(BASELINE_PATH) as f:
        baseline = json.load(f)

    regressions = check_regressions(current, baseline, args.tolerance)
    if regressions:
        print(f"\n  ✗ {len(regressions)} SPEED REGRESSION(S) "
              f"(tolerance {args.tolerance}%):")
        for r in regressions:
            print(f"    {r}")
        print("\n  Speed is inherently noisy — if you believe these are noise,")
        print("  rerun with --iters 30 to get more samples. To accept, use")
        print("  --update after verifying on the canonical machine.")
        return 1
    else:
        print(f"\n  ✓ Speed gate passed. All {len(current)} fixtures within "
              f"{args.tolerance}% of baseline.")
        return 0


if __name__ == '__main__':
    sys.exit(main())
