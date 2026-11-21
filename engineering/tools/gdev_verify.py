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
    parser.add_argument("check", choices=("metadata", "format", "native"))
    args = parser.parse_args()
    python = ROOT / ".tools/venv/bin/python3"
    interpreter = str(python) if python.is_file() else sys.executable
    commands = {
        "metadata": [[interpreter, "engineering/tools/validate_assurance.py"],
                     [interpreter, "engineering/tools/export_gdev_catalog.py", "--check"],
                     [interpreter, "-m", "unittest", "discover", "-s", "engineering/tests", "-p", "test_gdev*.py"]],
        "format": [["./scripts/dev.sh", "format-check"]],
        "native": [["./scripts/dev.sh", "test"]],
    }
    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    for command in commands[args.check]:
        result = subprocess.run(command, cwd=ROOT, env=environment, check=False)
        if result.returncode:
            return result.returncode if result.returncode > 0 else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
