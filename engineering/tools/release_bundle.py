#!/usr/bin/env python3
"""Build, seal, and verify fail-closed GlifiStore release metadata."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

from release_identity import ReleaseIdentityError, validate_release_identity
from release_evidence import EVIDENCE_FILENAMES, EvidenceError, validate_evidence


HEX64 = re.compile(r"^[0-9a-f]{64}$")
GIT_OBJECT_ID = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")
CONTROL_FILES = {
    "SHA256SUMS",
    "candidate-admission.json",
    "candidate-seal.json",
    "evidence-import.json",
    "release-manifest.json",
    "verification.json",
    "verified-seal.json",
}
MANIFEST_EXCLUSIONS = {"SHA256SUMS", "release-manifest.json", "verified-seal.json"}


class BundleError(RuntimeError):
    pass


def artifact_product_version(name: str) -> str | None:
    match = re.match(
        r"^(?:GlifiStore|glifistore)-"
        r"(?:(?:abi-v[0-9]+-consumer|wire-v[0-9]+-client)-)?"
        r"((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))"
        r"(?:[.-]|$)",
        name,
    )
    return None if match is None else match.group(1)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def write_json(path: Path, value: dict[str, Any]) -> None:
    encoded = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    path.write_text(encoded, encoding="utf-8")


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BundleError(f"invalid JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise BundleError(f"JSON root must be an object: {path}")
    return value


def _utc_timestamp(value: object) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError:
        return False
    return parsed.tzinfo is not None and parsed.utcoffset() == dt.timedelta(0)


def release_files(directory: Path, *, excluding: set[str] | None = None) -> list[Path]:
    excluding = excluding or set()
    if not directory.is_dir():
        raise BundleError(f"release directory does not exist: {directory}")
    result: list[Path] = []
    for path in sorted(directory.iterdir(), key=lambda entry: entry.name):
        if path.name in excluding:
            continue
        if not SAFE_NAME.fullmatch(path.name):
            raise BundleError(f"unsafe release filename: {path.name!r}")
        if path.is_symlink() or not path.is_file():
            raise BundleError(f"release bundle accepts regular files only: {path.name}")
        result.append(path)
    return result


def _file_entries(directory: Path, excluding: set[str]) -> list[dict[str, Any]]:
    return [
        {"name": path.name, "sha256": digest(path), "size": path.stat().st_size}
        for path in release_files(directory, excluding=excluding)
    ]


def seal(directory: Path, seal_name: str, kind: str) -> Path:
    if not SAFE_NAME.fullmatch(seal_name) or not seal_name.endswith(".json"):
        raise BundleError("seal name must be a safe JSON filename")
    output = directory / seal_name
    value = {
        "schema_version": 1,
        "kind": kind,
        "digest_algorithm": "sha256",
        "files": _file_entries(directory, {seal_name}),
    }
    if not value["files"]:
        raise BundleError("refusing to seal an empty release directory")
    write_json(output, value)
    return output


def verify_seal(directory: Path, seal_name: str) -> dict[str, Any]:
    return _verify_seal(directory, seal_name, require_exact_file_set=True)


def _verify_seal(
    directory: Path,
    seal_name: str,
    *,
    require_exact_file_set: bool,
    expected_kind: str | None = None,
) -> dict[str, Any]:
    seal_path = directory / seal_name
    value = read_json(seal_path)
    _require_exact_keys(
        value,
        {"schema_version", "kind", "digest_algorithm", "files"},
        "release seal",
    )
    if value.get("schema_version") != 1 or value.get("digest_algorithm") != "sha256":
        raise BundleError("unsupported release seal schema or digest algorithm")
    kind = value.get("kind")
    if kind not in {"candidate", "verified"}:
        raise BundleError("unsupported release seal kind")
    if expected_kind is not None and kind != expected_kind:
        raise BundleError(f"release seal kind mismatch: got {kind}, expected {expected_kind}")
    entries = value.get("files")
    if not isinstance(entries, list) or not entries:
        raise BundleError("release seal has no files")
    expected_names: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise BundleError("release seal entry is not an object")
        _require_exact_keys(entry, {"name", "sha256", "size"}, "release seal entry")
        name = entry.get("name")
        checksum = entry.get("sha256")
        size = entry.get("size")
        if not isinstance(name, str) or not SAFE_NAME.fullmatch(name) or name == seal_name:
            raise BundleError("release seal contains an unsafe filename")
        if not isinstance(checksum, str) or not HEX64.fullmatch(checksum):
            raise BundleError(f"release seal has an invalid digest for {name}")
        if type(size) is not int or size < 0:
            raise BundleError(f"release seal has an invalid size for {name}")
        expected_names.append(name)
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise BundleError(f"sealed file is missing or not regular: {name}")
        if path.stat().st_size != size:
            raise BundleError(f"sealed size mismatch: {name}")
        if digest(path) != checksum:
            raise BundleError(f"sealed SHA256 mismatch: {name}")
    if expected_names != sorted(expected_names) or len(expected_names) != len(set(expected_names)):
        raise BundleError("release seal filenames must be unique and sorted")
    actual_names = [path.name for path in release_files(directory, excluding={seal_name})]
    if require_exact_file_set and actual_names != expected_names:
        missing = sorted(set(expected_names) - set(actual_names))
        extra = sorted(set(actual_names) - set(expected_names))
        raise BundleError(f"sealed file set mismatch: missing={missing}, extra={extra}")
    return value


def write_candidate_admission(
    directory: Path,
    candidate_seal_name: str = "candidate-seal.json",
    output_name: str = "candidate-admission.json",
) -> Path:
    """Admit an exact candidate before any post-build evidence is merged."""
    if not SAFE_NAME.fullmatch(output_name) or not output_name.endswith(".json"):
        raise BundleError("candidate admission name must be a safe JSON filename")
    output = directory / output_name
    if output.exists() or output.is_symlink():
        raise BundleError("candidate admission already exists")
    sealed = _verify_seal(
        directory,
        candidate_seal_name,
        require_exact_file_set=True,
        expected_kind="candidate",
    )
    timestamp = (
        dt.datetime.now(dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )
    value = {
        "schema_version": 1,
        "result": "admitted",
        "candidate_seal": candidate_seal_name,
        "candidate_seal_sha256": digest(directory / candidate_seal_name),
        "sealed_file_count": len(sealed["files"]),
        "admitted_at": timestamp,
        "workflow": os.environ.get("GITHUB_WORKFLOW_REF", "local-unattested"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
    }
    write_json(output, value)
    return output


def validate_candidate_admission(
    directory: Path,
    candidate_seal_name: str = "candidate-seal.json",
    admission_name: str = "candidate-admission.json",
) -> dict[str, Any]:
    value = read_json(directory / admission_name)
    _require_exact_keys(
        value,
        {
            "schema_version",
            "result",
            "candidate_seal",
            "candidate_seal_sha256",
            "sealed_file_count",
            "admitted_at",
            "workflow",
            "run_id",
        },
        "candidate admission",
    )
    if value.get("schema_version") != 1 or value.get("result") != "admitted":
        raise BundleError("candidate admission schema or result is invalid")
    if value.get("candidate_seal") != candidate_seal_name:
        raise BundleError("candidate admission refers to a different seal")
    checksum = value.get("candidate_seal_sha256")
    if not isinstance(checksum, str) or not HEX64.fullmatch(checksum):
        raise BundleError("candidate admission seal digest is invalid")
    seal_path = directory / candidate_seal_name
    if seal_path.is_symlink() or not seal_path.is_file() or digest(seal_path) != checksum:
        raise BundleError("candidate admission seal digest mismatch")
    sealed = _verify_seal(
        directory,
        candidate_seal_name,
        require_exact_file_set=False,
        expected_kind="candidate",
    )
    sealed_file_count = value.get("sealed_file_count")
    if type(sealed_file_count) is not int or sealed_file_count < 1:
        raise BundleError("candidate admission sealed file count is invalid")
    if sealed_file_count != len(sealed["files"]):
        raise BundleError("candidate admission sealed file count mismatch")
    if not _utc_timestamp(value.get("admitted_at")):
        raise BundleError("candidate admission timestamp is invalid")
    for field in ("workflow", "run_id"):
        if not isinstance(value.get(field), str) or not value[field].strip():
            raise BundleError(f"candidate admission {field} is invalid")
    return value


def write_evidence_import(
    directory: Path,
    source_directories: list[Path],
    *,
    candidate_seal_name: str = "candidate-seal.json",
    admission_name: str = "candidate-admission.json",
    output_name: str = "evidence-import.json",
) -> Path:
    """Import a closed, collision-free set of post-admission release inputs."""
    validate_candidate_admission(directory, candidate_seal_name, admission_name)
    if not source_directories:
        raise BundleError("evidence import requires at least one source directory")
    if output_name in CONTROL_FILES - {"evidence-import.json"} or not SAFE_NAME.fullmatch(output_name):
        raise BundleError("evidence import receipt name is invalid")
    output = directory / output_name
    if output.exists() or output.is_symlink():
        raise BundleError("evidence import receipt already exists")

    sources: dict[str, Path] = {}
    forbidden = CONTROL_FILES | {"build-metadata.json"}
    for source_directory in source_directories:
        for source in release_files(source_directory):
            if source.name in forbidden:
                raise BundleError(f"evidence import contains protected file {source.name}")
            if source.name in sources:
                raise BundleError(f"evidence import duplicates filename {source.name}")
            destination = directory / source.name
            if destination.exists() or destination.is_symlink():
                raise BundleError(f"evidence import collides with candidate file {source.name}")
            sources[source.name] = source
    if not sources:
        raise BundleError("evidence import source set is empty")

    entries: list[dict[str, Any]] = []
    for name, source in sorted(sources.items()):
        destination = directory / name
        shutil.copyfile(source, destination)
        entries.append(
            {"name": name, "sha256": digest(destination), "size": destination.stat().st_size}
        )
    timestamp = (
        dt.datetime.now(dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )
    value = {
        "schema_version": 1,
        "candidate_seal": candidate_seal_name,
        "candidate_seal_sha256": digest(directory / candidate_seal_name),
        "imported_at": timestamp,
        "workflow": os.environ.get("GITHUB_WORKFLOW_REF", "local-unattested"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "files": entries,
    }
    write_json(output, value)
    validate_evidence_import(
        directory,
        candidate_seal_name=candidate_seal_name,
        admission_name=admission_name,
        import_name=output_name,
    )
    return output


def validate_evidence_import(
    directory: Path,
    *,
    candidate_seal_name: str = "candidate-seal.json",
    admission_name: str = "candidate-admission.json",
    import_name: str = "evidence-import.json",
    allow_verification_outputs: bool = True,
) -> dict[str, Any]:
    admission = validate_candidate_admission(directory, candidate_seal_name, admission_name)
    value = read_json(directory / import_name)
    _require_exact_keys(
        value,
        {
            "schema_version",
            "candidate_seal",
            "candidate_seal_sha256",
            "imported_at",
            "workflow",
            "run_id",
            "files",
        },
        "evidence import",
    )
    if value.get("schema_version") != 1 or value.get("candidate_seal") != candidate_seal_name:
        raise BundleError("evidence import schema or candidate seal is invalid")
    if value.get("candidate_seal_sha256") != admission["candidate_seal_sha256"]:
        raise BundleError("evidence import is bound to a different candidate seal")
    if not _utc_timestamp(value.get("imported_at")):
        raise BundleError("evidence import timestamp is invalid")
    for field in ("workflow", "run_id"):
        if not isinstance(value.get(field), str) or not value[field].strip():
            raise BundleError(f"evidence import {field} is invalid")
    if value["workflow"] != admission["workflow"] or value["run_id"] != admission["run_id"]:
        raise BundleError("evidence import and candidate admission come from different workflow runs")

    entries = value.get("files")
    if not isinstance(entries, list) or not entries:
        raise BundleError("evidence import has no files")
    imported_names: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise BundleError("evidence import entry is not an object")
        _require_exact_keys(entry, {"name", "sha256", "size"}, "evidence import entry")
        name = entry.get("name")
        checksum = entry.get("sha256")
        size = entry.get("size")
        if (
            not isinstance(name, str)
            or not SAFE_NAME.fullmatch(name)
            or name in CONTROL_FILES
            or name == "build-metadata.json"
            or not isinstance(checksum, str)
            or not HEX64.fullmatch(checksum)
            or type(size) is not int
            or size < 0
        ):
            raise BundleError("evidence import entry metadata is invalid")
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise BundleError(f"imported evidence file is missing or not regular: {name}")
        if path.stat().st_size != size or digest(path) != checksum:
            raise BundleError(f"imported evidence file digest mismatch: {name}")
        imported_names.append(name)
    if imported_names != sorted(set(imported_names)):
        raise BundleError("evidence import filenames must be unique and sorted")

    sealed = _verify_seal(
        directory,
        candidate_seal_name,
        require_exact_file_set=False,
        expected_kind="candidate",
    )
    expected_names = {
        *(entry["name"] for entry in sealed["files"]),
        candidate_seal_name,
        admission_name,
        import_name,
        *imported_names,
    }
    if allow_verification_outputs:
        expected_names.update(
            name
            for name in ("verification.json", "release-manifest.json", "SHA256SUMS", "verified-seal.json")
            if (directory / name).is_file() and not (directory / name).is_symlink()
        )
    actual_names = {path.name for path in release_files(directory)}
    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        raise BundleError(f"augmented candidate file set mismatch: missing={missing}, extra={extra}")
    return value


def _authority_constant(path: Path, name: str) -> int:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"\b{name}\s*=\s*([0-9]+)\s*;", text)
    if match is None:
        raise BundleError(f"cannot read {name} from {path}")
    return int(match.group(1))


def _tool_version(command: list[str]) -> str:
    attempts = [command]
    if command and command[-1] == "--version":
        attempts.append([*command[:-1], "-v"])
    for attempt in attempts:
        try:
            completed = subprocess.run(
                attempt,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
        except OSError:
            continue
        if completed.returncode == 0 and completed.stdout:
            first_line = completed.stdout.splitlines()[0].strip()
            if first_line:
                return first_line[:512]
    return "unavailable"


def _cmake_cache_value(path: Path, key: str) -> str:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise BundleError(f"cannot read CMake cache {path}: {error}") from error
    prefix = f"{key}:"
    for line in lines:
        if line.startswith(prefix) and "=" in line:
            value = line.split("=", 1)[1].strip()
            if value:
                return value
    raise BundleError(f"CMake cache does not identify {key}: {path}")


def write_build_metadata(
    root: Path,
    tag: str,
    output: Path,
    cmake_cache: Path,
    target_os: str,
    target_os_version: str,
    architecture: str,
    tls_backend: str,
    build_options: list[str],
) -> None:
    identity = validate_release_identity(root, tag)
    wire = _authority_constant(root / "include/glifistore/server/protocol.hpp", "kProtocolVersion")
    persistent = _authority_constant(
        root / "include/glifistore/persistence/manifest.hpp", "kManifestFormatVersion"
    )
    compiler = _cmake_cache_value(cmake_cache, "CMAKE_CXX_COMPILER")
    linker = _cmake_cache_value(cmake_cache, "CMAKE_LINKER")
    metadata = {
        "schema_version": 1,
        "product_version": identity.product_version,
        "abi": {"major": identity.abi_major, "minor": identity.abi_minor},
        "wire_version": wire,
        "persistent_format_version": persistent,
        "source": {
            "repository": os.environ.get("GITHUB_SERVER_URL", "https://github.com")
            + "/"
            + os.environ.get("GITHUB_REPOSITORY", "gpicchiarelli/GlifiStore"),
            "tag": identity.tag,
            "git_sha": identity.git_sha,
            "source_date_epoch": identity.source_date_epoch,
        },
        "target": {
            "os": target_os,
            "os_version": target_os_version,
            "architecture": architecture,
            "tls_backend": tls_backend,
        },
        "toolchain": {
            "compiler": _tool_version([compiler, "--version"]),
            "linker": _tool_version([linker, "--version"]),
            "cmake": _tool_version(["cmake", "--version"]),
            "build_tool": _tool_version(["ninja", "--version"]),
            "python": platform.python_version(),
        },
        "build_options": sorted(set(build_options)),
        "builder": {
            "workflow": os.environ.get("GITHUB_WORKFLOW_REF", "local-unattested"),
            "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
            "runner_environment": os.environ.get("RUNNER_ENVIRONMENT", "local"),
        },
    }
    write_json(output, metadata)
    validate_build_metadata(output)


def _require_exact_keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        raise BundleError(
            f"{context} fields differ: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )


def validate_build_metadata(path: Path) -> dict[str, Any]:
    value = read_json(path)
    _require_exact_keys(
        value,
        {
            "schema_version",
            "product_version",
            "abi",
            "wire_version",
            "persistent_format_version",
            "source",
            "target",
            "toolchain",
            "build_options",
            "builder",
        },
        "build metadata",
    )
    version = value.get("product_version")
    if value.get("schema_version") != 1 or not isinstance(version, str) or not re.fullmatch(
        r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version
    ):
        raise BundleError("build metadata schema or product version is invalid")
    abi = value.get("abi")
    if not isinstance(abi, dict):
        raise BundleError("build metadata ABI identity is invalid")
    _require_exact_keys(abi, {"major", "minor"}, "build metadata ABI")
    if any(type(abi.get(field)) is not int or abi[field] < 0 for field in ("major", "minor")):
        raise BundleError("build metadata ABI version is invalid")
    if any(
        type(value.get(field)) is not int or value[field] < 1
        for field in ("wire_version", "persistent_format_version")
    ):
        raise BundleError("build metadata wire/persistence version is invalid")

    source = value.get("source")
    if not isinstance(source, dict):
        raise BundleError("build metadata source identity is invalid")
    _require_exact_keys(
        source,
        {"repository", "tag", "git_sha", "source_date_epoch"},
        "build metadata source",
    )
    if (
        not isinstance(source.get("repository"), str)
        or not source["repository"].startswith("https://")
        or source.get("tag") != f"v{version}"
        or not isinstance(source.get("git_sha"), str)
        or not GIT_OBJECT_ID.fullmatch(source["git_sha"])
        or type(source.get("source_date_epoch")) is not int
        or source["source_date_epoch"] <= 0
    ):
        raise BundleError("build metadata source authority is invalid")

    expected_objects = {
        "target": {"os", "os_version", "architecture", "tls_backend"},
        "toolchain": {"compiler", "linker", "cmake", "build_tool", "python"},
        "builder": {"workflow", "run_id", "runner_environment"},
    }
    for field, keys in expected_objects.items():
        nested = value.get(field)
        if not isinstance(nested, dict):
            raise BundleError(f"build metadata {field} is invalid")
        _require_exact_keys(nested, keys, f"build metadata {field}")
        if any(not isinstance(nested.get(key), str) or not nested[key].strip() for key in keys):
            raise BundleError(f"build metadata {field} contains an empty identity")
        if field == "toolchain" and any(nested[key] == "unavailable" for key in keys):
            raise BundleError("build metadata toolchain contains an unavailable command")
    options = value.get("build_options")
    if (
        not isinstance(options, list)
        or any(
            not isinstance(option, str)
            or not re.fullmatch(r"[A-Za-z0-9_]+=[^=\r\n]+", option)
            for option in options
        )
        or options != sorted(set(options))
    ):
        raise BundleError("build metadata options must be unique, sorted KEY=VALUE strings")
    return value


def bind_sbom(artifact: Path, input_path: Path, metadata_path: Path, output: Path) -> None:
    if artifact.is_symlink() or not artifact.is_file():
        raise BundleError(f"SBOM subject is missing or not regular: {artifact}")
    metadata = read_json(metadata_path)
    version = metadata.get("product_version")
    source = metadata.get("source")
    target = metadata.get("target")
    if not isinstance(version, str) or not isinstance(source, dict) or not isinstance(target, dict):
        raise BundleError("SBOM binding requires complete build metadata")
    if artifact_product_version(artifact.name) != version:
        raise BundleError("SBOM subject filename disagrees with product version")

    value = read_json(input_path)
    if value.get("spdxVersion") != "SPDX-2.3" or value.get("SPDXID") != "SPDXRef-DOCUMENT":
        raise BundleError("SBOM generator output is not SPDX 2.3")
    packages = value.get("packages", [])
    if not isinstance(packages, list):
        raise BundleError("SBOM generator packages field is not an array")
    packages = [
        package
        for package in packages
        if isinstance(package, dict) and package.get("SPDXID") != "SPDXRef-Package-GlifiStore"
    ]
    artifact_sha256 = digest(artifact)
    root_package = {
        "SPDXID": "SPDXRef-Package-GlifiStore",
        "name": "GlifiStore",
        "versionInfo": version,
        "packageFileName": artifact.name,
        "downloadLocation": "NOASSERTION",
        "filesAnalyzed": False,
        "checksums": [{"algorithm": "SHA256", "checksumValue": artifact_sha256}],
        "licenseConcluded": "BSD-3-Clause",
        "licenseDeclared": "BSD-3-Clause",
        "copyrightText": "Copyright (c) 2026, Giacomo Picchiarelli",
        "supplier": "Organization: GlifiStore",
        "sourceInfo": (
            f"git_sha={source.get('git_sha')}; tag={source.get('tag')}; "
            f"target={target.get('os')}/{target.get('architecture')}; "
            f"tls_backend={target.get('tls_backend')}"
        ),
        "externalRefs": [
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": f"pkg:generic/glifistore@{version}",
            }
        ],
    }
    packages.append(root_package)
    packages.sort(key=lambda package: str(package.get("SPDXID", "")))

    relationships = value.get("relationships", [])
    if not isinstance(relationships, list):
        raise BundleError("SBOM generator relationships field is not an array")
    describes = {
        "spdxElementId": "SPDXRef-DOCUMENT",
        "relationshipType": "DESCRIBES",
        "relatedSpdxElement": "SPDXRef-Package-GlifiStore",
    }
    relationships = [relationship for relationship in relationships if relationship != describes]
    relationships.append(describes)

    epoch = source.get("source_date_epoch")
    if not isinstance(epoch, int) or epoch < 0:
        raise BundleError("SBOM binding requires source_date_epoch")
    created = dt.datetime.fromtimestamp(epoch, tz=dt.timezone.utc).isoformat().replace("+00:00", "Z")
    creation_info = value.get("creationInfo")
    creators = creation_info.get("creators", []) if isinstance(creation_info, dict) else []
    if not isinstance(creators, list):
        creators = []
    creators = sorted(set([*creators, "Tool: GlifiStore release_bundle.py"]))

    value.update(
        {
            "dataLicense": "CC0-1.0",
            "name": f"GlifiStore-{version}-{artifact.name}",
            "documentNamespace": f"https://glifistore.dev/spdx/{version}/{artifact_sha256}",
            "creationInfo": {"created": created, "creators": creators},
            "packages": packages,
            "relationships": relationships,
        }
    )
    write_json(output, value)
    validate_sbom(output)


def validate_sbom(path: Path) -> None:
    value = read_json(path)
    if value.get("spdxVersion") != "SPDX-2.3" or value.get("SPDXID") != "SPDXRef-DOCUMENT":
        raise BundleError(f"SBOM is not an SPDX 2.3 document: {path.name}")
    if not isinstance(value.get("name"), str) or not value["name"]:
        raise BundleError(f"SBOM has no document name: {path.name}")
    namespace = value.get("documentNamespace")
    if not isinstance(namespace, str) or not namespace.startswith(("https://", "http://", "urn:")):
        raise BundleError(f"SBOM has no usable namespace: {path.name}")
    creation = value.get("creationInfo")
    if not isinstance(creation, dict) or not creation.get("creators"):
        raise BundleError(f"SBOM has no creator identity: {path.name}")
    packages = value.get("packages")
    if not isinstance(packages, list) or not packages:
        raise BundleError(f"SBOM contains no packages: {path.name}")
    identifiers: set[str] = set()
    for package in packages:
        if not isinstance(package, dict) or not package.get("name") or not package.get("SPDXID"):
            raise BundleError(f"SBOM contains an unidentified package: {path.name}")
        identifier = package["SPDXID"]
        if identifier in identifiers:
            raise BundleError(f"SBOM contains duplicate package identity {identifier}: {path.name}")
        identifiers.add(identifier)
        declared = package.get("licenseDeclared")
        concluded = package.get("licenseConcluded")
        if declared in (None, "", "NOASSERTION") and concluded in (None, "", "NOASSERTION"):
            raise BundleError(f"SBOM package has no resolved license: {package['name']}")

    suffix = ".spdx.json"
    if not path.name.endswith(suffix):
        raise BundleError(f"SBOM filename must end in {suffix}: {path.name}")
    artifact = path.with_name(path.name[: -len(suffix)])
    if artifact.is_symlink() or not artifact.is_file():
        raise BundleError(f"SBOM subject artifact is missing: {artifact.name}")
    artifact_version = artifact_product_version(artifact.name)
    if artifact_version is None:
        raise BundleError(f"SBOM subject has no product version: {artifact.name}")
    roots = [package for package in packages if package.get("SPDXID") == "SPDXRef-Package-GlifiStore"]
    if len(roots) != 1:
        raise BundleError(f"SBOM must identify exactly one GlifiStore root package: {path.name}")
    root = roots[0]
    if root.get("name") != "GlifiStore" or root.get("versionInfo") != artifact_version:
        raise BundleError(f"SBOM root package identity disagrees with its artifact: {path.name}")
    if root.get("licenseDeclared") != "BSD-3-Clause":
        raise BundleError(f"SBOM root package license is not BSD-3-Clause: {path.name}")
    expected_checksum = {"algorithm": "SHA256", "checksumValue": digest(artifact)}
    if expected_checksum not in root.get("checksums", []):
        raise BundleError(f"SBOM root package checksum disagrees with its artifact: {path.name}")
    if not any(
        relationship.get("spdxElementId") == "SPDXRef-DOCUMENT"
        and relationship.get("relationshipType") == "DESCRIBES"
        and relationship.get("relatedSpdxElement") == "SPDXRef-Package-GlifiStore"
        for relationship in value.get("relationships", [])
        if isinstance(relationship, dict)
    ):
        raise BundleError(f"SBOM document does not describe the GlifiStore package: {path.name}")


def write_checksums(directory: Path, output_name: str = "SHA256SUMS") -> Path:
    if output_name != "SHA256SUMS":
        raise BundleError("checksum authority must be named SHA256SUMS")
    output = directory / output_name
    lines = [
        f"{digest(path)}  {path.name}\n"
        for path in release_files(directory, excluding={output_name, "verified-seal.json"})
    ]
    if not lines:
        raise BundleError("refusing to write an empty checksum manifest")
    output.write_text("".join(lines), encoding="ascii")
    return output


def verify_checksums(directory: Path, name: str = "SHA256SUMS") -> None:
    path = directory / name
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except OSError as error:
        raise BundleError(f"cannot read {name}: {error}") from error
    expected_names: list[str] = []
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9][A-Za-z0-9._+-]*)", line)
        if match is None:
            raise BundleError(f"malformed SHA256SUMS line: {line!r}")
        checksum, filename = match.groups()
        if filename in {name, "verified-seal.json"}:
            raise BundleError(f"SHA256SUMS must not contain circular control file {filename}")
        artifact = directory / filename
        if artifact.is_symlink() or not artifact.is_file() or digest(artifact) != checksum:
            raise BundleError(f"SHA256SUMS mismatch: {filename}")
        expected_names.append(filename)
    actual = [
        path.name
        for path in release_files(directory, excluding={name, "verified-seal.json"})
    ]
    if expected_names != sorted(set(expected_names)) or expected_names != actual:
        raise BundleError("SHA256SUMS file set is incomplete, duplicated, or unsorted")


def _artifact_kind(name: str) -> str:
    if name.endswith(".spdx.json"):
        return "sbom"
    if name.endswith(".sigstore.json"):
        return "provenance"
    if name == "build-metadata.json":
        return "build_metadata"
    if name == "candidate-admission.json":
        return "candidate_admission"
    if name == "evidence-import.json":
        return "evidence_import"
    if name.endswith(".tar.xz") and name.startswith("GlifiStore-"):
        return "source"
    if name.endswith((".pkg", ".tgz", ".tar.xz")):
        return "binary_package"
    return "release_evidence"


def write_release_manifest(directory: Path, metadata_name: str = "build-metadata.json") -> Path:
    metadata = validate_build_metadata(directory / metadata_name)
    source = metadata.get("source")
    abi = metadata.get("abi")
    if not isinstance(source, dict) or not isinstance(abi, dict):
        raise BundleError("build metadata misses source or ABI identity")
    artifacts = []
    for path in release_files(directory, excluding=MANIFEST_EXCLUSIONS):
        artifacts.append(
            {
                "name": path.name,
                "kind": _artifact_kind(path.name),
                "sha256": digest(path),
                "size": path.stat().st_size,
            }
        )
    if not any(entry["kind"] == "source" for entry in artifacts):
        raise BundleError("release manifest requires a source archive")
    if not any(entry["kind"] == "sbom" for entry in artifacts):
        raise BundleError("release manifest requires at least one SPDX SBOM")
    manifest = {
        "schema_version": 1,
        "product_version": metadata.get("product_version"),
        "abi_major": abi.get("major"),
        "abi_minor": abi.get("minor"),
        "wire_version": metadata.get("wire_version"),
        "persistent_format_version": metadata.get("persistent_format_version"),
        "git_sha": source.get("git_sha"),
        "tag": source.get("tag"),
        "build_metadata_ref": metadata_name,
        "provenance_subject": "verified-seal.json",
        "provenance_ref": "verified-seal.sigstore.json",
        "compatibility_evidence_refs": sorted(
            entry["name"] for entry in artifacts if entry["name"].endswith("-evidence.json")
        ),
        "artifacts": artifacts,
    }
    output = directory / "release-manifest.json"
    write_json(output, manifest)
    validate_release_manifest(directory, output.name)
    return output


def validate_release_manifest(directory: Path, name: str = "release-manifest.json") -> None:
    value = read_json(directory / name)
    _require_exact_keys(
        value,
        {
            "schema_version",
            "product_version",
            "abi_major",
            "abi_minor",
            "wire_version",
            "persistent_format_version",
            "git_sha",
            "tag",
            "build_metadata_ref",
            "provenance_subject",
            "provenance_ref",
            "compatibility_evidence_refs",
            "artifacts",
        },
        "release manifest",
    )
    required_scalars = {
        "schema_version": int,
        "product_version": str,
        "abi_major": int,
        "abi_minor": int,
        "wire_version": int,
        "persistent_format_version": int,
        "git_sha": str,
        "tag": str,
        "build_metadata_ref": str,
        "provenance_subject": str,
        "provenance_ref": str,
    }
    for field, expected_type in required_scalars.items():
        if not isinstance(value.get(field), expected_type):
            raise BundleError(f"release manifest field {field} has the wrong type")
    if (
        value["schema_version"] != 1
        or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", value["product_version"])
        or not GIT_OBJECT_ID.fullmatch(value["git_sha"])
        or type(value["abi_major"]) is not int
        or type(value["abi_minor"]) is not int
        or value["abi_major"] < 0
        or value["abi_minor"] < 0
    ):
        raise BundleError("release manifest schema or git SHA is invalid")
    if value["tag"] != f"v{value['product_version']}":
        raise BundleError("release manifest tag/version mismatch")
    if (
        value["provenance_subject"] != "verified-seal.json"
        or value["provenance_ref"] != "verified-seal.sigstore.json"
        or not SAFE_NAME.fullmatch(value["build_metadata_ref"])
    ):
        raise BundleError("release manifest control references are invalid")
    entries = value.get("artifacts")
    if not isinstance(entries, list) or not entries:
        raise BundleError("release manifest has no artifacts")
    names: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise BundleError("release manifest artifact is not an object")
        _require_exact_keys(entry, {"name", "kind", "sha256", "size"}, "manifest artifact")
        filename = entry.get("name")
        kind = entry.get("kind")
        checksum = entry.get("sha256")
        size = entry.get("size")
        if not isinstance(filename, str) or not SAFE_NAME.fullmatch(filename):
            raise BundleError("release manifest contains an unsafe artifact name")
        if (
            filename in MANIFEST_EXCLUSIONS
            or kind != _artifact_kind(filename)
            or not isinstance(checksum, str)
            or not HEX64.fullmatch(checksum)
            or type(size) is not int
            or size < 0
        ):
            raise BundleError(f"release manifest artifact metadata is invalid: {filename}")
        artifact = directory / filename
        if artifact.is_symlink() or not artifact.is_file():
            raise BundleError(f"release manifest artifact is missing: {filename}")
        if artifact.stat().st_size != size or digest(artifact) != checksum:
            raise BundleError(f"release manifest artifact digest mismatch: {filename}")
        names.append(filename)
    expected = [path.name for path in release_files(directory, excluding=MANIFEST_EXCLUSIONS)]
    if names != sorted(set(names)) or names != expected:
        raise BundleError("release manifest artifact set is incomplete, duplicated, or unsorted")
    evidence_refs = value.get("compatibility_evidence_refs")
    expected_evidence_refs = sorted(filename for filename in names if filename.endswith("-evidence.json"))
    if evidence_refs != expected_evidence_refs:
        raise BundleError("release manifest evidence index is incomplete, duplicated, or unsorted")
    metadata = validate_build_metadata(directory / value["build_metadata_ref"])
    if (
        metadata.get("product_version") != value["product_version"]
        or metadata.get("abi") != {"major": value["abi_major"], "minor": value["abi_minor"]}
        or metadata.get("wire_version") != value["wire_version"]
        or metadata.get("persistent_format_version") != value["persistent_format_version"]
        or metadata.get("source", {}).get("git_sha") != value["git_sha"]
        or metadata.get("source", {}).get("tag") != value["tag"]
    ):
        raise BundleError("release manifest disagrees with build metadata authority")


def validate_release_policy(directory: Path) -> None:
    """Release-wide fail-closed policy; platform jobs must supply these exact proofs."""
    validate_release_manifest(directory)
    manifest = read_json(directory / "release-manifest.json")
    version = manifest["product_version"]
    names = {entry["name"] for entry in manifest["artifacts"]}
    if "candidate-admission.json" not in names:
        raise BundleError("release policy requires a candidate admission receipt")
    if "evidence-import.json" not in names:
        raise BundleError("release policy requires an evidence import receipt")
    import_receipt = validate_evidence_import(directory)
    target_patterns = {
        "source": re.compile(rf"^GlifiStore-{re.escape(version)}\.tar\.xz$"),
        "linux": re.compile(rf"^glifistore-{re.escape(version)}-linux-[A-Za-z0-9_-]+\.tar\.xz$"),
        "abi_consumer": re.compile(
            rf"^glifistore-abi-v{manifest['abi_major']}-consumer-{re.escape(version)}-linux-"
            r"[A-Za-z0-9_-]+\.tar\.xz$"
        ),
        "wire_client": re.compile(
            rf"^glifistore-wire-v{manifest['wire_version']}-client-{re.escape(version)}-linux-"
            r"[A-Za-z0-9_-]+\.tar\.xz$"
        ),
        "freebsd": re.compile(rf"^glifistore-{re.escape(version)}-freebsd[0-9._-]*-[A-Za-z0-9_-]+\.pkg$"),
        "openbsd": re.compile(rf"^glifistore-{re.escape(version)}-openbsd[0-9._-]*-[A-Za-z0-9_-]+\.tgz$"),
    }
    for target, pattern in target_patterns.items():
        matches = sorted(name for name in names if pattern.fullmatch(name))
        if len(matches) != 1:
            raise BundleError(f"release policy requires exactly one {target} artifact, found {matches}")
        if f"{matches[0]}.spdx.json" not in names:
            raise BundleError(f"release policy requires an SPDX SBOM for {matches[0]}")

    required_evidence = {
        filename: evidence_type for evidence_type, filename in EVIDENCE_FILENAMES.items()
    }
    missing = sorted(set(required_evidence) - names)
    if missing:
        raise BundleError("release policy misses compatibility/security evidence: " + ", ".join(missing))
    subjects = {entry["name"]: entry["sha256"] for entry in manifest["artifacts"]}
    freebsd_subject = next(name for name in names if target_patterns["freebsd"].fullmatch(name))
    openbsd_subject = next(name for name in names if target_patterns["openbsd"].fullmatch(name))
    for name, evidence_type in sorted(required_evidence.items()):
        expected_subject = "candidate-seal.json"
        if evidence_type == "freebsd_package":
            expected_subject = freebsd_subject
        elif evidence_type == "openbsd_package":
            expected_subject = openbsd_subject
        evidence = validate_evidence(
            directory / name,
            expected_type=evidence_type,
            expected_git_sha=manifest["git_sha"],
            expected_product_version=manifest["product_version"],
            expected_subjects=subjects,
            require_ci=True,
        )
        if evidence["producer"]["run_id"] != import_receipt["run_id"]:
            raise BundleError(f"release evidence comes from a different workflow run: {name}")
        if evidence["subject"]["name"] != expected_subject:
            raise BundleError(f"release evidence has the wrong subject for {name}")


def write_verification(directory: Path, candidate_seal_name: str) -> Path:
    # Exactness was proven before augmentation and is bound by the admission
    # receipt. Here we re-check every originally sealed byte while permitting
    # evidence/package files that the verified seal will cover transitively.
    validate_evidence_import(
        directory,
        candidate_seal_name=candidate_seal_name,
        allow_verification_outputs=False,
    )
    seal_path = directory / candidate_seal_name
    timestamp = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    value = {
        "schema_version": 1,
        "result": "verified",
        "candidate_seal": candidate_seal_name,
        "candidate_seal_sha256": digest(seal_path),
        "verified_at": timestamp,
        "workflow": os.environ.get("GITHUB_WORKFLOW_REF", "local-unattested"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
    }
    output = directory / "verification.json"
    write_json(output, value)
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    metadata = subparsers.add_parser("build-metadata")
    metadata.add_argument("--root", type=Path, default=Path.cwd())
    metadata.add_argument("--tag", required=True)
    metadata.add_argument("--output", type=Path, required=True)
    metadata.add_argument("--cmake-cache", type=Path, required=True)
    metadata.add_argument("--target-os", required=True)
    metadata.add_argument("--target-os-version", required=True)
    metadata.add_argument("--architecture", required=True)
    metadata.add_argument("--tls-backend", required=True)
    metadata.add_argument("--build-option", action="append", default=[])
    validate_metadata = subparsers.add_parser("validate-metadata")
    validate_metadata.add_argument("paths", type=Path, nargs="+")

    for command in ("seal", "verify-seal"):
        sub = subparsers.add_parser(command)
        sub.add_argument("--directory", type=Path, required=True)
        sub.add_argument("--seal", required=True)
        if command == "seal":
            sub.add_argument("--kind", choices=("candidate", "verified"), required=True)

    sbom = subparsers.add_parser("validate-sbom")
    sbom.add_argument("paths", type=Path, nargs="+")
    bind = subparsers.add_parser("bind-sbom")
    bind.add_argument("--artifact", type=Path, required=True)
    bind.add_argument("--input", type=Path, required=True)
    bind.add_argument("--metadata", type=Path, required=True)
    bind.add_argument("--output", type=Path, required=True)

    checksums = subparsers.add_parser("checksums")
    checksums.add_argument("--directory", type=Path, required=True)
    verify_sums = subparsers.add_parser("verify-checksums")
    verify_sums.add_argument("--directory", type=Path, required=True)

    manifest = subparsers.add_parser("manifest")
    manifest.add_argument("--directory", type=Path, required=True)
    validate_manifest = subparsers.add_parser("validate-manifest")
    validate_manifest.add_argument("--directory", type=Path, required=True)
    release_policy = subparsers.add_parser("validate-release-policy")
    release_policy.add_argument("--directory", type=Path, required=True)

    admission = subparsers.add_parser("admit-candidate")
    admission.add_argument("--directory", type=Path, required=True)
    admission.add_argument("--candidate-seal", default="candidate-seal.json")

    evidence_import = subparsers.add_parser("import-evidence")
    evidence_import.add_argument("--directory", type=Path, required=True)
    evidence_import.add_argument("--source-directory", type=Path, action="append", required=True)
    validate_import = subparsers.add_parser("validate-evidence-import")
    validate_import.add_argument("--directory", type=Path, required=True)

    verification = subparsers.add_parser("verification")
    verification.add_argument("--directory", type=Path, required=True)
    verification.add_argument("--candidate-seal", default="candidate-seal.json")

    arguments = parser.parse_args()
    try:
        if arguments.command == "build-metadata":
            write_build_metadata(
                arguments.root.resolve(),
                arguments.tag,
                arguments.output,
                arguments.cmake_cache,
                arguments.target_os,
                arguments.target_os_version,
                arguments.architecture,
                arguments.tls_backend,
                arguments.build_option,
            )
        elif arguments.command == "validate-metadata":
            for path in arguments.paths:
                validate_build_metadata(path)
        elif arguments.command == "seal":
            seal(arguments.directory, arguments.seal, arguments.kind)
        elif arguments.command == "verify-seal":
            verify_seal(arguments.directory, arguments.seal)
        elif arguments.command == "validate-sbom":
            for path in arguments.paths:
                validate_sbom(path)
        elif arguments.command == "bind-sbom":
            bind_sbom(arguments.artifact, arguments.input, arguments.metadata, arguments.output)
        elif arguments.command == "checksums":
            write_checksums(arguments.directory)
        elif arguments.command == "verify-checksums":
            verify_checksums(arguments.directory)
        elif arguments.command == "manifest":
            write_release_manifest(arguments.directory)
        elif arguments.command == "validate-manifest":
            validate_release_manifest(arguments.directory)
        elif arguments.command == "validate-release-policy":
            validate_release_policy(arguments.directory)
        elif arguments.command == "admit-candidate":
            write_candidate_admission(arguments.directory, arguments.candidate_seal)
        elif arguments.command == "import-evidence":
            write_evidence_import(arguments.directory, arguments.source_directory)
        elif arguments.command == "validate-evidence-import":
            validate_evidence_import(arguments.directory)
        elif arguments.command == "verification":
            write_verification(arguments.directory, arguments.candidate_seal)
        else:
            raise BundleError(f"unsupported command: {arguments.command}")
    except (BundleError, EvidenceError, OSError, ReleaseIdentityError) as error:
        print(f"release bundle FAILED: {error}", file=sys.stderr)
        return 1
    print(f"release bundle {arguments.command} OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
