"""The per-owner report must reject misleading counts, timings and partial runs."""

import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.benchmark_owner_capacity import campaign
from scripts.owner_capacity_report import SCOPE, validate_profiles


def fixture():
    run = {"metadata": {"owner_profile_version": 1, "owner_profile_scope": SCOPE,
                        "hot_worker_clients": 3, "hot_worker": 1, "workload": "get_only",
                        "storage_mode": "volatile", "durable_reads": "none",
                        "owner_latency_scope": "get-responses",
                        "latency_measurement": "pipelined-response", "latency_sample_stride": 1},
           "results": [{"workers": 2, "threads": 4, "samples": 2, "operations": 40, "latency_samples": 80,
                        "name": "server_tcp_get_only_volatile_hot_worker_1_clients_3"}]}
    rows = []
    for repeat in range(2):
        for owner, clients, operations, seconds in [(0, 1, 10, 1.0), (1, 3, 30, 2.0)]:
            rows.append({"repeat": repeat, "owner": owner, "clients": clients,
                         "planned_operations": operations, "planned_gets": operations, "planned_puts": 0,
                         "completed_operations": operations, "sample_seconds": 2.0,
                         "first_client_done_seconds": 0.5, "completion_window_seconds": seconds,
                         "owner_window_ops_per_second": operations / seconds,
                         "latency_samples": operations, "p50_latency_ns": 100.0,
                         "p95_latency_ns": 200.0, "p99_latency_ns": 300.0,
                         "p999_latency_ns": 400.0, "max_latency_ns": 500.0})
    return run, rows


class OwnerCapacityReportTests(unittest.TestCase):
    def test_complete_profile_keeps_cold_and_hot_completion_windows_distinct(self):
        run, rows = fixture()
        validate_profiles(run, rows)
        self.assertEqual(1.0, rows[0]["completion_window_seconds"])
        self.assertEqual(2.0, rows[1]["completion_window_seconds"])

    def test_missing_duplicate_and_out_of_range_coordinates_fail(self):
        run, rows = fixture()
        for bad in [rows[:-1], [rows[0], rows[0], *rows[2:]],
                    [{**rows[0], "owner": 2}, *rows[1:]]]:
            with self.subTest(rows=bad), self.assertRaises(ValueError):
                validate_profiles(run, bad)

    def test_counts_latency_and_rate_tampering_fail_closed(self):
        run, rows = fixture()
        patches = [{"clients": 2}, {"completed_operations": 9}, {"planned_operations": 11},
                   {"planned_gets": 9, "planned_puts": 1}, {"latency_samples": 9},
                   {"completion_window_seconds": 3.0}, {"first_client_done_seconds": 1.5},
                   {"owner_window_ops_per_second": 11.0}, {"p99_latency_ns": float("nan")},
                   {"p95_latency_ns": 301.0}, {"sample_seconds": 3.0}]
        for patch in patches:
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                validate_profiles(run, [{**rows[0], **patch}, *rows[1:]])

    def test_skew_and_measurement_identity_must_match_the_source_contract(self):
        run, rows = fixture()
        for patch in [{"owner_profile_version": 2}, {"hot_worker_clients": 4},
                      {"hot_worker": 2}, {"latency_measurement": "disabled"},
                      {"owner_latency_scope": "all-responses"}]:
            bad = copy.deepcopy(run)
            bad["metadata"].update(patch)
            with self.subTest(patch=patch), self.assertRaises(ValueError):
                validate_profiles(bad, rows)
        run["results"][0]["name"] = "server_tcp_get_only_volatile"
        with self.assertRaisesRegex(ValueError, "identity"):
            validate_profiles(run, rows)

    def test_per_owner_mix_cannot_be_swapped_while_global_totals_stay_valid(self):
        run, rows = fixture()
        run["metadata"]["workload"] = "read_95_write_5"
        run["results"][0]["name"] = "server_tcp_read_95_write_5_volatile_hot_worker_1_clients_3"
        run["results"][0]["latency_samples"] = 76
        for row in rows:
            if row["owner"] == 1:
                row.update(planned_puts=2, planned_gets=28, latency_samples=28)
        validate_profiles(run, rows)
        rows[0].update(planned_puts=1, planned_gets=9, latency_samples=9)
        rows[1].update(planned_puts=1, planned_gets=29, latency_samples=29)
        with self.assertRaisesRegex(ValueError, "counts disagree"):
            validate_profiles(run, rows)


class OwnerCapacityCampaignTests(unittest.TestCase):
    def test_existing_outputs_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "benchmark"
            binary.write_text("fixture")
            output = root / "evidence"
            output.mkdir()
            marker = output / "campaign.json"
            marker.write_text("User evidence")
            with self.assertRaises(FileExistsError):
                campaign(binary, output)
            self.assertEqual("User evidence", marker.read_text())

    def test_failed_and_timed_out_attempts_keep_diagnostics(self):
        for failure in (subprocess.CompletedProcess([], 1, "failed response", "diagnostic"),
                        subprocess.TimeoutExpired([], 120, output=b"partial response", stderr=b"deadline")):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                binary = root / "benchmark"
                binary.write_text("fixture")
                output = root / "evidence"
                with patch("scripts.benchmark_owner_capacity.environment", return_value={}):
                    with patch("scripts.benchmark_owner_capacity.subprocess.run") as run:
                        if isinstance(failure, Exception):
                            run.side_effect = failure
                        else:
                            run.return_value = failure
                        with self.assertRaises((ValueError, subprocess.TimeoutExpired)):
                            campaign(binary, output)
                manifest = json.loads((output / "campaign.json").read_text())
                self.assertEqual("FAILED", manifest["status"])
                self.assertEqual("FAILED", manifest["runs"][0]["status"])
                self.assertTrue((output / "get-only-c4-hot0.txt").read_text())
                self.assertTrue((output / "get-only-c4-hot0.stderr.txt").read_text())

    def test_rebuilt_binary_invalidates_a_running_campaign(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "benchmark"
            binary.write_text("first binary")
            output = root / "evidence"

            def rebuild(*args, **kwargs):
                binary.write_text("replacement binary")
                return subprocess.CompletedProcess([], 0, "partial measured data", "")

            with patch("scripts.benchmark_owner_capacity.environment", return_value={}):
                with patch("scripts.benchmark_owner_capacity.subprocess.run", side_effect=rebuild):
                    with self.assertRaisesRegex(ValueError, "binary changed"):
                        campaign(binary, output)
            self.assertEqual("FAILED", json.loads((output / "campaign.json").read_text())["status"])
            self.assertTrue((output / "get-only-c4-hot0.txt").is_file())


if __name__ == "__main__":
    unittest.main()
