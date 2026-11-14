#!/usr/bin/env python3
"""Compare the complete deterministic archive set from an independent release rebuild."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


SEMVER = re.compile(r"^(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)$")
IDENTIFIER = re.compile(r"^[A-Za-z0-9_-]+$")


class RebuildComparisonError(RuntimeError):
    pass


def read_metadata(path: Path) -> dict[str, object]:
    if path.is_symlink() or not path.is_file():
        raise RebuildComparisonError(f"build metadata is missing or unsafe: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RebuildComparisonError(f"build metadata is invalid: {path}: {error}") from error
    if not isinstance(value, dict):
        raise RebuildComparisonError(f"build metadata root is not an object: {path}")
    return value


def compare_build_authority(reference: Path, rebuilt: Path) -> None:
    reference_value = read_metadata(reference)
    rebuilt_value = read_metadata(rebuilt)
    authority_fields = (
        "schema_version",
        "product_version",
        "abi",
        "wire_version",
        "persistent_format_version",
        "source",
        "target",
        "toolchain",
        "build_options",
    )
    for field in authority_fields:
        if reference_value.get(field) != rebuilt_value.get(field):
            raise RebuildComparisonError(f"independent build authority differs for {field}")
    print("build authority matches for source, target, toolchain, versions, and options")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def byte_equal(left: Path, right: Path) -> bool:
    with left.open("rb") as left_stream, right.open("rb") as right_stream:
        while True:
            left_block = left_stream.read(1024 * 1024)
            right_block = right_stream.read(1024 * 1024)
            if left_block != right_block:
                return False
            if not left_block:
                return True


def expected_names(
    version: str,
    abi_major: int,
    wire_version: int,
    target_os: str,
    architecture: str,
) -> tuple[str, ...]:
    if SEMVER.fullmatch(version) is None:
        raise RebuildComparisonError("product version is invalid")
    if abi_major <= 0 or wire_version <= 0 or wire_version > 65535:
        raise RebuildComparisonError("ABI or wire version is invalid")
    if IDENTIFIER.fullmatch(target_os) is None or IDENTIFIER.fullmatch(architecture) is None:
        raise RebuildComparisonError("target OS or architecture is invalid")
    return tuple(
        sorted(
            (
                f"GlifiStore-{version}.tar.xz",
                f"glifistore-{version}-{target_os}-{architecture}.tar.xz",
                f"glifistore-abi-v{abi_major}-consumer-"
                f"{version}-{target_os}-{architecture}.tar.xz",
                f"glifistore-wire-v{wire_version}-client-"
                f"{version}-{target_os}-{architecture}.tar.xz",
            )
        )
    )


def archive_names(directory: Path) -> tuple[str, ...]:
    if directory.is_symlink() or not directory.is_dir():
        raise RebuildComparisonError(f"archive directory is missing or unsafe: {directory}")
    result: list[str] = []
    for path in directory.iterdir():
        if path.name.endswith(".tar.xz"):
            if path.is_symlink() or not path.is_file():
                raise RebuildComparisonError(f"archive is missing or unsafe: {path}")
            result.append(path.name)
    return tuple(sorted(result))


def compare(
    reference: Path,
    rebuilt: Path,
    version: str,
    abi_major: int,
    wire_version: int,
    target_os: str,
    architecture: str,
    reference_metadata: Path | None = None,
    rebuilt_metadata: Path | None = None,
) -> None:
    if (reference_metadata is None) != (rebuilt_metadata is None):
        raise RebuildComparisonError("both build metadata paths must be supplied together")
    if reference_metadata is not None and rebuilt_metadata is not None:
        compare_build_authority(reference_metadata, rebuilt_metadata)
    expected = expected_names(
        version, abi_major, wire_version, target_os, architecture
    )
    reference_names = archive_names(reference)
    rebuilt_names = archive_names(rebuilt)
    if reference_names != expected:
        raise RebuildComparisonError(
            f"reference archive set differs from policy: {reference_names} != {expected}"
        )
    if rebuilt_names != expected:
        raise RebuildComparisonError(
            f"rebuilt archive set differs from policy: {rebuilt_names} != {expected}"
        )
    for name in expected:
        reference_path = reference / name
        rebuilt_path = rebuilt / name
        reference_size = reference_path.stat().st_size
        rebuilt_size = rebuilt_path.stat().st_size
        reference_digest = digest(reference_path)
        rebuilt_digest = digest(rebuilt_path)
        identical_bytes = byte_equal(reference_path, rebuilt_path)
        print(
            f"{name} reference_size={reference_size} rebuilt_size={rebuilt_size} "
            f"reference_sha256={reference_digest} rebuilt_sha256={rebuilt_digest} "
            f"byte_equal={'yes' if identical_bytes else 'no'}"
        )
        if (
            reference_size != rebuilt_size
            or reference_digest != rebuilt_digest
            or not identical_bytes
        ):
            raise RebuildComparisonError(f"independent rebuild differs for {name}")
    print("REPRODUCIBILITY artifact-compare PASSED")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--rebuilt", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--abi-major", type=int, required=True)
    parser.add_argument("--wire-version", type=int, required=True)
    parser.add_argument("--target-os", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--reference-metadata", type=Path)
    parser.add_argument("--rebuilt-metadata", type=Path)
    args = parser.parse_args()
    try:
        compare(
            args.reference,
            args.rebuilt,
            args.version,
            args.abi_major,
            args.wire_version,
            args.target_os,
            args.architecture,
            args.reference_metadata,
            args.rebuilt_metadata,
        )
    except (OSError, RebuildComparisonError) as error:
        print(f"release rebuild comparison FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
