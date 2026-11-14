#!/usr/bin/env python3
"""Shared fail-closed primitives for the GlifiStore package CI framework."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import platform
import re
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
SCHEMA_DIR = REPO_ROOT / "engineering" / "schemas"
MATRIX_PATH = REPO_ROOT / "engineering" / "distribution" / "package-matrix.yaml"

BACKENDS = ("deb", "freebsd", "homebrew", "macports", "openbsd", "rpm")
PROFILES = ("pr", "main", "nightly", "release")
STAGES = ("metadata", "build", "inspect", "install", "verify", "upgrade", "remove", "full")
RESULTS = (
    "PASS",
    "FAIL",
    "NOT_RUN",
    "NOT_APPLICABLE",
    "NOT_APPLICABLE_INITIAL_BASELINE",
    "BLOCKED",
    "OPEN_GATE",
)
SETTLED_RESULTS = frozenset({"PASS", "NOT_APPLICABLE", "NOT_APPLICABLE_INITIAL_BASELINE"})
LIFECYCLE_STATES = (
    "NONE",
    "STRUCTURAL",
    "BUILT",
    "INSTALLED",
    "FUNCTIONALLY_VERIFIED",
    "LIFECYCLE_VERIFIED",
    "UPGRADE_VERIFIED",
    "RELEASE_ADMITTED",
)
ARTIFACT_KINDS = (
    "deb",
    "evidence",
    "freebsd_pkg",
    "homebrew_formula",
    "macports_port",
    "openbsd_tgz",
    "rpm",
    "sbom",
    "signature",
    "source",
    "tarball",
)

HEX64 = re.compile(r"^[0-9a-f]{64}$")
GIT_OBJECT_ID = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")


class PackageFrameworkError(RuntimeError):
    pass


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PackageFrameworkError(f"invalid JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise PackageFrameworkError(f"JSON root must be an object: {path}")
    return value


def encode_json(value: Any) -> str:
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def write_json(path: Path, value: Any, *, replace: bool = False) -> Path:
    if not replace and (path.exists() or path.is_symlink()):
        raise PackageFrameworkError(f"refusing to replace existing output: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(encode_json(value), encoding="utf-8")
    return path


def utc_now() -> str:
    return (
        dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    )


def is_utc_timestamp(value: object) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError:
        return False
    return parsed.tzinfo is not None and parsed.utcoffset() == dt.timedelta(0)


def require_exact_keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        raise PackageFrameworkError(
            f"{context} fields differ: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )


def producer_identity() -> dict[str, str]:
    return {
        "workflow": os.environ.get("GITHUB_WORKFLOW_REF", "local-unattested"),
        "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
        "os": os.environ.get("RUNNER_OS", platform.system()),
        "os_version": platform.release(),
        "architecture": os.environ.get("RUNNER_ARCH", platform.machine()),
    }


def load_schema(name: str) -> dict[str, Any]:
    path = SCHEMA_DIR / name
    if path.is_symlink() or not path.is_file():
        raise PackageFrameworkError(f"missing JSON Schema: {path}")
    return read_json(path)


def validate_against_schema(value: Any, schema_name: str, context: str) -> None:
    """Fail closed: an unavailable validator is an error, never a silent skip."""
    try:
        from jsonschema import Draft202012Validator
    except ImportError as error:  # pragma: no cover - dependency is installed in CI
        raise PackageFrameworkError(
            "jsonschema is required. Install with: python3 -m pip install jsonschema"
        ) from error
    validator = Draft202012Validator(load_schema(schema_name))
    failures = sorted(validator.iter_errors(value), key=lambda failure: list(failure.path))
    if failures:
        location = "/".join(str(part) for part in failures[0].path) or "<root>"
        raise PackageFrameworkError(f"{context} violates {schema_name} at {location}: {failures[0].message}")


def load_yaml(path: Path) -> dict[str, Any]:
    try:
        import yaml
    except ImportError as error:  # pragma: no cover - dependency is installed in CI
        raise PackageFrameworkError(
            "PyYAML is required. Install with: python3 -m pip install PyYAML"
        ) from error
    if path.is_symlink() or not path.is_file():
        raise PackageFrameworkError(f"missing YAML authority: {path}")
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        raise PackageFrameworkError(f"invalid YAML {path}: {error}") from error
    if not isinstance(value, dict):
        raise PackageFrameworkError(f"YAML root must be a mapping: {path}")
    return value
