#!/usr/bin/env python3
"""Apply or check one pinned style for all repository-owned native sources."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


SOURCE_ROOTS = (
    "include",
    "src",
    "tests",
    "tools",
    "benchmarks",
    "fuzz",
    "scripts/probes",
    "packaging/common/consumer",
)
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl"})


@dataclass(frozen=True)
class FormatReport:
    files: int
    changed: tuple[Path, ...]
    errors: tuple[str, ...]


def source_files(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z", "--", *SOURCE_ROOTS],
        cwd=root,
        check=True,
        capture_output=True,
    )
    paths = {Path(os.fsdecode(name)) for name in result.stdout.split(b"\0") if name}
    sources = []
    for relative in sorted(paths):
        path = root / relative
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()):
            raise ValueError(f"native source must be a regular repository-owned file: {relative}")
        if path.is_file():
            sources.append(relative)
    return sources


def pinned_version(root: Path) -> str:
    version = (root / ".clang-format-version").read_text(encoding="ascii").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise ValueError(".clang-format-version must contain one major.minor.patch version")
    return version


def verify_formatter(executable: str, expected: str) -> None:
    result = subprocess.run([executable, "--version"], check=True, capture_output=True, text=True)
    match = re.search(r"\bversion\s+(\d+\.\d+\.\d+)\b", result.stdout)
    if match is None or match[1] != expected:
        actual = result.stdout.strip() or "unknown version"
        raise ValueError(f"expected clang-format {expected}; got {actual}")


def format_sources(root: Path, executable: str, *, write: bool) -> FormatReport:
    verify_formatter(executable, pinned_version(root))
    sources = source_files(root)
    if not sources:
        raise ValueError("no native source files selected; refusing a vacuous formatting pass")
    changed = []
    errors = []
    for relative in sources:
        path = root / relative
        before = path.read_bytes()
        result = subprocess.run(
            [
                executable,
                f"--style=file:{root / '.clang-format'}",
                f"--assume-filename={relative}",
                "--fail-on-incomplete-format",
                "--Werror",
            ],
            cwd=root,
            input=before,
            capture_output=True,
        )
        if result.returncode != 0:
            errors.append(f"{relative}: {result.stderr.decode(errors='replace').strip()}")
            continue
        if result.stdout == before:
            continue
        if write:
            # Preserve a newer editor/agent save instead of replacing a stale snapshot.
            if path.read_bytes() != before:
                errors.append(f"{relative}: changed during formatting; retry against the current file")
                continue
            path.write_bytes(result.stdout)
        changed.append(relative)
    if source_files(root) != sources:
        errors.append("native source inventory changed during formatting; rerun the check")
    return FormatReport(len(sources), tuple(changed), tuple(errors))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true", help="report drift without editing files")
    mode.add_argument("--write", action="store_true", help="format repository-owned native files")
    parser.add_argument("--clang-format", help="explicit path to the pinned clang-format executable")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    local = root / ".tools/venv/bin/clang-format"
    selected = args.clang_format or os.environ.get("CLANG_FORMAT") or (
        str(local) if local.is_file() else "clang-format"
    )
    executable = shutil.which(selected)
    if executable is None:
        print(f"Formatting FAILED: executable not found: {selected}", file=sys.stderr)
        return 1
    try:
        report = format_sources(root, str(Path(executable).resolve()), write=args.write)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Formatting FAILED: {error}", file=sys.stderr)
        return 1
    for error in report.errors:
        print(error, file=sys.stderr)
    if args.check:
        for path in report.changed:
            print(f"{path}: formatting differs; run python3 engineering/tools/format_code.py --write")
    if report.errors or (args.check and report.changed):
        print(
            f"Formatting FAILED ({report.files} files, "
            f"{len(report.changed)} differ, {len(report.errors)} errors)"
        )
        return 1
    print(
        f"Formatting OK ({report.files} files, {len(report.changed)} updated; "
        f"clang-format {pinned_version(root)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
