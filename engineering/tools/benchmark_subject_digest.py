#!/usr/bin/env python3
"""Hash the tracked inputs that can change the hosted benchmark executables."""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
from pathlib import Path, PurePosixPath
from typing import Iterable


SUBJECT_PATHS = (
    ".github/workflows/benchmarks.yml",
    "CMakeLists.txt",
    "CMakePresets.json",
    "VERSION",
    "cmake",
    "include",
    "src",
    "benchmarks",
)
SUBJECT_EXCLUDES = (":(exclude)benchmarks/results/**",)
FORMAT_DOMAIN = b"glifistore-hosted-benchmark-subject-v1\0"


def tracked_subject_paths(root: Path) -> list[str]:
    completed = subprocess.run(
        [
            "git",
            "-C",
            os.fspath(root),
            "ls-files",
            "-z",
            "--",
            *SUBJECT_PATHS,
            *SUBJECT_EXCLUDES,
        ],
        check=True,
        stdout=subprocess.PIPE,
    )
    paths = [os.fsdecode(entry) for entry in completed.stdout.split(b"\0") if entry]
    if not paths:
        raise ValueError("benchmark subject contains no tracked files")
    if len(paths) != len(set(paths)):
        raise ValueError("benchmark subject contains duplicate paths")
    return paths


def subject_digest(root: Path, relative_paths: Iterable[str]) -> str:
    resolved_root = root.resolve(strict=True)
    normalized: list[tuple[bytes, Path]] = []
    seen: set[str] = set()
    for value in relative_paths:
        logical = PurePosixPath(value)
        if logical.is_absolute() or not logical.parts or ".." in logical.parts:
            raise ValueError(f"invalid benchmark subject path: {value!r}")
        relative = logical.as_posix()
        if relative in seen:
            raise ValueError(f"duplicate benchmark subject path: {relative}")
        seen.add(relative)
        path = resolved_root.joinpath(*logical.parts)
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"benchmark subject path is not a regular file: {relative}")
        resolved = path.resolve(strict=True)
        if not resolved.is_relative_to(resolved_root):
            raise ValueError(f"benchmark subject path escapes repository: {relative}")
        normalized.append((os.fsencode(relative), resolved))

    normalized.sort(key=lambda entry: entry[0])
    if not normalized:
        raise ValueError("benchmark subject contains no files")

    digest = hashlib.sha256(FORMAT_DOMAIN)
    for encoded_path, path in normalized:
        payload = path.read_bytes()
        executable = 1 if path.stat().st_mode & 0o111 else 0
        digest.update(len(encoded_path).to_bytes(8, "little"))
        digest.update(encoded_path)
        digest.update(executable.to_bytes(1, "little"))
        digest.update(len(payload).to_bytes(8, "little"))
        digest.update(payload)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root (defaults to the checkout containing this tool).",
    )
    args = parser.parse_args()
    print(subject_digest(args.root, tracked_subject_paths(args.root)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
