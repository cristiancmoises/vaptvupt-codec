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
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CORE = ["vv_encoder", "vv_decoder", "vv_simd", "vv_xxh64", "vv_huffman",
        "vv_ans", "vv_bcj", "vaptvupt_api"]
SIZES = (4096, 16384, 65536)
FIXTURES = ("text", "records", "random", "repeating", "zero", "same-filled")
CODECS = ("vv-oneshot-c0", "vv-oneshot-c1", "vv-context-c0", "vv-context-c1",
          "lz4-default", "lz4-extstate", "zstd-oneshot-1-c0", "zstd-oneshot-3-c0",
          "zstd-context-1-c0", "zstd-context-1-c1", "zstd-context-3-c0", "zstd-context-3-c1")
FIELDS = ("status", "fixture", "input_bytes", "page_index", "input_fnv1a64",
          "consumer_bypassed", "codec", "checksum", "phase", "sample", "iterations",
          "elapsed_ns", "ns_per_call", "compressed_bytes", "framing_bytes",
          "payload_bytes", "not_smaller_than_input", "state_bytes")
STATUSES = ("FAIL", "BLOCKED", "NOT_RUN", "PASS")
EXIT_STATUS = {"PASS": 0, "FAIL": 1, "BLOCKED": 2, "NOT_RUN": 3}


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


def combined_status(statuses):
    return next((status for status in STATUSES if status in statuses), "FAIL")


def write_report(destination, report):
    destination.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")


def expected_phases(codec, config):
    if config["self_test"]:
        return {}
    phases = {"encode_call": config["samples"], "decode_call": config["samples"],
              "encode_batch": config["batch_samples"], "decode_batch": config["batch_samples"]}
    if "context" in codec or codec == "lz4-extstate":
        phases.update({name: config["samples"]
                       for name in ("context_setup", "first_encode_call", "first_decode_call")})
    if codec.startswith("vv-context"):
        phases["vv_init_preallocated"] = config["samples"]
    return phases


def requested_profiles(size=None, fixture=None, codec=None):
    return {(f, n, c) for f in ((fixture,) if fixture else FIXTURES)
            for n in ((size,) if size else SIZES) for c in ((codec,) if codec else CODECS)}


def _summarize(raw, config, profiles):
    for name, low, high in (("pages", 1, 256), ("samples", 101, 10001), ("batch_samples", 3, 31)):
        if type(config.get(name)) is not int or not low <= config[name] <= high:
            raise ValueError(f"invalid requested configuration: {name}")
    if type(config.get("self_test")) is not bool:
        raise ValueError("self_test must be an explicit boolean")
    groups = {}
    metadata = []
    declared = {}
    cohort_hashes = {}
    with raw.open() as stream:
        def lines():
            for line in stream:
                if line.startswith("#"):
                    metadata.append(line.rstrip())
                    for field in line[1:].strip().split(";"):
                        name, separator, value = field.partition("=")
                        if separator and name in ("pages", "individual_samples", "batch_samples", "self_test"):
                            if name in declared:
                                raise ValueError(f"duplicate configuration declaration: {name}")
                            declared[name] = value
                else:
                    yield line
        reader = csv.DictReader(lines())
        if reader.fieldnames != list(FIELDS):
            raise ValueError("missing, reordered, or unexpected CSV columns")
        for line_number, row in enumerate(reader, 2):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"incomplete CSV row at data line {line_number}")
            if row["status"] not in STATUSES or row["fixture"] not in FIXTURES or row["codec"] not in CODECS:
                raise ValueError(f"unknown status, fixture, or codec at data line {line_number}")
            numeric = {}
            for field in FIELDS:
                if field in ("status", "fixture", "input_fnv1a64", "codec", "phase", "ns_per_call"):
                    continue
                value = row[field]
                if not value.isascii() or not value.isdecimal() or len(value) > 20:
                    raise ValueError(f"invalid {field} at data line {line_number}")
                numeric[field] = int(value)
                if numeric[field] > (1 << 64) - 1:
                    raise ValueError(f"out-of-range {field} at data line {line_number}")
            size, page = numeric["input_bytes"], numeric["page_index"]
            if size not in SIZES or page >= config["pages"]:
                raise ValueError(f"out-of-range input size/page at data line {line_number}")
            if not re.fullmatch(r"[0-9a-f]{16}", row["input_fnv1a64"]):
                raise ValueError(f"invalid FNV-1a fingerprint at data line {line_number}")
            if numeric["consumer_bypassed"] != int(row["fixture"] in ("zero", "same-filled")):
                raise ValueError(f"incorrect bypass-control label at data line {line_number}")
            if numeric["checksum"] != int(row["codec"].endswith("-c1")):
                raise ValueError(f"incorrect checksum label at data line {line_number}")
            comp, framing, payload = (numeric[name] for name in ("compressed_bytes", "framing_bytes", "payload_bytes"))
            if not comp or framing + payload != comp or numeric["not_smaller_than_input"] != int(comp >= size):
                raise ValueError(f"inconsistent compressed-size accounting at data line {line_number}")
            if row["codec"].startswith("lz4-") and framing:
                raise ValueError(f"raw LZ4 block has framing bytes at data line {line_number}")
            iterations, elapsed = numeric["iterations"], numeric["elapsed_ns"]
            ns = float(row["ns_per_call"])
            if not 1 <= iterations <= 1048576 or not math.isfinite(ns) or ns < 0:
                raise ValueError(f"invalid timing at data line {line_number}")
            if not math.isclose(ns, elapsed / iterations, rel_tol=1e-12, abs_tol=.00051):
                raise ValueError(f"inconsistent amortized timing at data line {line_number}")
            key = (row["fixture"], size, row["codec"])
            if key not in profiles:
                raise ValueError(f"unrequested profile: {key}")
            phase, sample = row["phase"], numeric["sample"]
            wanted_phases = expected_phases(row["codec"], config)
            if phase not in wanted_phases and phase != "size":
                raise ValueError(f"unknown or unrequested phase: {phase}")
            if phase == "size":
                if iterations != 1 or sample or elapsed or ns:
                    raise ValueError(f"size record contains a timing/sample at data line {line_number}")
            else:
                if not phase.endswith("_batch") and iterations != 1:
                    raise ValueError("individual latency record is an amortized batch")
                if phase.endswith("_batch") and row["status"] == "PASS" and iterations % config["pages"]:
                    raise ValueError("batch does not visit a whole number of page cohorts")
                if row["status"] == "PASS" and (not elapsed or sample >= wanted_phases[phase]):
                    raise ValueError(f"invalid successful timing/sample at data line {line_number}")
                wanted_page = ((iterations - 1) % config["pages"] if phase.endswith("_batch") else
                               0 if phase in ("context_setup", "vv_init_preallocated") else sample % config["pages"])
                if page != wanted_page:
                    raise ValueError(f"unexpected sample page at data line {line_number}")
            cohort = (row["fixture"], size, page)
            if cohort_hashes.setdefault(cohort, row["input_fnv1a64"]) != row["input_fnv1a64"]:
                raise ValueError(f"input fingerprint differs between codecs/samples: {cohort}")
            identity = (row["input_fnv1a64"], comp, framing, payload, numeric["not_smaller_than_input"])
            group = groups.setdefault(key, {"pages": {}, "phases": {}, "status": set(), "identities": {}})
            if group["identities"].setdefault(page, identity) != identity:
                raise ValueError(f"page size/accounting changed between phases: {key}, page {page}")
            group["status"].add(row["status"])
            phase = row["phase"]
            if phase == "size":
                if page in group["pages"]:
                    raise ValueError(f"duplicate page in {key}: {page}")
                group["pages"][page] = {
                    "input_fnv1a64": row["input_fnv1a64"],
                    "compressed_bytes": comp, "framing_bytes": framing, "payload_bytes": payload,
                    "not_smaller_than_input": bool(numeric["not_smaller_than_input"]),
                }
            else:
                phase_data = group["phases"].setdefault(phase, {"ns": [], "state_bytes": [], "status": set(), "seen": set()})
                identity = (row["status"], sample)
                if identity in phase_data["seen"]:
                    raise ValueError(f"duplicate phase sample: {key}, {phase}, {identity}")
                phase_data["seen"].add(identity)
                phase_data["status"].add(row["status"])
                if row["status"] == "PASS":
                    phase_data["ns"].append(ns)
                    phase_data["state_bytes"].append(numeric["state_bytes"])
    wanted_declarations = {"pages": str(config["pages"]), "individual_samples": str(config["samples"]),
                           "batch_samples": str(config["batch_samples"]), "self_test": str(int(config["self_test"]))}
    if declared != wanted_declarations:
        raise ValueError(f"CSV configuration differs from requested run: {declared}")
    if set(groups) != profiles:
        raise ValueError(f"incomplete profile matrix: expected {len(profiles)}, observed {len(groups)}")
    results = []
    for (fixture, size, codec), group in sorted(groups.items()):
        if set(group["pages"]) != set(range(config["pages"])):
            raise ValueError(f"incomplete page cohort: {fixture}, {size}, {codec}")
        wanted_phases = expected_phases(codec, config)
        if set(group["phases"]) != set(wanted_phases):
            raise ValueError(f"incomplete timing phases: {fixture}, {size}, {codec}")
        pages = list(group["pages"].values())
        compressed = sum(p["compressed_bytes"] for p in pages)
        item = {
            "fixture": fixture, "input_bytes": size, "codec": codec,
            "status": combined_status(group["status"]), "observed_statuses": sorted(group["status"]), "page_count": len(pages),
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
            status = combined_status(data["status"])
            if status == "PASS" and len(values) != wanted_phases[phase]:
                raise ValueError(f"incomplete successful phase: {fixture}, {size}, {codec}, {phase}")
            report = {"status": status, "observed_statuses": sorted(data["status"]),
                      "samples": len(values), "expected_samples": wanted_phases[phase],
                      "complete": status == "PASS"}
            if values and status == "PASS":
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
    return {"status": combined_status({result["status"] for result in results}),
            "metadata": metadata, "results": results, "expected_profiles": len(profiles)}


def summarize(raw, destination, config, profiles):
    report = {
        "schema_version": 2,
        "status_scope": "Requested userspace profiles only; kernel runtime and LZO-RLE are separate NOT_RUN items.",
        "configuration": config,
        "kernel_runtime": "NOT_RUN",
        "lzo_rle": {"status": "NOT_RUN", "reason": "No suitable userspace LZO-RLE implementation; ordinary LZO is not substituted."},
        "percentiles": "Nearest rank of individual observed calls, clock/dispatch overhead included; batching is separate.",
        "limits": ["Synthetic warmed page cohorts, not private captures or kernel workloads.",
                   "Zero and same-filled controls are consumer-bypassed; do not count them as consumer codec wins.",
                   "Context setup excludes first-use allocations; first_encode_call and first_decode_call report those separately.",
                   "Reported state sizes are retained context sizes, not allocator overhead, RSS, peak memory, or hidden one-shot workspace.",
                   "A not-smaller page rate describes this generated cohort, not a production workload.",
                   "Framing subtraction is size accounting; no raw-VV/Zstd performance is inferred."],
        "results": [],
    }
    try:
        report.update(_summarize(raw, config, set(profiles)))
    except (OSError, ValueError, KeyError, csv.Error) as error:
        report.update({"status": "FAIL", "errors": [str(error)]})
    write_report(destination, report)
    return report


def finalize_measurement(report, metadata, root, hashes, destination):
    changed = []
    for name, digest in hashes.items():
        try:
            if sha256(root / name) == digest:
                continue
        except OSError:
            pass
        changed.append(name)
    metadata["sources_unchanged_after_run"] = not changed
    if changed:
        report["status"] = "FAIL"
        report.setdefault("errors", []).append("source changed during execution: " + ", ".join(changed))
    report["measurement_quality"] = metadata["measurement_quality"]
    for name in ("vv_build_mode", "vv_build_scope", "effective_child_cpus"):
        if name in metadata:
            report[name] = metadata[name]
    metadata["status"] = report["status"]
    metadata["profile_count"] = len(report["results"])
    write_report(destination, report)
    return EXIT_STATUS[report["status"]]


def codec_compile_flags(name, scalar, machine):
    if scalar:
        return ["-DVV_DISABLE_SIMD=1", "-fno-tree-vectorize"]
    return ["-mavx2"] if machine == "x86_64" and name in ("vv_simd", "vv_decoder") else []


def read_cpu_configuration(cpu, sysroot=Path("/sys/devices/system/cpu")):
    paths = {"smt_active": sysroot / "smt/active", "smt_control": sysroot / "smt/control"}
    if cpu is not None:
        base = sysroot / f"cpu{cpu}"
        paths.update({"thread_siblings": base / "topology/thread_siblings_list",
                      "governor": base / "cpufreq/scaling_governor",
                      "frequency_driver": base / "cpufreq/scaling_driver",
                      "minimum_khz": base / "cpufreq/scaling_min_freq",
                      "maximum_khz": base / "cpufreq/scaling_max_freq"})
    result = {}
    for name, path in paths.items():
        try:
            result[name] = {"status": "READ", "value": path.read_text().strip(), "path": str(path)}
        except OSError as error:
            result[name] = {"status": "NOT_AVAILABLE", "reason": str(error), "path": str(path)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="Evidence parent directory; each invocation makes a new child.")
    parser.add_argument("--run", action="store_true", help="Collect timings; otherwise only build and self-test.")
    parser.add_argument("--scalar", action="store_true",
                        help="Disable VV intrinsics/dispatch and compiler auto-vectorization; libc and competitors remain userspace builds.")
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
        "lzo_rle": "NOT_RUN", "kernel_runtime": "NOT_RUN", "timings": "NOT_RUN",
        "vv_build_mode": "scalar" if args.scalar else "userspace-default",
        "vv_build_scope": ("VV intrinsics/dispatch and compiler auto-vectorization disabled; not a kernel/general-register-only certification."
                           if args.scalar else "Default x86-64 decoder/SIMD objects use AVX2; other VV objects use baseline compiler settings."),
        "measurement_quality": ("not_measured" if not args.run else
                                "pinned_userspace" if args.cpu is not None else "preliminary_unpinned"),
        "affinity_policy": "Unpinned measurements, including the earlier 4 KiB exploratory run, are preliminary.",
        "cpu_configuration_before": read_cpu_configuration(args.cpu),
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
        affinity_command = [sys.executable, "-c", "import json,os; print(json.dumps(sorted(os.sched_getaffinity(0))))"]
        if args.cpu is not None:
            if not shutil.which("taskset"):
                raise FileNotFoundError("taskset is required for --cpu")
            affinity_command = ["taskset", "-c", str(args.cpu), *affinity_command]
        if hasattr(os, "sched_getaffinity"):
            execute(affinity_command, evidence / "affinity.json")
            metadata["effective_child_cpus"] = json.loads((evidence / "affinity.json").read_text())
            if args.cpu is not None and metadata["effective_child_cpus"] != [args.cpu]:
                raise ValueError("taskset did not produce the requested child affinity")
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
            codec_flags = codec_compile_flags(name, args.scalar, platform.machine())
            execute([args.cc, *flags, *codec_flags, "-c", f"src/{name}.c", "-o", str(obj)], evidence / f"build-{name}.log")
            objects.append(str(obj))
        # rpaths allow pkg-config dependencies in immutable/non-system stores.
        rpaths = ["-Wl,-rpath," + flag[2:] for flag in dependencies if flag.startswith("-L")]
        binary = evidence / "bench_page_profile"
        link_flags = codec_compile_flags("bench_page_profile", args.scalar, platform.machine())
        execute([args.cc, *flags, *link_flags, "bench/bench_page_profile.c", *objects, *dependencies, *rpaths,
                 "-o", str(binary)], evidence / "build-link.log")
        current_hashes = {str(p.relative_to(ROOT)): sha256(p) for p in paths}
        if current_hashes != metadata["source_sha256"]:
            raise ValueError("source changed during the build; use a stable worktree")
        metadata["binary_sha256"] = sha256(binary)
        execute([str(binary), "--self-test", "--pages", "2"], evidence / "self-test.csv")
        self_test = summarize(evidence / "self-test.csv", evidence / "self-test-summary.json",
                              {"pages": 2, "samples": 101, "batch_samples": 7, "self_test": True}, requested_profiles())
        if self_test["status"] != "PASS":
            raise ValueError("preliminary self-test data failed completeness/validation checks")
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
            if args.run:
                metadata["timings"] = "STARTED"
            subprocess.run(command, cwd=ROOT, stdout=stdout, stderr=stderr, check=True, timeout=1800)
        config = {"pages": args.pages, "samples": args.samples, "batch_samples": args.batch_samples,
                  "self_test": not args.run}
        report = summarize(raw, evidence / "summary.json", config,
                           requested_profiles(args.size, args.fixture, args.codec))
        exit_status = finalize_measurement(report, metadata, ROOT, metadata["source_sha256"], evidence / "summary.json")
        metadata["timings"] = ("RUN" if not exit_status else "INCOMPLETE") if args.run else "NOT_RUN"
        metadata["cpu_configuration_after"] = read_cpu_configuration(args.cpu)
        print(f"{metadata['status']}: {metadata['profile_count']} profiles; timings {metadata['timings']}; LZO-RLE NOT RUN", flush=True)
        for error in report.get("errors", []):
            print(error, file=sys.stderr)
        return exit_status
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
        if metadata.get("status") in ("FAIL", "BLOCKED"):
            if metadata["timings"] == "STARTED":
                metadata["timings"] = "INCOMPLETE"
            summary_path = evidence / "summary.json"
            try:
                failed_report = json.loads(summary_path.read_text())
            except (OSError, ValueError):
                failed_report = {"schema_version": 2, "results": []}
            if failed_report.get("status") != metadata["status"]:
                failed_report["status"] = metadata["status"]
                failed_report.setdefault("errors", []).append(metadata.get("error", "execution failed"))
                write_report(summary_path, failed_report)
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())
