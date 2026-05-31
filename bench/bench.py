#!/usr/bin/env python3
"""
bench/bench.py — reproducible speed and ratio measurement for VaptVupt.

Measures vv and zstd at multiple levels against the Silesia corpus
fixtures (dickens, xml, sao, x-ray). Reports best-of-3 encode and
decode times, MB/s throughput, and compression ratio.

Usage:
    cd /path/to/vaptvupt
    make
    python3 bench/bench.py

Requires:
    - ./vaptvupt built and in PATH or repo root
    - zstd installed and in PATH
    - Silesia corpus extracted to /tmp/silesia/ (configurable via env)

Output: a per-fixture ratio/throughput table.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

DEFAULT_SILESIA = "/tmp/silesia"
DEFAULT_FIXTURES = ["dickens", "xml", "sao", "x-ray"]


def find_vv():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(here, "vaptvupt"),
        shutil.which("vaptvupt"),
    ]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    sys.exit("error: vaptvupt binary not found. Run 'make' first.")


def measure(cmd, out_path, runs=3):
    """Run cmd `runs` times. Returns (best_time_s, output_size_bytes)."""
    sizes = []
    times = []
    for _ in range(runs):
        if os.path.exists(out_path):
            os.remove(out_path)
        t0 = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True)
        t1 = time.perf_counter()
        if r.returncode != 0:
            return None, None
        if not os.path.exists(out_path):
            return None, None
        sizes.append(os.path.getsize(out_path))
        times.append(t1 - t0)
    if len(set(sizes)) != 1:
        print(f"WARNING: non-deterministic output sizes for {cmd}: {sizes}",
              file=sys.stderr)
    return min(times), sizes[0]


def fmt_row(fixture, tool, enc_ms, enc_mbs, dec_ms, dec_mbs, ratio):
    return (f"{fixture:<10} {tool:<14} {enc_ms:>8.1f} {enc_mbs:>10.1f} "
            f"{dec_ms:>8.1f} {dec_mbs:>10.1f} {ratio:>7.2f}")


def bench_one(vv, fixture, raw_size, fixture_path, runs):
    rows = []
    with tempfile.TemporaryDirectory() as td:
        for mode in ["fast", "balanced", "extreme"]:
            cf = os.path.join(td, f"{fixture}.vv")
            df = os.path.join(td, f"{fixture}.out")
            t, sz = measure([vv, "-c", "-m", mode, "-o", cf, fixture_path],
                            cf, runs)
            if t is None:
                continue
            enc_mbs = (raw_size / t) / 1e6
            td_, _ = measure([vv, "-d", "-o", df, cf], df, runs)
            dec_mbs = (raw_size / td_) / 1e6 if td_ else 0.0
            rows.append(fmt_row(fixture, f"vv -{mode}",
                                t * 1000, enc_mbs,
                                td_ * 1000 if td_ else 0, dec_mbs,
                                raw_size / sz))

        for lvl in [1, 3, 9]:
            cf = os.path.join(td, f"{fixture}.zst")
            df = os.path.join(td, f"{fixture}.zout")
            t, sz = measure(["zstd", f"-{lvl}", "-q", "-f",
                             "-o", cf, fixture_path], cf, runs)
            if t is None:
                continue
            enc_mbs = (raw_size / t) / 1e6
            td_, _ = measure(["zstd", "-d", "-q", "-f", "-o", df, cf],
                             df, runs)
            dec_mbs = (raw_size / td_) / 1e6 if td_ else 0.0
            rows.append(fmt_row(fixture, f"zstd -{lvl}",
                                t * 1000, enc_mbs,
                                td_ * 1000 if td_ else 0, dec_mbs,
                                raw_size / sz))

    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--silesia", default=os.environ.get("SILESIA",
                                                        DEFAULT_SILESIA))
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--fixtures", nargs="+", default=DEFAULT_FIXTURES)
    args = ap.parse_args()

    if not shutil.which("zstd"):
        sys.exit("error: zstd not in PATH (apt install zstd)")

    vv = find_vv()
    print(f"vv: {vv}")
    print(f"silesia: {args.silesia}")
    print(f"runs: {args.runs} (best-of)")
    print()

    header = fmt_row("fixture", "tool", float('nan'), float('nan'),
                     float('nan'), float('nan'), float('nan'))
    # Pretty header (skip the actual NaN formatting)
    print(f"{'fixture':<10} {'tool':<14} {'enc_ms':>8} {'enc_MB/s':>10} "
          f"{'dec_ms':>8} {'dec_MB/s':>10} {'ratio':>7}")
    print("-" * 78)

    for f in args.fixtures:
        fp = os.path.join(args.silesia, f)
        if not os.path.exists(fp):
            print(f"# skipping {f} (not found at {fp})")
            continue
        raw_size = os.path.getsize(fp)
        for row in bench_one(vv, f, raw_size, fp, args.runs):
            print(row)
        print()


if __name__ == "__main__":
    main()
