#!/usr/bin/env python3
"""Fail unless a GlifiStore shared library exports exactly the C ABI allowlist."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def parse_names(text: str, *, darwin: bool) -> set[str]:
    names: set[str] = set()
    for line in text.splitlines():
        fields = line.split()
        if not fields:
            continue
        name = fields[-1]
        if darwin and name.startswith("_"):
            name = name[1:]
        if name.startswith("gs_"):
            names.add(name)
    return names


def inspect_library(path: Path) -> set[str]:
    darwin = sys.platform == "darwin"
    commands = [["nm", "-gU", str(path)]] if darwin else [
        ["nm", "-D", "--defined-only", str(path)],
        ["nm", "-g", str(path)],
    ]
    last_error = ""
    for command in commands:
        completed = subprocess.run(command, text=True, capture_output=True, check=False)
        if completed.returncode == 0:
            return parse_names(completed.stdout, darwin=darwin)
        last_error = completed.stderr.strip()
    raise RuntimeError(f"cannot inspect {path}: {last_error}")


def load_lines(path: Path) -> set[str]:
    return {
        line.strip().removeprefix("_")
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=Path)
    parser.add_argument("--actual-list", type=Path)
    parser.add_argument("--allowlist", type=Path, required=True)
    args = parser.parse_args()
    if (args.library is None) == (args.actual_list is None):
        parser.error("pass exactly one of --library or --actual-list")

    expected = load_lines(args.allowlist)
    actual = inspect_library(args.library) if args.library else load_lines(args.actual_list)
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        if missing:
            print("missing ABI symbols: " + ", ".join(missing), file=sys.stderr)
        if unexpected:
            print("unexpected ABI symbols: " + ", ".join(unexpected), file=sys.stderr)
        return 1
    print(f"C ABI symbol set verified ({len(actual)} symbols)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
