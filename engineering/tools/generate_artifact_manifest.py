#!/usr/bin/env python3
"""Record the identity graph of artifacts produced by GlifiStore packaging CI.

Every packaged artifact is identified by id, kind, platform, architecture and
SHA-256, and is bound to the SHA-256 of the sealed source archive it was derived
from. Publishing may then admit exact bytes instead of rebuilding, and a package
built from a different source can never be mistaken for the release artifact.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.generate_release_context import ReleaseContextError, load_release_context
from engineering.tools.package_framework import (
    ARTIFACT_KINDS,
    BACKENDS,
    HEX64,
    LIFECYCLE_STATES,
    PROFILES,
    SAFE_NAME,
    PackageFrameworkError,
    digest,
    is_utc_timestamp,
    read_json,
    utc_now,
    validate_against_schema,
    write_json,
)


SCHEMA_NAME = "artifact-manifest.schema.json"
ARTIFACT_FIELDS = {"id", "kind", "platform", "arch", "path", "backend", "lifecycle_state"}
REQUIRED_ARTIFACT_FIELDS = {"id", "kind", "platform", "arch", "path"}


class ArtifactManifestError(RuntimeError):
    pass


def parse_artifact(specification: str) -> dict[str, str]:
    """Parse `id=...,kind=...,platform=...,arch=...,path=...[,backend=...]`."""
    fields: dict[str, str] = {}
    for item in specification.split(","):
        key, separator, value = item.partition("=")
        key, value = key.strip(), value.strip()
        if not separator or not key or not value:
            raise ArtifactManifestError(f"artifact fields must be key=value: {item!r}")
        if key not in ARTIFACT_FIELDS:
            raise ArtifactManifestError(f"unsupported artifact field: {key}")
        if key in fields:
            raise ArtifactManifestError(f"artifact repeats field {key}")
        fields[key] = value
    missing = sorted(REQUIRED_ARTIFACT_FIELDS - set(fields))
    if missing:
        raise ArtifactManifestError(f"artifact misses required fields: {missing}")
    return fields


def _regular_file(path: Path, context: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise ArtifactManifestError(f"{context} is missing or not a regular file: {path}")
    return path


def _entry(fields: dict[str, str], parent_sha256: str, root: Path | None) -> dict[str, Any]:
    identifier = fields["id"]
    if SAFE_NAME.fullmatch(identifier) is None:
        raise ArtifactManifestError(f"unsafe artifact id: {identifier!r}")
    if fields["kind"] not in ARTIFACT_KINDS:
        raise ArtifactManifestError(f"unsupported artifact kind: {fields['kind']}")
    backend = fields.get("backend")
    if backend is not None and backend not in BACKENDS:
        raise ArtifactManifestError(f"unsupported artifact backend: {backend}")
    lifecycle_state = fields.get("lifecycle_state", "BUILT")
    if lifecycle_state not in LIFECYCLE_STATES:
        raise ArtifactManifestError(f"unsupported artifact lifecycle state: {lifecycle_state}")
    candidate = Path(fields["path"])
    if root is not None and not candidate.is_absolute():
        candidate = root / candidate
    path = _regular_file(candidate, f"artifact {identifier}")
    if SAFE_NAME.fullmatch(path.name) is None:
        raise ArtifactManifestError(f"unsafe artifact filename: {path.name}")
    return {
        "id": identifier,
        "kind": fields["kind"],
        "backend": backend,
        "platform": fields["platform"],
        "arch": fields["arch"],
        "name": path.name,
        "sha256": digest(path),
        "size": path.stat().st_size,
        "parent_source_sha256": parent_sha256,
        "lifecycle_state": lifecycle_state,
    }


def build_manifest(
    *,
    context_path: Path,
    profile: str,
    parent_source: Path,
    artifacts: list[str],
    artifact_root: Path | None = None,
) -> dict[str, Any]:
    if profile not in PROFILES:
        raise ArtifactManifestError(f"unknown CI profile: {profile}")
    if not artifacts:
        raise ArtifactManifestError("an artifact manifest without artifacts proves nothing")
    context = load_release_context(context_path)
    source = _regular_file(parent_source, "parent source archive")
    if SAFE_NAME.fullmatch(source.name) is None:
        raise ArtifactManifestError(f"unsafe parent source filename: {source.name}")
    parent_sha256 = digest(source)
    manifest = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "product_version": context["product_version"],
        "package_revision": context["package_revision"],
        "git_sha": context["git"]["commit"],
        "profile": profile,
        "parent_source": {"name": source.name, "sha256": parent_sha256},
        "artifacts": [
            _entry(parse_artifact(specification), parent_sha256, artifact_root)
            for specification in artifacts
        ],
    }
    validate_manifest(manifest)
    return manifest


def validate_manifest(
    manifest: dict[str, Any],
    *,
    artifact_root: Path | None = None,
    context: dict[str, Any] | None = None,
) -> dict[str, Any]:
    validate_against_schema(manifest, SCHEMA_NAME, "artifact manifest")
    if not is_utc_timestamp(manifest["generated_at"]):
        raise ArtifactManifestError("artifact manifest timestamp is not an ISO-8601 UTC instant")
    parent = manifest["parent_source"]
    if HEX64.fullmatch(parent["sha256"]) is None:
        raise ArtifactManifestError("parent source digest is invalid")
    identifiers: list[str] = []
    names: list[str] = []
    for entry in manifest["artifacts"]:
        if entry["parent_source_sha256"] != parent["sha256"]:
            raise ArtifactManifestError(
                f"artifact {entry['id']} was not derived from the recorded parent source"
            )
        if entry["name"] == parent["name"] and entry["sha256"] != parent["sha256"]:
            raise ArtifactManifestError(
                f"artifact {entry['id']} reuses the parent source name with different bytes"
            )
        identifiers.append(entry["id"])
        names.append(entry["name"])
        if artifact_root is not None:
            path = _regular_file(artifact_root / entry["name"], f"artifact {entry['id']}")
            if digest(path) != entry["sha256"] or path.stat().st_size != entry["size"]:
                raise ArtifactManifestError(f"artifact digest mismatch: {entry['name']}")
    if len(set(identifiers)) != len(identifiers):
        raise ArtifactManifestError("artifact manifest duplicates an artifact id")
    if len(set(names)) != len(names):
        raise ArtifactManifestError("artifact manifest duplicates an artifact filename")
    if context is not None:
        if manifest["product_version"] != context["product_version"]:
            raise ArtifactManifestError("artifact manifest is for a different product version")
        if manifest["git_sha"] != context["git"]["commit"]:
            raise ArtifactManifestError("artifact manifest is for a different commit")
        if manifest["package_revision"] != context["package_revision"]:
            raise ArtifactManifestError("artifact manifest is for a different package revision")
    return manifest


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    create = commands.add_parser("create")
    create.add_argument("--release-context", type=Path, required=True)
    create.add_argument("--profile", choices=PROFILES, required=True)
    create.add_argument("--parent-source", type=Path, required=True)
    create.add_argument("--artifact", action="append", default=[])
    create.add_argument("--artifact-root", type=Path)
    create.add_argument("--output", type=Path)
    create.add_argument("--replace", action="store_true")

    validate = commands.add_parser("validate")
    validate.add_argument("--manifest", type=Path, required=True)
    validate.add_argument("--artifact-root", type=Path)
    validate.add_argument("--release-context", type=Path)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "create":
            manifest = build_manifest(
                context_path=arguments.release_context,
                profile=arguments.profile,
                parent_source=arguments.parent_source,
                artifacts=arguments.artifact,
                artifact_root=arguments.artifact_root,
            )
            if arguments.output is None:
                print(json.dumps(manifest, indent=2, sort_keys=True))
            else:
                write_json(arguments.output, manifest, replace=arguments.replace)
                print(arguments.output)
        else:
            context = (
                load_release_context(arguments.release_context)
                if arguments.release_context
                else None
            )
            validate_manifest(
                read_json(arguments.manifest),
                artifact_root=arguments.artifact_root,
                context=context,
            )
            print(f"artifact manifest OK ({arguments.manifest})")
    except (ArtifactManifestError, PackageFrameworkError, ReleaseContextError, OSError) as error:
        print(f"artifact manifest FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
