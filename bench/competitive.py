#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# competitive.py — VaptVupt competitive honesty harness (Lever L10).
#
# Measures VaptVupt against the standard compressors available on the
# system (gzip, zstd, lz4, xz) across a set of input files, and reports
# compression ratio and throughput. Generated-suite mode measures both
# compression and decompression; legacy file mode measures compression only.
# The point of this tool is HONESTY:
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
#   bench/competitive.py --generated-suite --vv ./vaptvupt --runs 5
#
# Legacy file mode remains a measurement tool, not a gate. Generated-suite
# mode fails on a missing required codec, command failure, or decode mismatch.
# (See tests/bench_gate.py for the compression-ratio regression gate.)

import argparse
import csv
import datetime
import hashlib
import json
import os
import platform
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time

# Each external codec: name -> (binary, compress-args building fn).
# Legacy file mode invokes "<bin> <args> -c FILE" and counts stdout bytes.
# Its decompression timing remains out of scope; generated-suite mode below
# performs and verifies timed roundtrips for every codec.
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

GENERATED_SUITE_VERSION = "generated-v1"
GENERATED_CODEC_NAMES = [
    "vv-fast", "vv-balanced", "lz4-1", "zstd-1", "zstd-3",
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


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _generated_fixtures():
    """Return deterministic, dependency-free fixtures for comparison runs.

    The algorithms and sizes are deliberately defined here rather than by
    Python's random module, whose higher-level sampling details can vary by
    runtime version. Changing any generator requires a suite-version bump.
    """
    target = 1024 * 1024

    words = (
        "the quick brown fox jumps over lazy dog compression storage packet "
        "record client server request response checksum window literal match "
        "stream buffer encoder decoder balanced fast durable archive"
    ).split()
    state = 0x6D2B79F5
    text_parts = []
    text_len = 0
    while text_len < target + 32:
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        word = words[state % len(words)]
        text_parts.append(word)
        text_len += len(word) + 1
    text_data = (" ".join(text_parts) + "\n").encode("ascii")[:target]

    state = 0x243F6A88
    json_lines = []
    json_len = 0
    i = 0
    while json_len < target:
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        line = (
            f'{{"id":{i},"sensor":"s{i % 97:02d}",'
            f'"value":{state % 100000},"status":"{("ok", "warn", "idle")[i % 3]}"}}\n'
        )
        json_lines.append(line)
        json_len += len(line)
        i += 1
    json_data = "".join(json_lines).encode("ascii")

    records = bytearray()
    state = 0x13198A2E
    for i in range(50000):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        label = f"dev{i % 4096:08x}".encode("ascii")
        records += struct.pack("<Iii12s", i, (state % 18000) - 4000,
                               state & 0x7FFFFFFF, label)

    randomish = bytearray()
    state64 = 0x9E3779B97F4A7C15
    mask64 = (1 << 64) - 1
    while len(randomish) < target:
        state64 ^= state64 >> 12
        state64 ^= (state64 << 25) & mask64
        state64 ^= state64 >> 27
        value = (state64 * 2685821657736338717) & mask64
        randomish += struct.pack("<Q", value)

    return [
        ("text.txt", text_data),
        ("records.jsonl", json_data),
        ("records.bin", bytes(records)),
        ("random.bin", bytes(randomish[:target])),
    ]


def _first_line(cmd):
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    return next((line.strip() for line in p.stdout.splitlines() if line.strip()),
                f"exit {p.returncode}")


def _git_metadata(root):
    def git(*args):
        try:
            p = subprocess.run(["git", "-C", root, *args],
                               stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True,
                               timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return p.stdout.strip() if p.returncode == 0 else None

    commit = git("rev-parse", "HEAD")
    status = git("status", "--porcelain", "--untracked-files=no")
    return {
        "commit": commit,
        "tracked_dirty": None if status is None else bool(status),
    }


def _cpu_model():
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8") as fh:
            for line in fh:
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _generated_codecs(vv):
    vv_path = os.path.abspath(vv)
    zstd = shutil.which("zstd")
    lz4 = shutil.which("lz4")
    codecs = [
        {
            "name": "vv-fast", "binary": vv_path, "suffix": ".vv",
            "options": "-m fast",
            "compress": lambda src, dst: [vv_path, "-c", "-m", "fast",
                                                  "-o", dst, src],
            "decompress": lambda src, dst: [vv_path, "-d", "-o", dst, src],
            "version_args": ["-h"],
        },
        {
            "name": "vv-balanced", "binary": vv_path, "suffix": ".vv",
            "options": "-m balanced",
            "compress": lambda src, dst: [vv_path, "-c", "-m", "balanced",
                                                  "-o", dst, src],
            "decompress": lambda src, dst: [vv_path, "-d", "-o", dst, src],
            "version_args": ["-h"],
        },
        {
            "name": "lz4-1", "binary": lz4, "suffix": ".lz4",
            "options": "-1 -T1",
            "compress": lambda src, dst: [lz4, "-1", "-T1", "-q", "-f",
                                                src, dst],
            "decompress": lambda src, dst: [lz4, "-d", "-q", "-f", src, dst],
            "version_args": ["--version"],
        },
        {
            "name": "zstd-1", "binary": zstd, "suffix": ".zst",
            "options": "-1 --single-thread",
            "compress": lambda src, dst: [zstd, "-1", "--single-thread",
                                             "-q", "-f", "-o", dst, src],
            "decompress": lambda src, dst: [zstd, "-d", "-q", "-f",
                                               "-o", dst, src],
            "version_args": ["--version"],
        },
        {
            "name": "zstd-3", "binary": zstd, "suffix": ".zst",
            "options": "-3 --single-thread",
            "compress": lambda src, dst: [zstd, "-3", "--single-thread",
                                             "-q", "-f", "-o", dst, src],
            "decompress": lambda src, dst: [zstd, "-d", "-q", "-f",
                                               "-o", dst, src],
            "version_args": ["--version"],
        },
    ]
    for codec in codecs:
        binary = codec["binary"]
        codec["available"] = bool(binary and os.path.isfile(binary)
                                  and os.access(binary, os.X_OK))
    return codecs


def _run_output(cmd, out_path, timeout):
    try:
        os.unlink(out_path)
    except FileNotFoundError:
        pass
    t0 = time.perf_counter()
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"command failed: {' '.join(cmd)}: {exc}") from exc
    elapsed = time.perf_counter() - t0
    if p.returncode != 0:
        detail = p.stderr.decode("utf-8", "replace").strip()[:300]
        raise RuntimeError(
            f"command exited {p.returncode}: {' '.join(cmd)}: {detail}")
    if not os.path.isfile(out_path):
        raise RuntimeError(f"command produced no output: {' '.join(cmd)}")
    return elapsed, os.path.getsize(out_path)


def _benchmark_generated_codec(codec, fixture_path, raw_size, raw_sha,
                               work_dir, runs, warmups, timeout):
    compressed = os.path.join(work_dir, codec["name"] + codec["suffix"])
    decoded = os.path.join(work_dir, codec["name"] + ".decoded")
    compress_cmd = codec["compress"](fixture_path, compressed)
    decompress_cmd = codec["decompress"](compressed, decoded)

    for _ in range(warmups):
        _run_output(compress_cmd, compressed, timeout)
        _run_output(decompress_cmd, decoded, timeout)
        if _sha256_file(decoded) != raw_sha:
            raise RuntimeError(f"{codec['name']} warm-up decode mismatch")

    encode_times = []
    sizes = []
    for _ in range(runs):
        elapsed, size = _run_output(compress_cmd, compressed, timeout)
        encode_times.append(elapsed)
        sizes.append(size)
    if len(set(sizes)) != 1:
        raise RuntimeError(f"{codec['name']} output sizes differ: {sizes}")

    decode_times = []
    for _ in range(runs):
        elapsed, _ = _run_output(decompress_cmd, decoded, timeout)
        if _sha256_file(decoded) != raw_sha:
            raise RuntimeError(f"{codec['name']} decode mismatch")
        decode_times.append(elapsed)

    enc_s = statistics.median(encode_times)
    dec_s = statistics.median(decode_times)
    return {
        "codec": codec["name"],
        "compressed_bytes": sizes[0],
        "ratio": raw_size / sizes[0],
        "encode_seconds_median": enc_s,
        "encode_mbps": raw_size / enc_s / 1e6,
        "decode_seconds_median": dec_s,
        "decode_mbps": raw_size / dec_s / 1e6,
        "runs": runs,
        "verified": True,
    }


def _write_generated_csv(path, fixture_meta, results):
    meta_by_name = {f["name"]: f for f in fixture_meta}
    with open(path, "w", newline="", encoding="utf-8") as fh:
        out = csv.writer(fh)
        out.writerow([
            "file", "raw_bytes", "raw_sha256", "codec", "comp_bytes",
            "ratio", "encode_seconds_median", "encode_MBps",
            "decode_seconds_median", "decode_MBps", "runs", "verified",
        ])
        for row in results:
            fixture = meta_by_name[row["file"]]
            out.writerow([
                row["file"], fixture["bytes"], fixture["sha256"],
                row["codec"], row["compressed_bytes"],
                f"{row['ratio']:.6f}",
                f"{row['encode_seconds_median']:.9f}",
                f"{row['encode_mbps']:.3f}",
                f"{row['decode_seconds_median']:.9f}",
                f"{row['decode_mbps']:.3f}", row["runs"],
                "true" if row["verified"] else "false",
            ])


def generated_suite(args):
    if args.runs < 1:
        raise ValueError("--runs must be at least 1")
    if args.warmups < 0:
        raise ValueError("--warmups must be non-negative")

    codecs = _generated_codecs(args.vv)
    missing = [c["name"] for c in codecs if not c["available"]]
    # vv-fast and vv-balanced share one binary; make that error concise.
    if not codecs[0]["available"]:
        raise RuntimeError(f"vaptvupt binary not found or not executable: {args.vv}")
    if missing and not args.allow_missing:
        raise RuntimeError(
            "required generated-suite codecs unavailable: "
            + ", ".join(missing)
            + " (install them, or use --allow-missing for a partial diagnostic run)")
    active = [c for c in codecs if c["available"]]

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    available_gcc = shutil.which("gcc")
    metadata = {
        "suite": GENERATED_SUITE_VERSION,
        "generated_at_utc": datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "timing": "median subprocess wall-clock",
        "throughput_unit": "MB/s (raw bytes / 1e6 / seconds)",
        "runs": args.runs,
        "warmups": args.warmups,
        "timeout_seconds": args.timeout,
        "complete_matrix": not missing,
        "missing_codecs": missing,
        "harness_sha256": _sha256_file(os.path.abspath(__file__)),
        "host": {
            "platform": platform.platform(),
            "cpu": _cpu_model(),
            "logical_cpus": os.cpu_count(),
            "affinity": (sorted(os.sched_getaffinity(0))
                         if hasattr(os, "sched_getaffinity") else None),
            "python": platform.python_version(),
            "available_gcc": (_first_line([available_gcc, "--version"])
                              if available_gcc else None),
        },
        "git": _git_metadata(root),
        "tools": {},
    }
    for codec in codecs:
        if codec["available"]:
            binary = codec["binary"]
            metadata["tools"][codec["name"]] = {
                "path": binary,
                "sha256": _sha256_file(binary),
                "version": _first_line([binary, *codec["version_args"]]),
                "options": codec["options"],
            }
        else:
            metadata["tools"][codec["name"]] = {
                "available": False, "options": codec["options"],
            }

    fixture_meta = []
    results = []
    with tempfile.TemporaryDirectory(prefix="vv-generated-suite-") as td:
        fixture_dir = os.path.join(td, "fixtures")
        result_dir = os.path.join(td, "results")
        os.mkdir(fixture_dir)
        os.mkdir(result_dir)
        for name, data in _generated_fixtures():
            path = os.path.join(fixture_dir, name)
            with open(path, "wb") as fh:
                fh.write(data)
            raw_sha = _sha256_file(path)
            fixture_meta.append({
                "name": name, "bytes": len(data), "sha256": raw_sha,
            })
            fixture_work = os.path.join(result_dir, name)
            os.mkdir(fixture_work)
            for codec in active:
                row = _benchmark_generated_codec(
                    codec, path, len(data), raw_sha, fixture_work,
                    args.runs, args.warmups, args.timeout)
                row["file"] = name
                results.append(row)

    payload = {
        "metadata": metadata,
        "fixtures": fixture_meta,
        "results": results,
    }
    if args.csv:
        _write_generated_csv(args.csv, fixture_meta, results)
    if args.json_path:
        with open(args.json_path, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2, sort_keys=True)
            fh.write("\n")

    print("# VaptVupt deterministic generated-suite benchmark")
    print("# metadata: " + json.dumps(metadata, sort_keys=True))
    for fixture in fixture_meta:
        print(f"# fixture: {fixture['name']} bytes={fixture['bytes']} "
              f"sha256={fixture['sha256']}")
    if missing:
        print("# INCOMPLETE: unavailable codecs: " + ", ".join(missing))
    print("# cells: ratio @ encode/decode MB/s; median of "
          f"{args.runs} measured run(s), {args.warmups} warm-up(s)")
    print("| file | " + " | ".join(GENERATED_CODEC_NAMES) + " |")
    print("|---|" + "---|" * len(GENERATED_CODEC_NAMES))
    by_key = {(r["file"], r["codec"]): r for r in results}
    for fixture in fixture_meta:
        cells = []
        for codec_name in GENERATED_CODEC_NAMES:
            row = by_key.get((fixture["name"], codec_name))
            if row is None:
                cells.append("—")
            else:
                cells.append(
                    f"{row['ratio']:.3f} @ "
                    f"{row['encode_mbps']:.1f}/{row['decode_mbps']:.1f}")
        print(f"| {fixture['name']} | " + " | ".join(cells) + " |")
    if args.csv:
        print(f"# wrote CSV: {args.csv}", file=sys.stderr)
    if args.json_path:
        print(f"# wrote JSON: {args.json_path}", file=sys.stderr)
    return 0


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
    ap.add_argument("--generated-suite", action="store_true",
                    help="benchmark deterministic generated-v1 fixtures with "
                         "vv fast/balanced, lz4-1, and zstd-1/3; verify decode")
    ap.add_argument("--runs", type=int, default=3,
                    help="measured runs per generated-suite cell (median; default 3)")
    ap.add_argument("--warmups", type=int, default=1,
                    help="warm-up roundtrips per generated-suite cell (default 1)")
    ap.add_argument("--json", dest="json_path",
                    help="write generated-suite metadata and results as JSON")
    ap.add_argument("--allow-missing", action="store_true",
                    help="allow an explicitly marked partial generated-suite run")
    ap.add_argument("--self-test", action="store_true",
                    help="run a corpus-free smoke test (for make test) and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test(args.vv)
    if args.generated_suite:
        if args.files or args.dir:
            ap.error("--generated-suite does not accept FILE or --dir")
        try:
            return generated_suite(args)
        except (RuntimeError, ValueError) as exc:
            print(f"generated-suite FAIL: {exc}", file=sys.stderr)
            return 1
    if args.json_path or args.allow_missing:
        ap.error("--json and --allow-missing require --generated-suite")

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
