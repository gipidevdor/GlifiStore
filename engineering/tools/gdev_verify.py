#!/usr/bin/env python3
"""Run the existing project checks from a GDev procedure, without a shell string."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("check", choices=("metadata", "format", "native", "capacity", "read-capacity"))
    args = parser.parse_args()
    python = ROOT / ".tools/venv/bin/python3"
    interpreter = str(python) if python.is_file() else sys.executable
    family = os.environ.get("GLIFISTORE_PRESET_FAMILY", "macos" if sys.platform == "darwin" else "unix")
    if family not in ("macos", "unix"):
        parser.error("GLIFISTORE_PRESET_FAMILY must be macos or unix")
    preset = f"{family}-release"
    tools = ROOT / ".tools/venv/bin"
    cmake = str(tools / "cmake") if (tools / "cmake").is_file() else "cmake"
    ctest = str(tools / "ctest") if (tools / "ctest").is_file() else "ctest"
    commands = {
        "metadata": [[interpreter, "engineering/tools/validate_assurance.py"],
                     [interpreter, "engineering/tools/export_gdev_catalog.py", "--check"],
                     [interpreter, "-m", "unittest", "discover", "-s", "engineering/tests", "-p", "test_gdev*.py"]],
        "format": [["./scripts/dev.sh", "format-check"]],
        "native": [["./scripts/dev.sh", "test"]],
        "read-capacity": [[interpreter, "engineering/tools/gdev_verify.py", "metadata"],
                          [interpreter, "-m", "unittest", "scripts.tests.test_benchmark_read_capacity"],
                          [cmake, "--preset", preset],
                          [cmake, "--build", "--preset", preset, "--target", "glifistore_tests", "glifistored"],
                          [ctest, "--test-dir", f"build/{preset}", "-R", "glifistore_read_", "--output-on-failure"]],
        "capacity": [[interpreter, "engineering/tools/gdev_verify.py", "metadata"],
                     [interpreter, "-m", "unittest", "scripts.tests.test_owner_capacity_report", "scripts.tests.test_benchmark_report"],
                     [cmake, "--preset", preset],
                     [cmake, "--build", "--preset", preset, "--target", "glifistore_server_benchmarks", "glifistore_tests"],
                     [ctest, "--test-dir", f"build/{preset}", "-R", "glifistore_benchmark_(owner_|server_tcp)", "--output-on-failure"]],
    }
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    if tools.is_dir():
        environment["PATH"] = str(tools) + os.pathsep + environment.get("PATH", "")
    for command in commands[args.check]:
        result = subprocess.run(command, cwd=ROOT, env=environment, check=False)
        if result.returncode:
            return result.returncode if result.returncode > 0 else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
