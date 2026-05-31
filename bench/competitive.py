#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# competitive.py — VaptVupt competitive honesty harness (Lever L10).
#
# Measures VaptVupt against the standard compressors available on the
# system (gzip, zstd, lz4, xz) across a set of input files, and reports
# compression ratio and throughput. The point of this tool is HONESTY:
# it prints every result, wins and losses alike, with measured numbers —
# never marketing. Where VaptVupt loses, the table shows it losing.
#
# It does NOT pick winners for you or hide anything. It runs each codec
# the same way on the same bytes and prints a column per codec plus a
# "best" column so the competitive position is unambiguous.
#
# Usage:
#   bench/competitive.py FILE [FILE ...]
#   bench/competitive.py --vv ./vaptvupt --dir /path/to/corpus
#   bench/competitive.py --modes balanced,extreme --csv results.csv FILE...
#
# Exit status is always 0 on a completed run; this is a measurement tool,
# not a gate. (See tests/bench_gate.py for the regression gate.)

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

# Each external codec: name -> (binary, compress-args building fn).
# We invoke "<bin> <args> -c FILE > /dev/null" and count stdout bytes,
# timing the compress. Decompress timing is intentionally out of scope
# here (ratio + compress speed are the headline numbers); a separate
# decode benchmark lives in the C test suite.
def _gzip(level):   return ["gzip", f"-{level}", "-c"]
def _zstd(level):   return ["zstd", f"-{level}", "-c"]
def _lz4(level):    return ["lz4", f"-{level}", "-c"]
def _xz(level):     return ["xz", f"-{level}", "-c"]

EXTERNAL = [
    ("gzip-9",   _gzip(9)),
    ("lz4-9",    _lz4(9)),
    ("zstd-3",   _zstd(3)),
    ("zstd-19",  _zstd(19)),
    ("xz-9",     _xz(9)),
]


def have(binary):
    return shutil.which(binary) is not None


def run_external(args, data, timeout):
    """Return (out_bytes, seconds) or (None, None) on failure/timeout."""
    try:
        t0 = time.perf_counter()
        p = subprocess.run(args, input=data, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=timeout)
        dt = time.perf_counter() - t0
        if p.returncode != 0:
            return None, None
        return len(p.stdout), dt
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return None, None


def run_vv(vv, mode, path, timeout, extra=None):
    """Compress a file via the VaptVupt CLI; return (out_bytes, seconds).
    Uses a temp output file (the CLI writes to a path, not stdout)."""
    fd, outp = tempfile.mkstemp(suffix=".vv")
    os.close(fd)
    try:
        args = [vv, "-c", "-m", mode]
        if extra:
            args += extra
        args += ["-o", outp, path]
        t0 = time.perf_counter()
        p = subprocess.run(args, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=timeout)
        dt = time.perf_counter() - t0
        if p.returncode != 0:
            return None, None
        return os.path.getsize(outp), dt
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return None, None
    finally:
        try:
            os.unlink(outp)
        except OSError:
            pass


def collect_files(args):
    files = list(args.files)
    if args.dir:
        for name in sorted(os.listdir(args.dir)):
            path = os.path.join(args.dir, name)
            if os.path.isfile(path):
                files.append(path)
    return files


def self_test(vv):
    """Smoke test: synthesize a compressible buffer, run the harness logic
    on it, and assert VaptVupt produced a smaller-than-raw output and the
    ratio computation is sane. Returns 0 on success, 1 on failure. Used by
    `make test` — it must not depend on an external corpus."""
    if not os.path.exists(vv):
        print(f"  competitive self-test SKIP: {vv} not found")
        return 0
    data = (b"the quick brown fox jumps over the lazy dog. " * 4096)
    fd, inp = tempfile.mkstemp(suffix=".bin"); os.close(fd)
    try:
        open(inp, "wb").write(data)
        ok = True
        for mode in ("fast", "balanced", "extreme"):
            b, s = run_vv(vv, mode, inp, 30.0)
            if b is None or b >= len(data):
                print(f"  competitive self-test FAIL: mode={mode} b={b} raw={len(data)}")
                ok = False
            else:
                ratio = len(data) / b
                if ratio < 1.0:
                    print(f"  competitive self-test FAIL: mode={mode} ratio={ratio:.2f} < 1")
                    ok = False
        if ok:
            print("  competitive self-test PASS (fast/balanced/extreme all compress)")
        return 0 if ok else 1
    finally:
        try:
            os.unlink(inp)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description="VaptVupt competitive honesty harness")
    ap.add_argument("files", nargs="*", help="input files to benchmark")
    ap.add_argument("--vv", default="./vaptvupt", help="path to vaptvupt binary")
    ap.add_argument("--dir", help="also benchmark every file in this directory")
    ap.add_argument("--modes", default="balanced,extreme",
                    help="comma-separated VaptVupt modes (fast,balanced,extreme)")
    ap.add_argument("--v2", action="store_true",
                    help="also measure VaptVupt --format-v2 (min_match=3) per mode")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-codec-per-file timeout in seconds")
    ap.add_argument("--csv", help="write machine-readable results to this CSV")
    ap.add_argument("--self-test", action="store_true",
                    help="run a corpus-free smoke test (for make test) and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test(args.vv)

    files = collect_files(args)
    if not files:
        ap.error("no input files (give FILE... or --dir)")
    if not os.path.exists(args.vv):
        ap.error(f"vaptvupt binary not found: {args.vv}")

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    ext = [(n, a) for (n, a) in EXTERNAL if have(a[0])]
    missing = [n for (n, a) in EXTERNAL if not have(a[0])]
    if missing:
        print(f"# note: skipping unavailable codecs: {', '.join(missing)}", file=sys.stderr)

    # Column order: externals, then vv modes (and optional v2 variants).
    vv_cols = []
    for m in modes:
        vv_cols.append((f"vv-{m}", m, None))
        if args.v2:
            vv_cols.append((f"vv-{m}-v2", m, ["--format-v2"]))
    col_names = [n for (n, _) in ext] + [n for (n, _, _) in vv_cols]

    csv_rows = []
    print(f"# VaptVupt competitive benchmark — ratios (higher = better), raw/compressed")
    print(f"# binary: {args.vv}")
    header = f"{'file':<14}{'raw':>11}  " + "  ".join(f"{c:>10}" for c in col_names) + f"  {'best':>10}"
    print(header)
    print("-" * len(header))

    for path in files:
        try:
            data = open(path, "rb").read()
        except OSError:
            continue
        raw = len(data)
        if raw == 0:
            continue
        name = os.path.basename(path)
        results = {}  # col -> (bytes, sec)

        for cname, cargs in ext:
            results[cname] = run_external(cargs, data, args.timeout)
        for cname, mode, extra in vv_cols:
            results[cname] = run_vv(args.vv, mode, path, args.timeout, extra)

        # Build the ratio cells; track best (smallest compressed size).
        best_bytes = None
        best_col = None
        cells = []
        for c in col_names:
            b, _ = results.get(c, (None, None))
            if b is None:
                cells.append(f"{'—':>10}")
            else:
                cells.append(f"{raw / b:>10.3f}")
                if best_bytes is None or b < best_bytes:
                    best_bytes, best_col = b, c
        best_str = f"{best_col}" if best_col else "—"
        print(f"{name:<14}{raw:>11}  " + "  ".join(cells) + f"  {best_str:>10}")

        for c in col_names:
            b, s = results.get(c, (None, None))
            csv_rows.append((name, raw, c, b if b is not None else "",
                             f"{raw/b:.4f}" if b else "",
                             f"{s:.4f}" if s else "",
                             f"{raw/s/1e6:.2f}" if (s and s > 0) else ""))

    if args.csv:
        with open(args.csv, "w") as fh:
            fh.write("file,raw_bytes,codec,comp_bytes,ratio,seconds,MBps\n")
            for r in csv_rows:
                fh.write(",".join(str(x) for x in r) + "\n")
        print(f"\n# wrote {args.csv}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
