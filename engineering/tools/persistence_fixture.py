#!/usr/bin/env python3
"""Create, validate, and select immutable tagged-release Store fixtures."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any


METADATA_NAME = "STORE-FIXTURE.json"
CHECKSUM_NAME = "SHA256SUMS"
STORE_DIRECTORY = "store"
HEX64 = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA = re.compile(r"^[0-9a-f]{40}$")
LABEL = re.compile(r"^[0-9][0-9A-Za-z._+-]*$")
SEMVER = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
HEX_BYTES = re.compile(r"^(?:[0-9a-f]{2})+$")
UTC_SECONDS = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
CHECKSUM_LINE = re.compile(r"^([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+/-]*)$")

FIELDS = {
    "schema_version",
    "baseline_kind",
    "label",
    "product_version",
    "tag",
    "git_sha",
    "producer_artifact_sha256",
    "persistence_format",
    "worker_count",
    "key_hex",
    "value_hex",
    "store_directory",
    "packaged_at",
}


class FixtureError(RuntimeError):
    pass


def _digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def _semver(value: str) -> tuple[int, int, int]:
    match = SEMVER.fullmatch(value)
    if match is None:
        raise FixtureError(f"invalid semantic version: {value!r}")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def _git(repository: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise FixtureError(f"cannot inspect fixture tag provenance: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or "git command failed"
        raise FixtureError(f"cannot inspect fixture tag provenance: {detail}")
    return result.stdout.strip()


def _verify_tag(repository: Path, metadata: dict[str, Any]) -> None:
    if repository.is_symlink() or not repository.is_dir():
        raise FixtureError(f"fixture provenance repository is invalid: {repository}")
    repository = repository.resolve()
    tag = metadata["tag"]
    reference = f"refs/tags/{tag}"
    if _git(repository, "cat-file", "-t", reference) != "tag":
        raise FixtureError(f"fixture provenance tag is not annotated: {tag}")
    commit = _git(repository, "rev-parse", f"{reference}^{{commit}}")
    if commit != metadata["git_sha"]:
        raise FixtureError(
            f"fixture git_sha does not match annotated tag {tag}: {commit}"
        )
    tagged_version = _git(repository, "show", f"{reference}^{{commit}}:VERSION")
    if tagged_version != metadata["product_version"]:
        raise FixtureError(
            f"fixture version does not match VERSION at annotated tag {tag}"
        )


def _safe_relative(name: str) -> bool:
    path = PurePosixPath(name)
    return (
        not path.is_absolute()
        and ".." not in path.parts
        and "." not in path.parts
        and len(path.parts) >= 1
        and all(part not in {"", ".", ".."} for part in path.parts)
    )


def _regular_files(root: Path, *, exclude_checksum: bool) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for directory, directory_names, file_names in os.walk(root, followlinks=False):
        directory_path = Path(directory)
        for name in list(directory_names):
            path = directory_path / name
            if path.is_symlink():
                raise FixtureError(f"fixture contains a symlinked directory: {path}")
        for name in file_names:
            path = directory_path / name
            if path.is_symlink() or not path.is_file():
                raise FixtureError(f"fixture member is not a regular file: {path}")
            relative = path.relative_to(root).as_posix()
            if not _safe_relative(relative):
                raise FixtureError(f"unsafe fixture member path: {relative!r}")
            if exclude_checksum and relative == CHECKSUM_NAME:
                continue
            files[relative] = path
    return files


def _read_metadata(root: Path) -> dict[str, Any]:
    path = root / METADATA_NAME
    if path.is_symlink() or not path.is_file():
        raise FixtureError(f"missing regular {METADATA_NAME}: {root}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FixtureError(f"invalid {METADATA_NAME}: {error}") from error
    if not isinstance(value, dict):
        raise FixtureError(f"{METADATA_NAME} root must be an object")
    if set(value) != FIELDS:
        raise FixtureError(
            f"{METADATA_NAME} fields differ: missing={sorted(FIELDS - set(value))}, "
            f"unexpected={sorted(set(value) - FIELDS)}"
        )
    return value


def validate(
    root: Path,
    *,
    before_version: str | None = None,
    repository: Path | None = None,
    require_directory_label: bool = True,
) -> dict[str, Any]:
    if root.is_symlink() or not root.is_dir():
        raise FixtureError(f"fixture root must be a directory, not a symlink: {root}")
    root = root.resolve()
    metadata = _read_metadata(root)

    if metadata["schema_version"] != 1:
        raise FixtureError("fixture schema_version must be 1")
    if metadata["baseline_kind"] != "tagged-release":
        raise FixtureError("fixture baseline_kind must be tagged-release")
    label = metadata["label"]
    if not isinstance(label, str) or LABEL.fullmatch(label) is None or ".." in label:
        raise FixtureError("fixture label is unsafe")
    if require_directory_label and root.name != label:
        raise FixtureError(f"fixture label {label!r} does not match directory {root.name!r}")
    version = metadata["product_version"]
    if not isinstance(version, str):
        raise FixtureError("fixture product_version must be a string")
    parsed_version = _semver(version)
    if label != version:
        raise FixtureError("fixture label must equal product_version")
    if metadata["tag"] != f"v{version}":
        raise FixtureError("fixture tag must equal v<product_version>")
    if before_version is not None and parsed_version >= _semver(before_version):
        raise FixtureError(
            f"fixture {version} is not older than candidate version {before_version}"
        )
    if not isinstance(metadata["git_sha"], str) or GIT_SHA.fullmatch(metadata["git_sha"]) is None:
        raise FixtureError("fixture git_sha must be a 40-character lowercase object id")
    if repository is not None:
        _verify_tag(repository, metadata)
    producer_digest = metadata["producer_artifact_sha256"]
    if not isinstance(producer_digest, str) or HEX64.fullmatch(producer_digest) is None:
        raise FixtureError("fixture producer_artifact_sha256 must be lowercase SHA-256")
    if metadata["persistence_format"] != 1:
        raise FixtureError("fixture persistence_format must be 1")
    worker_count = metadata["worker_count"]
    if not isinstance(worker_count, int) or isinstance(worker_count, bool) or not 1 <= worker_count <= 256:
        raise FixtureError("fixture worker_count must be an integer in 1..256")
    for field in ("key_hex", "value_hex"):
        value = metadata[field]
        if not isinstance(value, str) or HEX_BYTES.fullmatch(value) is None:
            raise FixtureError(f"fixture {field} must be non-empty lowercase whole-byte hex")
    if len(metadata["key_hex"]) > 4 * 1024 * 1024 or len(metadata["value_hex"]) > 8 * 1024 * 1024:
        raise FixtureError("fixture probe key/value exceeds bounded metadata limits")
    if metadata["store_directory"] != STORE_DIRECTORY:
        raise FixtureError(f"fixture store_directory must be {STORE_DIRECTORY!r}")
    packaged_at = metadata["packaged_at"]
    if not isinstance(packaged_at, str) or UTC_SECONDS.fullmatch(packaged_at) is None:
        raise FixtureError("fixture packaged_at must be UTC ISO-8601 seconds")
    try:
        dt.datetime.strptime(packaged_at, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as error:
        raise FixtureError("fixture packaged_at is not a real UTC timestamp") from error

    store = root / STORE_DIRECTORY
    if store.is_symlink() or not store.is_dir():
        raise FixtureError("fixture store directory is missing or symlinked")
    files = _regular_files(root, exclude_checksum=True)
    store_files = [name for name in files if name.startswith(f"{STORE_DIRECTORY}/")]
    if not store_files:
        raise FixtureError("fixture store directory contains no files")
    if f"{STORE_DIRECTORY}/manifest.glifi" not in files:
        raise FixtureError("fixture store has no manifest.glifi")

    sums = root / CHECKSUM_NAME
    if sums.is_symlink() or not sums.is_file():
        raise FixtureError(f"missing regular {CHECKSUM_NAME}")
    recorded: dict[str, str] = {}
    for line in sums.read_text(encoding="utf-8").splitlines():
        match = CHECKSUM_LINE.fullmatch(line)
        if match is None:
            raise FixtureError(f"malformed checksum line: {line!r}")
        digest, name = match.groups()
        if not _safe_relative(name):
            raise FixtureError(f"unsafe checksum path: {name!r}")
        if name in recorded:
            raise FixtureError(f"duplicate checksum path: {name}")
        recorded[name] = digest
    if set(recorded) != set(files):
        raise FixtureError(
            f"checksum inventory differs: missing={sorted(set(files) - set(recorded))}, "
            f"unexpected={sorted(set(recorded) - set(files))}"
        )
    for name, path in files.items():
        if _digest(path) != recorded[name]:
            raise FixtureError(f"checksum mismatch: {name}")
    return metadata


def create(args: argparse.Namespace) -> None:
    if args.source.is_symlink() or not args.source.is_dir():
        raise FixtureError("source Store must be a regular directory")
    source = args.source.resolve()
    output = args.output.absolute()
    if output.exists() or output.is_symlink():
        raise FixtureError(f"output already exists: {output}")
    source_files = _regular_files(source, exclude_checksum=False)
    if not source_files or "manifest.glifi" not in source_files:
        raise FixtureError("source Store has no manifest.glifi")
    version = args.product_version
    _semver(version)
    if output.name != version:
        raise FixtureError("output directory name must equal --product-version")
    if args.tag != f"v{version}":
        raise FixtureError("--tag must equal v<product-version>")
    if GIT_SHA.fullmatch(args.git_sha) is None:
        raise FixtureError("--git-sha must be a 40-character lowercase object id")
    producer_artifact = args.producer_artifact
    if producer_artifact.is_symlink() or not producer_artifact.is_file():
        raise FixtureError("--producer-artifact must be a regular release artifact")
    producer_artifact = producer_artifact.resolve()
    for name, value in (("--key-hex", args.key_hex), ("--value-hex", args.value_hex)):
        if HEX_BYTES.fullmatch(value) is None:
            raise FixtureError(f"{name} must be non-empty lowercase whole-byte hex")
    packaged_at = args.packaged_at
    if UTC_SECONDS.fullmatch(packaged_at) is None:
        raise FixtureError("--packaged-at must be UTC ISO-8601 seconds")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{version}.", dir=output.parent))
    try:
        shutil.copytree(source, temporary / STORE_DIRECTORY, symlinks=False)
        metadata = {
            "schema_version": 1,
            "baseline_kind": "tagged-release",
            "label": version,
            "product_version": version,
            "tag": args.tag,
            "git_sha": args.git_sha,
            "producer_artifact_sha256": _digest(producer_artifact),
            "persistence_format": 1,
            "worker_count": args.worker_count,
            "key_hex": args.key_hex,
            "value_hex": args.value_hex,
            "store_directory": STORE_DIRECTORY,
            "packaged_at": packaged_at,
        }
        _verify_tag(args.repository, metadata)
        (temporary / METADATA_NAME).write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        files = _regular_files(temporary, exclude_checksum=True)
        (temporary / CHECKSUM_NAME).write_text(
            "".join(f"{_digest(path)}  {name}\n" for name, path in sorted(files.items())),
            encoding="utf-8",
        )
        validate(temporary, require_directory_label=False)
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    print(output)


def select(directory: Path, before_version: str, repository: Path) -> Path:
    if directory.is_symlink() or not directory.is_dir():
        raise FixtureError(f"released Store fixture directory is missing: {directory}")
    directory = directory.resolve()
    candidates: list[tuple[tuple[int, int, int], Path]] = []
    errors: list[str] = []
    for child in sorted(directory.iterdir()):
        if not child.is_dir() or child.is_symlink():
            continue
        try:
            metadata = validate(
                child, before_version=before_version, repository=repository
            )
            candidates.append((_semver(metadata["product_version"]), child.resolve()))
        except FixtureError as error:
            errors.append(f"{child.name}: {error}")
    if errors:
        raise FixtureError("invalid released Store fixture(s): " + "; ".join(errors))
    if not candidates:
        detail = "; ".join(errors) if errors else "no versioned fixture directories found"
        raise FixtureError(
            f"no valid tagged-release Store fixture older than {before_version}: {detail}"
        )
    candidates.sort(key=lambda item: item[0])
    selected = candidates[-1][1]
    print(selected)
    return selected


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validation = subparsers.add_parser("validate")
    validation.add_argument("fixture", type=Path)
    validation.add_argument("--before-version")
    validation.add_argument("--repository", type=Path, required=True)

    selection = subparsers.add_parser("select")
    selection.add_argument("--directory", type=Path, required=True)
    selection.add_argument("--before-version", required=True)
    selection.add_argument("--repository", type=Path, required=True)

    creation = subparsers.add_parser("create")
    creation.add_argument("--source", type=Path, required=True)
    creation.add_argument("--output", type=Path, required=True)
    creation.add_argument("--product-version", required=True)
    creation.add_argument("--tag", required=True)
    creation.add_argument("--git-sha", required=True)
    creation.add_argument("--producer-artifact", type=Path, required=True)
    creation.add_argument("--worker-count", type=int, choices=range(1, 257), required=True)
    creation.add_argument("--key-hex", required=True)
    creation.add_argument("--value-hex", required=True)
    creation.add_argument("--packaged-at", required=True)
    creation.add_argument("--repository", type=Path, required=True)
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        if args.command == "validate":
            metadata = validate(
                args.fixture,
                before_version=args.before_version,
                repository=args.repository,
            )
            print(
                f"Tagged Store fixture OK ({metadata['product_version']}, "
                f"workers={metadata['worker_count']})"
            )
        elif args.command == "select":
            select(args.directory, args.before_version, args.repository)
        else:
            create(args)
    except FixtureError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
