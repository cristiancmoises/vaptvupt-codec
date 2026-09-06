#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic evidence checks only; this test neither compiles nor benchmarks."""
import copy
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "page_profile_runner", Path(__file__).resolve().parents[1] / "bench/run_page_profile.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class ProfileSummaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vv-profile-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.config = {"pages": 2, "samples": 101, "batch_samples": 3, "self_test": False}
        self.profiles = {("text", 4096, "lz4-default")}

    def make_rows(self, codec="lz4-default", self_test=False):
        def row(phase, sample=0, page=0, iterations=1):
            elapsed = 0 if phase == "size" else (sample + 1) * iterations
            return {"status": "PASS", "fixture": "text", "input_bytes": "4096",
                    "page_index": str(page), "input_fnv1a64": f"{page:016x}",
                    "consumer_bypassed": "0", "codec": codec, "checksum": str(int(codec.endswith("-c1"))),
                    "phase": phase, "sample": str(sample), "iterations": str(iterations),
                    "elapsed_ns": str(elapsed), "ns_per_call": f"{elapsed / iterations:.3f}",
                    "compressed_bytes": "1000", "framing_bytes": "0", "payload_bytes": "1000",
                    "not_smaller_than_input": "0", "state_bytes": "0"}
        rows = [row("size", page=page) for page in range(self.config["pages"])]
        if self_test:
            return rows
        for phase, samples in RUNNER.expected_phases(codec, self.config).items():
            for sample in range(samples):
                iterations = self.config["pages"] * 2 if phase.endswith("_batch") else 1
                page = ((iterations - 1) % self.config["pages"] if phase.endswith("_batch") else
                        0 if phase in ("context_setup", "vv_init_preallocated") else sample % self.config["pages"])
                rows.append(row(phase, sample, page, iterations))
        return rows

    def summarize(self, rows, fields=RUNNER.FIELDS, preamble=None, profiles=None):
        raw, destination = self.directory / "raw.csv", self.directory / "summary.json"
        with raw.open("w", newline="") as stream:
            stream.write(preamble or (f"# pages={self.config['pages']};individual_samples={self.config['samples']};"
                                     f"batch_samples={self.config['batch_samples']};self_test={int(self.config['self_test'])}\n"))
            writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
            writer.writeheader()
            writer.writerows(rows)
        report = RUNNER.summarize(raw, destination, self.config, profiles or self.profiles)
        self.assertEqual(json.loads(destination.read_text()), report)
        return report

    def assert_failure(self, rows, text=None, **kwargs):
        report = self.summarize(rows, **kwargs)
        self.assertEqual(report["status"], "FAIL")
        if text:
            self.assertIn(text, " ".join(report["errors"]))
        return report

    def test_valid_individual_percentiles_and_separate_throughput(self):
        report = self.summarize(self.make_rows())
        self.assertEqual(report["status"], "PASS")
        phase = report["results"][0]["phases"]["encode_call"]
        self.assertEqual((phase["p50_ns"], phase["p95_ns"], phase["p99_ns"]), (51, 96, 100))
        batch = report["results"][0]["phases"]["encode_batch"]
        self.assertIn("median_MBps", batch)
        self.assertNotIn("p99_ns", batch)
        self.assertEqual(report["kernel_runtime"], "NOT_RUN")
        self.assertEqual(report["lzo_rle"]["status"], "NOT_RUN")

    def test_not_run_is_not_a_successful_calibration_or_overall_pass(self):
        rows = self.make_rows()
        marker = next(copy.copy(row) for row in rows if row["phase"] == "encode_batch")
        marker["status"] = "NOT_RUN"
        rows = [row for row in rows if row["phase"] != "encode_batch"] + [marker]
        report = self.summarize(rows)
        self.assertEqual(report["status"], "NOT_RUN")
        self.assertEqual(report["results"][0]["status"], "NOT_RUN")
        phase = report["results"][0]["phases"]["encode_batch"]
        self.assertEqual(phase["status"], "NOT_RUN")
        self.assertFalse(phase["complete"])
        self.assertNotIn("median_MBps", phase)
        self.assertEqual(RUNNER.EXIT_STATUS[report["status"]], 3)

    def test_fail_and_blocked_rows_propagate(self):
        for status in ("FAIL", "BLOCKED"):
            with self.subTest(status=status):
                rows = self.make_rows()
                next(row for row in rows if row["phase"] == "encode_call")["status"] = status
                report = self.summarize(rows)
                self.assertEqual(report["status"], status)
                phase = report["results"][0]["phases"]["encode_call"]
                self.assertFalse(phase["complete"])
                self.assertNotIn("p99_ns", phase)

    def test_empty_and_missing_columns_fail(self):
        self.assert_failure([], "incomplete profile matrix")
        self.assert_failure(self.make_rows(), "CSV columns", fields=RUNNER.FIELDS[:-1])

    def test_missing_cell_fails(self):
        rows = self.make_rows()
        del rows[0]["compressed_bytes"]
        self.assert_failure(rows, "invalid compressed_bytes")

    def test_incomplete_page_cohort_fails(self):
        rows = [row for row in self.make_rows() if row["page_index"] != "1"]
        self.assert_failure(rows, "incomplete page cohort")

    def test_missing_phase_and_sample_fail(self):
        rows = self.make_rows()
        self.assert_failure([row for row in rows if row["phase"] != "decode_batch"], "incomplete timing phases")
        self.assert_failure(rows[:-1], "incomplete successful phase")

    def test_duplicate_rows_fail(self):
        rows = self.make_rows()
        self.assert_failure(rows + [rows[0]], "duplicate page")
        self.assert_failure(rows + [rows[-1]], "duplicate phase sample")

    def test_missing_requested_profile_fails(self):
        self.assert_failure(self.make_rows(), "incomplete profile matrix",
                            profiles=self.profiles | {("text", 4096, "lz4-extstate")})

    def test_configuration_mismatch_fails(self):
        self.assert_failure(self.make_rows(), "configuration differs",
                            preamble="# pages=2;individual_samples=100;batch_samples=3;self_test=0\n")

    def test_less_than_101_requested_samples_cannot_produce_tail_claims(self):
        rows = self.make_rows()
        self.config["samples"] = 100
        self.assert_failure(rows, "invalid requested configuration")

    def test_malformed_numbers_and_statuses_fail(self):
        mutations = (("elapsed_ns", "-1"), ("elapsed_ns", "1" * 40),
                     ("ns_per_call", "nan"), ("ns_per_call", "inf"),
                     ("ns_per_call", "12.0"), ("status", "MAYBE"),
                     ("input_fnv1a64", "not-a-hash"), ("checksum", "1"),
                     ("consumer_bypassed", "1"), ("payload_bytes", "999"),
                     ("not_smaller_than_input", "1"))
        for field, value in mutations:
            with self.subTest(field=field, value=value):
                rows = self.make_rows()
                rows[0][field] = value
                self.assert_failure(rows)

    def test_input_hash_and_size_cannot_change_between_phases(self):
        for field, value in (("input_fnv1a64", "0123456789abcdef"), ("compressed_bytes", "1001")):
            with self.subTest(field=field):
                rows = self.make_rows()
                rows[-1][field] = value
                self.assert_failure(rows)

    def test_individual_latency_cannot_be_a_batch_average(self):
        rows = self.make_rows()
        row = next(row for row in rows if row["phase"] == "encode_call")
        row.update({"iterations": "2", "elapsed_ns": "2", "ns_per_call": "1.000"})
        self.assert_failure(rows, "individual latency record is an amortized batch")

    def test_batch_must_cover_whole_cohorts(self):
        rows = self.make_rows()
        row = next(row for row in rows if row["phase"] == "encode_batch")
        row.update({"iterations": "3", "elapsed_ns": "3", "ns_per_call": "1.000", "page_index": "0"})
        self.assert_failure(rows, "whole number of page cohorts")

    def test_context_phases_require_101_samples(self):
        rows = self.make_rows("vv-context-c1")
        report = self.summarize(rows, profiles={("text", 4096, "vv-context-c1")})
        self.assertEqual(report["status"], "PASS")
        phases = report["results"][0]["phases"]
        for name in ("context_setup", "first_encode_call", "first_decode_call", "vv_init_preallocated"):
            self.assertEqual(phases[name]["samples"], 101)
        self.assert_failure(rows[:-1], "incomplete successful phase", profiles={("text", 4096, "vv-context-c1")})

    def test_self_test_has_no_timing_phases(self):
        self.config["self_test"] = True
        report = self.summarize(self.make_rows(self_test=True))
        self.assertEqual(report["status"], "PASS")
        self.assertFalse(report["results"][0]["phases"])

    def test_source_change_invalidates_summary_provenance_and_exit(self):
        source = self.directory / "source.c"
        source.write_text("before\n")
        hashes = {"source.c": RUNNER.sha256(source)}
        report = self.summarize(self.make_rows())
        source.write_text("after\n")
        metadata = {"measurement_quality": "pinned_userspace"}
        exit_status = RUNNER.finalize_measurement(report, metadata, self.directory, hashes, self.directory / "summary.json")
        self.assertEqual(exit_status, 1)
        self.assertEqual(metadata["status"], "FAIL")
        self.assertFalse(metadata["sources_unchanged_after_run"])
        saved = json.loads((self.directory / "summary.json").read_text())
        self.assertEqual(saved["status"], "FAIL")
        self.assertIn("source changed", saved["errors"][0])

    def test_not_run_propagates_to_final_provenance_and_exit(self):
        report = {"status": "NOT_RUN", "results": []}
        metadata = {"measurement_quality": "preliminary_unpinned"}
        self.assertEqual(RUNNER.finalize_measurement(report, metadata, self.directory, {}, self.directory / "summary.json"), 3)
        self.assertEqual(metadata["status"], "NOT_RUN")

    def test_build_mode_and_pinning_survive_in_the_summary(self):
        report = self.summarize(self.make_rows())
        metadata = {"measurement_quality": "pinned_userspace", "vv_build_mode": "scalar",
                    "vv_build_scope": "userspace only", "effective_child_cpus": [4]}
        self.assertEqual(RUNNER.finalize_measurement(report, metadata, self.directory, {}, self.directory / "summary.json"), 0)
        self.assertEqual(report["vv_build_mode"], "scalar")
        self.assertEqual(report["effective_child_cpus"], [4])


class BuildModeTests(unittest.TestCase):
    def test_scalar_flags_and_default_userspace_avx2(self):
        for source in (*RUNNER.CORE, "bench_page_profile"):
            scalar = RUNNER.codec_compile_flags(source, True, "x86_64")
            self.assertIn("-DVV_DISABLE_SIMD=1", scalar)
            self.assertIn("-fno-tree-vectorize", scalar)
            self.assertNotIn("-mavx2", scalar)
        self.assertEqual(RUNNER.codec_compile_flags("vv_decoder", False, "x86_64"), ["-mavx2"])
        self.assertEqual(RUNNER.codec_compile_flags("vv_simd", False, "x86_64"), ["-mavx2"])
        self.assertEqual(RUNNER.codec_compile_flags("vv_encoder", False, "x86_64"), [])
        self.assertEqual(RUNNER.codec_compile_flags("vv_decoder", False, "aarch64"), [])

    def test_full_matrix_has_216_profiles(self):
        self.assertEqual(len(RUNNER.requested_profiles()), 216)

    def test_missing_sysfs_is_explicit(self):
        with tempfile.TemporaryDirectory(prefix="vv-cpu-test-") as directory:
            result = RUNNER.read_cpu_configuration(4, Path(directory))
            self.assertTrue(all(field["status"] == "NOT_AVAILABLE" for field in result.values()))


if __name__ == "__main__":
    unittest.main()
