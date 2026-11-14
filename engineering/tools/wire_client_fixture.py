#!/usr/bin/env python3
"""Validate and extract a sealed compiled wire-protocol client fixture."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tarfile
from pathlib import Path, PurePosixPath


ARCHIVE = re.compile(
    r"^glifistore-wire-v([0-9]+)-client-"
    r"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))-"
    r"([a-z0-9_-]+)-([a-z0-9_-]+)\.tar\.xz$"
)
HEX64 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")


class WireClientFixtureError(RuntimeError):
    pass


def validate(
    archive_path: Path,
    expected_version: str,
    expected_git_sha: str,
    expected_wire_version: int,
    extract_to: Path,
) -> Path:
    if archive_path.is_symlink() or not archive_path.is_file():
        raise WireClientFixtureError("wire client archive must be a regular file")
    match = ARCHIVE.fullmatch(archive_path.name)
    if match is None:
        raise WireClientFixtureError("wire client archive name is invalid")
    wire_version, product_version, target_os, architecture = match.groups()
    if int(wire_version) != expected_wire_version or product_version != expected_version:
        raise WireClientFixtureError("wire client archive identity differs from release")
    basename = archive_path.name.removesuffix(".tar.xz")
    binary_name = f"glifistore-wire-v{wire_version}-client"
    expected_names = {
        basename,
        f"{basename}/{binary_name}",
        f"{basename}/WIRE-CLIENT.json",
    }
    try:
        with tarfile.open(archive_path, "r:xz") as archive:
            members = archive.getmembers()
            names = {member.name.rstrip("/") for member in members}
            if names != expected_names or len(members) != len(expected_names):
                raise WireClientFixtureError("wire client archive member set is not exact")
            root = next(member for member in members if member.name.rstrip("/") == basename)
            binary = next(member for member in members if member.name.endswith(f"/{binary_name}"))
            metadata_member = next(
                member for member in members if member.name.endswith("/WIRE-CLIENT.json")
            )
            if not root.isdir() or not binary.isfile() or not metadata_member.isfile():
                raise WireClientFixtureError("wire client archive member types are invalid")
            for member in members:
                path = PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts or member.issym() or member.islnk():
                    raise WireClientFixtureError("wire client archive contains an unsafe member")
            if binary.size <= 0 or binary.size > 256 * 1024 * 1024 or binary.mode & 0o111 == 0:
                raise WireClientFixtureError("wire client binary size or mode is invalid")
            metadata_stream = archive.extractfile(metadata_member)
            binary_stream = archive.extractfile(binary)
            if metadata_stream is None or binary_stream is None:
                raise WireClientFixtureError("wire client archive payload is unreadable")
            metadata_bytes = metadata_stream.read(64 * 1024 + 1)
            if len(metadata_bytes) > 64 * 1024:
                raise WireClientFixtureError("wire client metadata is too large")
            payload = binary_stream.read(256 * 1024 * 1024 + 1)
            if len(payload) != binary.size:
                raise WireClientFixtureError("wire client binary extent is invalid")
    except (OSError, tarfile.TarError) as error:
        raise WireClientFixtureError(f"cannot read wire client archive: {error}") from error
    try:
        metadata = json.loads(metadata_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise WireClientFixtureError(f"wire client metadata is invalid: {error}") from error
    expected_fields = {
        "schema_version",
        "product_version",
        "wire_version",
        "git_sha",
        "tag",
        "target_os",
        "architecture",
        "client_sha256",
    }
    if not isinstance(metadata, dict) or set(metadata) != expected_fields:
        raise WireClientFixtureError("wire client metadata fields are not exact")
    string_fields = expected_fields - {"schema_version", "wire_version"}
    if (
        metadata.get("schema_version") != 1
        or not isinstance(metadata.get("wire_version"), int)
        or isinstance(metadata.get("wire_version"), bool)
        or any(not isinstance(metadata.get(field), str) for field in string_fields)
    ):
        raise WireClientFixtureError("wire client metadata field types are invalid")
    digest = hashlib.sha256(payload).hexdigest()
    if (
        metadata["product_version"] != expected_version
        or metadata["tag"] != f"v{expected_version}"
        or metadata["git_sha"] != expected_git_sha
        or GIT_SHA.fullmatch(metadata["git_sha"]) is None
        or metadata["wire_version"] != expected_wire_version
        or metadata["target_os"] != target_os
        or metadata["architecture"] != architecture
        or metadata["client_sha256"] != digest
        or HEX64.fullmatch(metadata["client_sha256"]) is None
    ):
        raise WireClientFixtureError("wire client metadata disagrees with payload or release")
    if extract_to.exists() or extract_to.is_symlink():
        raise WireClientFixtureError("wire client extraction destination already exists")
    extract_to.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(extract_to, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o755)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
    except BaseException:
        extract_to.unlink(missing_ok=True)
        raise
    print(extract_to)
    return extract_to


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-git-sha", required=True)
    parser.add_argument("--expected-wire-version", type=int, required=True)
    parser.add_argument("--extract-to", type=Path, required=True)
    args = parser.parse_args()
    try:
        validate(
            args.archive,
            args.expected_version,
            args.expected_git_sha,
            args.expected_wire_version,
            args.extract_to,
        )
    except (KeyError, OSError, TypeError, ValueError, WireClientFixtureError) as error:
        print(f"wire client fixture FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
