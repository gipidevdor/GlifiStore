#!/usr/bin/env python3
"""Fail closed on high-signal clang-tidy diagnostics in compiled production sources."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess


HIGH_SIGNAL_CHECKS = (
    "bugprone-unchecked-optional-access",
    "bugprone-use-after-move",
    "clang-analyzer-deadcode.DeadStores",
    "readability-inconsistent-declaration-parameter-name",
    "readability-braces-around-statements",
    "readability-duplicate-include",
    "modernize-use-nullptr",
    "modernize-use-override",
)


@dataclass(frozen=True)
class ClangTidyFailure:
    source: Path
    output: str


@dataclass(frozen=True)
class ClangTidyReport:
    sources: int
    failures: tuple[ClangTidyFailure, ...]


def production_sources(root: Path, build_directory: Path) -> list[Path]:
    root = root.resolve()
    database = build_directory / "compile_commands.json"
    commands = json.loads(database.read_text(encoding="utf-8"))
    sources: set[Path] = set()
    for entry in commands:
        raw = Path(entry["file"])
        source = (raw if raw.is_absolute() else Path(entry.get("directory", root)) / raw).resolve()
        try:
            relative = source.relative_to(root)
        except ValueError:
            continue
        if relative.parts and relative.parts[0] == "src" and source.suffix in {".cpp", ".cc", ".cxx"}:
            sources.add(source)
    return sorted(sources)


def clang_tidy_environment() -> dict[str, str]:
    environment = os.environ.copy()
    if platform.system() != "Darwin" or environment.get("SDKROOT"):
        return environment
    xcrun = shutil.which("xcrun")
    if xcrun is None:
        return environment
    completed = subprocess.run(
        [xcrun, "--show-sdk-path"], text=True, capture_output=True, check=False
    )
    sdk_root = completed.stdout.strip()
    if completed.returncode == 0 and sdk_root:
        environment["SDKROOT"] = sdk_root
    return environment


def run_gate(root: Path, build_directory: Path, clang_tidy: str, jobs: int) -> ClangTidyReport:
    root = root.resolve()
    build_directory = build_directory.resolve()
    sources = production_sources(root, build_directory)
    checks = "-*," + ",".join(HIGH_SIGNAL_CHECKS)
    environment = clang_tidy_environment()

    def inspect(source: Path) -> ClangTidyFailure | None:
        completed = subprocess.run(
            [
                clang_tidy,
                "-p",
                str(build_directory),
                "--quiet",
                f"--checks={checks}",
                "--warnings-as-errors=*",
                str(source),
            ],
            cwd=root,
            text=True,
            capture_output=True,
            env=environment,
        )
        if completed.returncode == 0:
            return None
        output = "\n".join(part.rstrip() for part in (completed.stdout, completed.stderr) if part.strip())
        return ClangTidyFailure(source.relative_to(root), output)

    with ThreadPoolExecutor(max_workers=jobs) as executor:
        failures = tuple(failure for failure in executor.map(inspect, sources) if failure is not None)
    return ClangTidyReport(len(sources), failures)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 4))
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be greater than zero")

    root = Path(__file__).resolve().parents[2]
    build_directory = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    executable = shutil.which(args.clang_tidy)
    if executable is None:
        print(f"clang-tidy gate FAILED: executable not found: {args.clang_tidy}")
        return 1
    try:
        report = run_gate(root, build_directory, executable, args.jobs)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"clang-tidy gate FAILED: {error}")
        return 1

    if report.sources == 0:
        print("clang-tidy gate FAILED: compile database contains no production sources")
        return 1
    if report.failures:
        for failure in report.failures:
            print(f"== {failure.source} ==")
            print(failure.output)
        print(f"clang-tidy gate FAILED ({len(report.failures)}/{report.sources} production sources)")
        return 1
    print(
        f"clang-tidy high-signal gate OK ({report.sources} production sources; "
        f"{len(HIGH_SIGNAL_CHECKS)} fail-closed checks)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
