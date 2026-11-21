#!/usr/bin/env python3
"""Validate finite closed-loop TCP owner profiles without inferring saturation."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import subprocess
import tempfile

if __package__:
    from .benchmark_report import fields, parse_output, validate_runs
else:
    from benchmark_report import fields, parse_output, validate_runs

SCOPE = "per-repeat-finite-closed-loop;not-fixed-offered-load"
COUNTS = ("repeat", "owner", "clients", "planned_operations", "planned_gets", "planned_puts",
          "completed_operations", "latency_samples")
TIMES = ("sample_seconds", "first_client_done_seconds", "completion_window_seconds",
         "owner_window_ops_per_second")
QUANTILES = ("p50_latency_ns", "p95_latency_ns", "p99_latency_ns", "p999_latency_ns", "max_latency_ns")


def integer(value, minimum=0):
    return isinstance(value, int) and not isinstance(value, bool) and value >= minimum


def finite(value, minimum=0):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value) and value >= minimum


def validate_profiles(run: dict, rows: list[dict]) -> None:
    metadata = run["metadata"]
    if metadata.get("owner_profile_version") != 1 or metadata.get("owner_profile_scope") != SCOPE:
        raise ValueError("missing or unsupported owner profile contract")
    if len(run["results"]) != 1:
        raise ValueError("owner profiles require exactly one benchmark result")
    result = run["results"][0]
    workers, clients, repeats = result["workers"], result["threads"], result["samples"]
    if not (integer(workers, 1) and integer(clients, workers) and integer(repeats, 1)
            and workers <= 64 and clients <= 1024 and workers * repeats <= 20000):
        raise ValueError("invalid owner profile dimensions")
    if len(rows) != workers * repeats:
        raise ValueError("incomplete owner profile matrix")
    hot_clients, hot_worker = metadata.get("hot_worker_clients"), metadata.get("hot_worker")
    if not integer(hot_clients) or not integer(hot_worker) or hot_worker >= workers:
        raise ValueError("invalid hot Worker metadata")
    if hot_clients and (workers < 2 or hot_clients >= clients or clients - hot_clients < workers - 1):
        raise ValueError("hot Worker clients leave a sibling without traffic")
    if not hot_clients and hot_worker:
        raise ValueError("hot Worker identity without a skew")
    expected_name = f"server_tcp_{metadata.get('workload')}_{metadata.get('storage_mode')}"
    if metadata.get("durable_reads") == "cold":
        expected_name += "_cold"
    if hot_clients:
        expected_name += f"_hot_worker_{hot_worker}_clients_{hot_clients}"
    if result["name"] != expected_name:
        raise ValueError("benchmark identity disagrees with workload, storage or skew")
    factor = 2 if metadata.get("workload") == "read_after_write" else 1
    latency_scope = "all-responses" if factor == 2 else "get-responses"
    if metadata.get("owner_latency_scope") != latency_scope:
        raise ValueError("owner latency population disagrees with the workload")
    operations = result["operations"]
    if not integer(operations, clients * factor) or operations % factor:
        raise ValueError("invalid operation count")
    keys = operations // factor
    periods = {"get_only": 0, "read_after_write": 1, "read_99_write_1": 100,
               "read_95_write_5": 20, "read_90_write_10": 10}
    if metadata.get("workload") not in periods:
        raise ValueError("unknown request mix")
    period = periods[metadata["workload"]]
    quotas = [0] * workers
    expected_puts = [0] * workers
    owner_clients = [0] * workers
    for client in range(clients):
        owner = client % workers
        if hot_clients:
            owner = hot_worker
            if client >= hot_clients:
                owner = (client - hot_clients) % (workers - 1)
                owner += owner >= hot_worker
        owner_clients[owner] += 1
        count = keys // clients + (client < keys % clients)
        quotas[owner] += count * factor
        if factor == 2:
            expected_puts[owner] += count
        elif period:
            # The global-index write selector repeats after at most 100 local slots.
            cycle = period // math.gcd(clients, period)
            first = next((slot for slot in range(cycle) if (client + slot * clients) % period == 0), None)
            if first is not None and first < count:
                expected_puts[owner] += 1 + (count - 1 - first) // cycle
    seen = set()
    totals = {repeat: [0, 0] for repeat in range(repeats)}
    sample_seconds = {}
    for row in rows:
        if set(row) != set(COUNTS + TIMES + QUANTILES):
            raise ValueError("missing or unexpected owner profile fields")
        if not all(integer(row[key]) for key in COUNTS):
            raise ValueError("owner profile counts must be nonnegative integers")
        repeat, owner = row["repeat"], row["owner"]
        if repeat >= repeats or owner >= workers or (repeat, owner) in seen:
            raise ValueError("duplicate or out-of-range owner profile coordinate")
        seen.add((repeat, owner))
        if (row["clients"] != owner_clients[owner] or row["planned_operations"] != quotas[owner]
                or row["completed_operations"] != quotas[owner]
                or row["planned_puts"] != expected_puts[owner]
                or row["planned_gets"] != quotas[owner] - expected_puts[owner]):
            raise ValueError("owner profile counts disagree with the declared workload")
        if not all(finite(row[key]) and row[key] > 0 for key in TIMES):
            raise ValueError("owner profile times and rates must be finite and positive")
        first = row["first_client_done_seconds"]
        last = row["completion_window_seconds"]
        sample = row["sample_seconds"]
        if first > last or (last > sample and not math.isclose(last, sample, rel_tol=1e-5)):
            raise ValueError("invalid owner completion window")
        if repeat in sample_seconds and sample_seconds[repeat] != sample:
            raise ValueError("owners disagree on the shared sample duration")
        sample_seconds[repeat] = sample
        if not math.isclose(row["owner_window_ops_per_second"], quotas[owner] / last, rel_tol=2e-5):
            raise ValueError("owner rate does not match its completion window")
        count = row["latency_samples"]
        eligible = quotas[owner] if factor == 2 else row["planned_gets"]
        quantiles = [row[key] for key in QUANTILES]
        if count > eligible or not all(finite(value) for value in quantiles) or quantiles != sorted(quantiles):
            raise ValueError("invalid owner latency samples or quantiles")
        if (not count and any(quantiles)) or (count and quantiles[0] <= 0):
            raise ValueError("latency values disagree with their sample count")
        measured = metadata.get("latency_measurement") != "disabled"
        if not measured and count:
            raise ValueError("latencies reported with measurement disabled")
        if measured and metadata.get("latency_sample_stride") == 1 and count != eligible:
            raise ValueError("missing per-request latency samples")
        totals[repeat][0] += row["planned_gets"]
        totals[repeat][1] += row["planned_puts"]
    puts = (keys + period - 1) // period if period else 0
    if any(counts != [operations - puts, puts] for counts in totals.values()):
        raise ValueError("per-owner request mix disagrees with the workload")
    if sum(row["latency_samples"] for row in rows) != result.get("latency_samples", 0):
        raise ValueError("owner latency counts disagree with the aggregate result")


def read_profile(path: Path) -> dict:
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("profile file exceeds 64 MiB")
    run = parse_output(path)
    validate_runs([run])
    rows = [fields(line.removeprefix("owner_profile ")) for line in path.read_text().splitlines()
            if line.startswith("owner_profile ")]
    validate_profiles(run, rows)
    summaries = []
    for owner in range(run["results"][0]["workers"]):
        samples = [row for row in rows if row["owner"] == owner]
        summary = {"owner": owner, "clients": samples[0]["clients"],
                   "operations_per_repeat": samples[0]["completed_operations"]}
        for key in ("owner_window_ops_per_second", "p99_latency_ns", "completion_window_seconds"):
            values = [row[key] for row in samples]
            summary[key] = {"median": statistics.median(values), "minimum": min(values), "maximum": max(values)}
        summaries.append(summary)
    return {"schema_version": 1, "scope": SCOPE, "source": path.name,
            "metadata": run["metadata"], "result": run["results"][0],
            "owners": summaries, "owner_samples": rows,
            "limits": ["finite closed-loop workload; offered rate is not held constant",
                       "cold clients may finish before hot clients; completion windows are reported",
                       "uncorrected client-observed latency; no coordinated-omission correction",
                       "many distinct keys per owner; single-hot-key contention is not measured",
                       "same-process loopback; no physical-core capacity or release claim"]}


def check_binary(binary: Path) -> None:
    common = [str(binary.resolve()), "--ops", "103", "--workers", "2", "--clients", "10",
              "--pipeline", "4", "--warmup", "0", "--repeats", "2", "--latency"]
    cases = [[], ["--hot-worker-clients", "9"],
             ["--hot-worker-clients", "9", "--hot-worker", "1", "--workload", "get-only"],
             ["--workers", "4", "--clients", "12", "--hot-worker-clients", "9",
              "--hot-worker", "2", "--workload", "read-95-write-5"],
             ["--clients", "20", "--hot-worker-clients", "1", "--workload", "read-95-write-5"]]
    with tempfile.TemporaryDirectory(prefix="glifi-owner-contract-") as temporary:
        for index, arguments in enumerate(cases):
            result = subprocess.run(common + ["--owner-profile"] + arguments, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise ValueError(f"owner contract case {arguments} failed: {result.stderr}\n{result.stdout}")
            path = Path(temporary) / f"case-{index}.txt"
            path.write_text(result.stdout)
            read_profile(path)
    for arguments in [["--hot-worker-clients", "0"], ["--hot-worker-clients", "10"],
                      ["--hot-worker-clients", "9", "--workers", "4"],
                      ["--hot-worker", "1"], ["--owner-profile", "--client-api"],
                      ["--owner-profile", "--ops", "3"], ["--hot-worker-clients", "1x"]]:
        result = subprocess.run(common + arguments, capture_output=True, text=True, timeout=10)
        if result.returncode != 2:
            raise ValueError(f"invalid owner arguments were not rejected: {arguments}: {result.returncode}")
    print("owner capacity contracts OK (5 workload profiles, 7 invalid inputs)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check-binary")
    check.add_argument("binary", type=Path)
    report = commands.add_parser("report")
    report.add_argument("inputs", nargs="+", type=Path)
    report.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "check-binary":
            check_binary(args.binary)
        else:
            data = {"schema_version": 1, "runs": [read_profile(path) for path in args.inputs]}
            args.output.write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"owner capacity error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
