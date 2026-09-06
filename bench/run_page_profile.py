#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build one job at a time, retain evidence, and summarize page profiles."""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = ["vv_encoder", "vv_decoder", "vv_simd", "vv_xxh64", "vv_huffman",
        "vv_ans", "vv_bcj", "vaptvupt_api"]


def bounded(low, high):
    def parse(value):
        if not value.isascii() or not value.isdecimal() or not low <= int(value) <= high:
            raise argparse.ArgumentTypeError(f"expected an integer in {low}..{high}")
        return int(value)
    return parse


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def percentile(values, quantile):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * quantile) - 1)]


def summarize(raw, destination):
    groups = {}
    metadata = []
    with raw.open() as stream:
        def lines():
            for line in stream:
                if line.startswith("#"):
                    metadata.append(line.rstrip())
                else:
                    yield line
        for row in csv.DictReader(lines()):
            key = (row["fixture"], int(row["input_bytes"]), row["codec"])
            group = groups.setdefault(key, {"pages": {}, "phases": {}, "status": set()})
            group["status"].add(row["status"])
            phase = row["phase"]
            if phase == "size":
                page = int(row["page_index"])
                if page in group["pages"]:
                    raise ValueError(f"duplicate page in {key}: {page}")
                group["pages"][page] = {
                    "input_fnv1a64": row["input_fnv1a64"],
                    "compressed_bytes": int(row["compressed_bytes"]),
                    "framing_bytes": int(row["framing_bytes"]),
                    "payload_bytes": int(row["payload_bytes"]),
                    "not_smaller_than_input": bool(int(row["not_smaller_than_input"])),
                }
            else:
                phase_data = group["phases"].setdefault(phase, {"ns": [], "state_bytes": [], "status": set()})
                phase_data["status"].add(row["status"])
                if row["status"] == "PASS":
                    phase_data["ns"].append(float(row["ns_per_call"]))
                    phase_data["state_bytes"].append(int(row["state_bytes"]))
    results = []
    for (fixture, size, codec), group in sorted(groups.items()):
        pages = list(group["pages"].values())
        compressed = sum(p["compressed_bytes"] for p in pages)
        item = {
            "fixture": fixture, "input_bytes": size, "codec": codec,
            "status": sorted(group["status"]), "page_count": len(pages),
            "consumer_bypassed_control": fixture in ("zero", "same-filled"),
            "ratio": size * len(pages) / compressed,
            "compressed_bytes_min": min(p["compressed_bytes"] for p in pages),
            "compressed_bytes_max": max(p["compressed_bytes"] for p in pages),
            "compressed_bytes_mean": compressed / len(pages),
            "framing_bytes_mean": sum(p["framing_bytes"] for p in pages) / len(pages),
            "payload_bytes_mean": sum(p["payload_bytes"] for p in pages) / len(pages),
            "not_smaller_than_input_page_rate": sum(p["not_smaller_than_input"] for p in pages) / len(pages),
            "phases": {},
        }
        for phase, data in sorted(group["phases"].items()):
            values = data["ns"]
            report = {"status": sorted(data["status"]), "samples": len(values)}
            if values:
                report["reported_state_bytes_min"] = min(data["state_bytes"])
                report["reported_state_bytes_max"] = max(data["state_bytes"])
                if phase.endswith("_batch"):
                    median = percentile(values, .5)
                    report["median_amortized_ns_per_call"] = median
                    report["median_MBps"] = size * 1000 / median
                else:
                    report.update({"p50_ns": percentile(values, .5),
                                   "p95_ns": percentile(values, .95),
                                   "p99_ns": percentile(values, .99)})
            item["phases"][phase] = report
        results.append(item)
    report = {
        "status": "PASS" if results else "FAIL",
        "kernel_runtime": "NOT_RUN",
        "lzo_rle": {"status": "NOT_RUN", "reason": "No suitable userspace LZO-RLE implementation; ordinary LZO is not substituted."},
        "metadata": metadata,
        "percentiles": "Nearest rank of individual observed calls, clock/dispatch overhead included; batching is separate.",
        "limits": ["Synthetic warmed page cohorts, not private captures or kernel workloads.",
                   "Zero and same-filled controls are consumer-bypassed; do not count them as consumer codec wins.",
                   "Context setup excludes first-use allocations; first_encode_call and first_decode_call report those separately.",
                   "Reported state sizes are retained context sizes, not allocator overhead, RSS, peak memory, or hidden one-shot workspace.",
                   "A not-smaller page rate describes this generated cohort, not a production workload.",
                   "Framing subtraction is size accounting; no raw-VV/Zstd performance is inferred."],
        "results": results,
    }
    destination.write_text(json.dumps(report, indent=2) + "\n")
    return len(results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="Evidence parent directory; each invocation makes a new child.")
    parser.add_argument("--run", action="store_true", help="Collect timings; otherwise only build and self-test.")
    parser.add_argument("--cpu", type=bounded(0, 65535), help="Pin the benchmark to this available logical CPU.")
    parser.add_argument("--size", type=bounded(4096, 65536), choices=(4096, 16384, 65536))
    parser.add_argument("--pages", type=bounded(1, 256), default=64)
    parser.add_argument("--samples", type=bounded(101, 10001), default=101)
    parser.add_argument("--batch-samples", type=bounded(3, 31), default=7)
    parser.add_argument("--min-batch-ms", type=bounded(1, 100), default=10)
    parser.add_argument("--fixture", choices=("text", "records", "random", "repeating", "zero", "same-filled"))
    parser.add_argument("--codec", choices=("vv-oneshot-c0", "vv-oneshot-c1", "vv-context-c0", "vv-context-c1",
                                          "lz4-default", "lz4-extstate", "zstd-oneshot-1-c0", "zstd-oneshot-3-c0",
                                          "zstd-context-1-c0", "zstd-context-1-c1", "zstd-context-3-c0", "zstd-context-3-c1"))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    if args.cpu is not None and hasattr(os, "sched_getaffinity") and args.cpu not in os.sched_getaffinity(0):
        parser.error("--cpu is outside this process's allowed affinity")
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents:
        parser.error("keep generated evidence outside the source worktree")
    output.mkdir(parents=True, exist_ok=True)
    evidence = Path(tempfile.mkdtemp(prefix="page-profile-", dir=output))
    command_log = evidence / "commands.log"
    command_log.write_text("")
    print(f"Evidence: {evidence}", flush=True)

    def execute(command, log, timeout=300):
        with command_log.open("a") as stream:
            stream.write(shlex.join(map(str, command)) + "\n")
        with log.open("wb") as stream:
            subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True, timeout=timeout)

    metadata = {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "worktree": str(ROOT), "host": platform.platform(), "python": platform.python_version(),
        "configuration": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
        "source_sha256": {}, "publication": "NONE", "build_jobs": 1,
        "lzo_rle": "NOT_RUN", "kernel_runtime": "NOT_RUN",
    }
    if hasattr(os, "sched_getaffinity"):
        metadata["allowed_cpus"] = sorted(os.sched_getaffinity(0))
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        metadata["cpu_model"] = next((line.split(":", 1)[1].strip()
                                      for line in cpuinfo.read_text().splitlines()
                                      if line.startswith("model name")), platform.processor())
    metadata["dependency_environment"] = {key: os.environ[key]
                                           for key in ("PKG_CONFIG_PATH", "LD_LIBRARY_PATH")
                                           if key in os.environ}
    paths = [ROOT / "src" / (name + ".c") for name in CORE]
    paths += list((ROOT / "include").glob("*.h"))
    paths += [ROOT / "bench/bench_page_profile.c", Path(__file__).resolve()]
    metadata["source_sha256"] = {str(p.relative_to(ROOT)): sha256(p) for p in paths}
    metadata["fixture_recipe"] = {
        "source": "bench/bench_page_profile.c", "function": "make_page",
        "source_sha256": metadata["source_sha256"]["bench/bench_page_profile.c"],
        "scope": "SHA-256 covers the complete source containing the deterministic recipe, not serialized page bytes.",
        "page_fingerprints": "FNV-1a64, non-cryptographic; recorded separately for every generated page.",
        "input_source": "Synthetic only; no private captures, kernel pages, or imported Linux code.",
    }
    for key, command in (("git_head", ["git", "rev-parse", "HEAD"]),
                         ("git_status", ["git", "status", "--short"]),
                         ("compiler", [args.cc, "--version"])):
        try:
            metadata[key] = subprocess.check_output(command, cwd=ROOT, text=True, stderr=subprocess.STDOUT).strip()
        except (OSError, subprocess.CalledProcessError) as error:
            metadata[key] = str(error)
    metadata_path = evidence / "provenance.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    try:
        if not shutil.which(args.cc) or not shutil.which("pkg-config"):
            raise FileNotFoundError("compiler and pkg-config are required")
        pc = ["pkg-config", "--cflags", "--libs", "liblz4", "libzstd"]
        try:
            execute(pc, evidence / "pkg-config.log")
        except subprocess.CalledProcessError as error:
            raise FileNotFoundError("LZ4/Zstd development metadata unavailable; see pkg-config.log") from error
        dependencies = shlex.split((evidence / "pkg-config.log").read_text())
        flags = ["-O3", "-flto", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-D_POSIX_C_SOURCE=200809L", "-Iinclude"]
        objects = []
        for name in CORE:
            obj = evidence / (name + ".o")
            simd = ["-mavx2"] if platform.machine() == "x86_64" and name in ("vv_simd", "vv_decoder") else []
            execute([args.cc, *flags, *simd, "-c", f"src/{name}.c", "-o", str(obj)], evidence / f"build-{name}.log")
            objects.append(str(obj))
        # rpaths allow pkg-config dependencies in immutable/non-system stores.
        rpaths = ["-Wl,-rpath," + flag[2:] for flag in dependencies if flag.startswith("-L")]
        binary = evidence / "bench_page_profile"
        execute([args.cc, *flags, "bench/bench_page_profile.c", *objects, *dependencies, *rpaths,
                 "-o", str(binary)], evidence / "build-link.log")
        current_hashes = {str(p.relative_to(ROOT)): sha256(p) for p in paths}
        if current_hashes != metadata["source_sha256"]:
            raise ValueError("source changed during the build; use a stable worktree")
        metadata["binary_sha256"] = sha256(binary)
        execute([str(binary), "--self-test", "--pages", "2"], evidence / "self-test.csv")
        command = [str(binary), "--pages", str(args.pages), "--samples", str(args.samples),
                   "--batch-samples", str(args.batch_samples), "--min-batch-ms", str(args.min_batch_ms)]
        for key in ("size", "fixture", "codec"):
            if getattr(args, key) is not None:
                command += ["--" + key, str(getattr(args, key))]
        if not args.run:
            command.append("--self-test")
        if args.cpu is not None:
            if not shutil.which("taskset"):
                raise FileNotFoundError("taskset is required for --cpu")
            command = ["taskset", "-c", str(args.cpu), *command]
        raw = evidence / "raw.csv"
        with command_log.open("a") as stream:
            stream.write(shlex.join(command) + "\n")
        with raw.open("wb") as stdout, (evidence / "run.log").open("wb") as stderr:
            subprocess.run(command, cwd=ROOT, stdout=stdout, stderr=stderr, check=True, timeout=1800)
        count = summarize(raw, evidence / "summary.json")
        metadata["sources_unchanged_after_run"] = all(
            sha256(ROOT / name) == digest for name, digest in metadata["source_sha256"].items())
        metadata["status"] = "PASS"
        metadata["timings"] = "RUN" if args.run else "NOT_RUN"
        metadata["profile_count"] = count
        print(f"PASS: {count} profiles; timings {metadata['timings']}; LZO-RLE NOT RUN", flush=True)
        return 0
    except FileNotFoundError as error:
        metadata["status"] = "BLOCKED"
        metadata["error"] = str(error)
        print(f"BLOCKED: {error}", file=sys.stderr)
        return 2
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        metadata["status"] = "FAIL"
        metadata["error"] = str(error)
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
