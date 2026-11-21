#!/usr/bin/env python3
"""Retain a bounded local balanced/skewed loopback campaign, including failed attempts."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

if __package__:
    from .owner_capacity_report import read_profile
else:
    from owner_capacity_report import read_profile

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def observation(argv):
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=5, check=False)
        return result.stdout.strip() if not result.returncode else "unavailable"
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"


def environment(binary):
    paths = set()
    for folder in ("include", "src", "benchmarks", "cmake"):
        for path in (ROOT / folder).rglob("*"):
            if path.is_file() and (path.suffix in (".hpp", ".cpp", ".cmake") or path.name == "CMakeLists.txt"):
                paths.add(path)
    paths.update(ROOT / name for name in ("CMakeLists.txt", "CMakePresets.json",
                                         "scripts/benchmark_owner_capacity.py", "scripts/owner_capacity_report.py"))
    flags = {}
    cache = binary.parent / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS:", "CMAKE_CXX_FLAGS_RELEASE:")):
                key, value = line.split("=", 1)
                flags[key] = value
    return {"generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git_commit": observation(["git", "-C", str(ROOT), "rev-parse", "HEAD"]),
            "git_describe": observation(["git", "-C", str(ROOT), "describe", "--always", "--dirty"]),
            "system": platform.system(), "kernel": platform.release(), "architecture": platform.machine(),
            "logical_cpus": os.cpu_count(),
            "physical_cpus": observation(["sysctl", "-n", "hw.physicalcpu"]),
            "cpu_model": observation(["sysctl", "-n", "machdep.cpu.brand_string"]),
            "memory_bytes": observation(["sysctl", "-n", "hw.memsize"]),
            "memory_state": observation(["vm_stat"]),
            "power_state": observation(["pmset", "-g", "batt"]),
            "thermal_state": observation(["pmset", "-g", "therm"]),
            "control": "uncontrolled local exploratory run; no hardware acceptance",
            "storage": "volatile; no durable test stores", "build_flags": flags,
            "binary_sha256": sha256(binary),
            "source_sha256": {path.relative_to(ROOT).as_posix(): sha256(path) for path in sorted(paths)}}


def campaign(binary, output, operations=40000, warmup=1, repeats=7):
    binary, output = binary.resolve(), output.resolve()
    if not (1 <= operations <= 200000 and 0 <= warmup <= 3 and 1 <= repeats <= 21):
        raise ValueError("campaign bounds: 1..200000 operations, 0..3 warmups, 1..21 repeats")
    if not binary.is_file():
        raise ValueError("benchmark binary is missing")
    binary_digest = sha256(binary)
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"schema_version": 1, "environment": environment(binary), "runs": [],
                "status": "RUNNING", "scope": "finite closed-loop; no saturation or isolation claim"}

    def save():
        (output / "campaign.json").write_text(json.dumps(manifest, indent=2, allow_nan=False) + "\n")

    save()
    for workload in ("get-only", "read-after-write"):
        for clients in (4, 10):
            for hot in (0, clients - 1):
                name = f"{workload}-c{clients}-hot{hot}"
                argv = [str(binary), "--workers", "2", "--clients", str(clients), "--ops", str(operations),
                        "--key-size", "16", "--value-size", "64", "--pipeline", "32", "--workload", workload,
                        "--owner-profile", "--latency", "--warmup", str(warmup), "--repeats", str(repeats)]
                if hot:
                    argv += ["--hot-worker-clients", str(hot)]
                record = {"name": name, "argv": [binary.name, *argv[1:]], "status": "RUNNING"}
                manifest["runs"].append(record)
                save()
                print(f"measuring {name}", flush=True)
                try:
                    if sha256(binary) != binary_digest:
                        raise ValueError("benchmark binary changed during the campaign; rebuild and start a new campaign")
                    result = subprocess.run(argv, capture_output=True, text=True, timeout=120, check=False)
                    raw = output / f"{name}.txt"
                    raw.write_text(result.stdout)
                    (output / f"{name}.stderr.txt").write_text(result.stderr)
                    record.update(returncode=result.returncode, raw_sha256=sha256(raw))
                    if sha256(binary) != binary_digest:
                        raise ValueError("benchmark binary changed during the campaign; raw output is not accepted")
                    if result.returncode:
                        raise ValueError(f"benchmark exited with {result.returncode}; raw output retained for {name}")
                    report = read_profile(raw)
                    (output / f"{name}.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
                    record["status"] = "SUCCEEDED"
                except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                    if isinstance(error, subprocess.TimeoutExpired):
                        for suffix, partial in (("txt", error.output), ("stderr.txt", error.stderr)):
                            text = partial.decode(errors="replace") if isinstance(partial, bytes) else partial or ""
                            (output / f"{name}.{suffix}").write_text(text)
                    record.update(status="FAILED", problem=str(error))
                    manifest["status"] = "FAILED"
                    save()
                    raise
                save()
    manifest["status"] = "SUCCEEDED"
    save()
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--operations", type=int, default=40000)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--repeats", type=int, default=7)
    args = parser.parse_args()
    try:
        campaign(args.binary, args.output, args.operations, args.warmup, args.repeats)
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"owner campaign error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
