#!/usr/bin/env python3
"""Validate retained, artifact-bound evidence used by a release promotion."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


HEX64 = re.compile(r"^[0-9a-f]{64}$")
GIT_OBJECT_ID = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]*$")

REQUIRED_CHECKS: dict[str, frozenset[str]] = {
    "abi_compatibility": frozenset(
        {"exact-symbols", "layout", "old-consumer-new-library", "new-consumer-old-library"}
    ),
    "persistent_compatibility": frozenset(
        {"new-opens-old", "fixture-recovery", "migration", "backup-verify-repair"}
    ),
    "sdk_installed_interop": frozenset(
        {"source-isolation", "c", "cpp", "python", "perl", "go", "erlang", "ruby"}
    ),
    "security_matrix": frozenset(
        {
            "asan",
            "ubsan",
            "tsan",
            "codeql",
            "static-analysis",
            "warnings-as-errors",
            "dependency-scan",
            "secret-scan",
            "license-validation",
            "sbom-validation",
            "binary-hardening",
        }
    ),
    "wire_compatibility": frozenset(
        {"new-client-new-server", "old-client-new-server", "new-client-old-server"}
    ),
    "freebsd_package": frozenset(
        {
            "package-build",
            "package-install",
            "file-inventory",
            "external-consumer",
            "service-start",
            "put-get-erase",
            "graceful-shutdown",
            "restart-recovery",
            "uninstall",
            "config-preservation",
        }
    ),
    "openbsd_package": frozenset(
        {
            "package-build",
            "package-install",
            "file-inventory",
            "external-consumer",
            "service-start",
            "put-get-erase",
            "graceful-shutdown",
            "restart-recovery",
            "uninstall",
            "config-preservation",
        }
    ),
    "reproducibility": frozenset({"independent-rebuild", "artifact-compare"}),
}
EVIDENCE_FILENAMES: dict[str, str] = {
    "abi_compatibility": "abi-compatibility-evidence.json",
    "persistent_compatibility": "persistent-compatibility-evidence.json",
    "sdk_installed_interop": "sdk-installed-interop-evidence.json",
    "security_matrix": "security-matrix-evidence.json",
    "wire_compatibility": "wire-compatibility-evidence.json",
    "freebsd_package": "freebsd-package-evidence.json",
    "openbsd_package": "openbsd-package-evidence.json",
    "reproducibility": "reproducibility-evidence.json",
}


class EvidenceError(RuntimeError):
    pass


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"invalid evidence JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise EvidenceError(f"evidence root must be an object: {path}")
    return value


def _exact_keys(value: dict[str, Any], expected: set[str], context: str) -> None:
    actual = set(value)
    if actual != expected:
        raise EvidenceError(
            f"{context} fields differ: missing={sorted(expected - actual)}, "
            f"unexpected={sorted(actual - expected)}"
        )


def _utc_timestamp(value: object) -> bool:
    if not isinstance(value, str) or not value.endswith("Z"):
        return False
    try:
        parsed = dt.datetime.fromisoformat(value[:-1] + "+00:00")
    except ValueError:
        return False
    return parsed.tzinfo is not None and parsed.utcoffset() == dt.timedelta(0)


def validate_evidence(
    path: Path,
    *,
    expected_type: str | None = None,
    expected_git_sha: str | None = None,
    expected_product_version: str | None = None,
    expected_subjects: dict[str, str] | None = None,
    require_ci: bool = False,
) -> dict[str, Any]:
    value = _read_json(path)
    _exact_keys(
        value,
        {
            "schema_version",
            "evidence_type",
            "result",
            "git_sha",
            "product_version",
            "generated_at",
            "producer",
            "subject",
            "checks",
            "limitations",
        },
        "release evidence",
    )
    evidence_type = value.get("evidence_type")
    if value.get("schema_version") != 1 or evidence_type not in REQUIRED_CHECKS:
        raise EvidenceError("unsupported release evidence schema or type")
    if expected_type is not None and evidence_type != expected_type:
        raise EvidenceError(f"evidence type mismatch: got {evidence_type}, expected {expected_type}")
    if value.get("result") != "passed":
        raise EvidenceError("release evidence result is not passed")
    git_sha = value.get("git_sha")
    if not isinstance(git_sha, str) or not GIT_OBJECT_ID.fullmatch(git_sha):
        raise EvidenceError("release evidence git SHA is invalid")
    if expected_git_sha is not None and git_sha != expected_git_sha:
        raise EvidenceError("release evidence is for a different commit")
    version = value.get("product_version")
    if not isinstance(version, str) or re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise EvidenceError("release evidence product version is invalid")
    if expected_product_version is not None and version != expected_product_version:
        raise EvidenceError("release evidence is for a different product version")
    if not _utc_timestamp(value.get("generated_at")):
        raise EvidenceError("release evidence timestamp is not an ISO-8601 UTC instant")

    producer = value.get("producer")
    if not isinstance(producer, dict):
        raise EvidenceError("release evidence producer is invalid")
    _exact_keys(
        producer,
        {"workflow", "run_id", "os", "os_version", "architecture"},
        "release evidence producer",
    )
    if any(not isinstance(producer.get(key), str) or not producer[key].strip() for key in producer):
        raise EvidenceError("release evidence producer contains an empty identity")
    if require_ci and (
        producer["workflow"] == "local-unattested" or producer["run_id"] == "local"
    ):
        raise EvidenceError("release promotion requires retained CI evidence")

    subject = value.get("subject")
    if not isinstance(subject, dict):
        raise EvidenceError("release evidence subject is invalid")
    _exact_keys(subject, {"name", "sha256"}, "release evidence subject")
    name = subject.get("name")
    checksum = subject.get("sha256")
    if not isinstance(name, str) or not SAFE_NAME.fullmatch(name):
        raise EvidenceError("release evidence subject name is unsafe")
    if not isinstance(checksum, str) or not HEX64.fullmatch(checksum):
        raise EvidenceError("release evidence subject digest is invalid")
    if expected_subjects is not None:
        if name not in expected_subjects or expected_subjects[name] != checksum:
            raise EvidenceError("release evidence subject is absent or has a different digest")

    checks = value.get("checks")
    if not isinstance(checks, list) or not checks:
        raise EvidenceError("release evidence contains no checks")
    check_ids: set[str] = set()
    for check in checks:
        if not isinstance(check, dict):
            raise EvidenceError("release evidence check is not an object")
        _exact_keys(check, {"id", "status", "command", "evidence_ref"}, "release evidence check")
        check_id = check.get("id")
        command = check.get("command")
        evidence_ref = check.get("evidence_ref")
        if not isinstance(check_id, str) or not SAFE_NAME.fullmatch(check_id):
            raise EvidenceError("release evidence check id is unsafe")
        if check_id in check_ids:
            raise EvidenceError(f"release evidence duplicates check id {check_id}")
        check_ids.add(check_id)
        if check.get("status") != "passed":
            raise EvidenceError(f"release evidence check did not pass: {check_id}")
        if not isinstance(command, str) or not command.strip():
            raise EvidenceError(f"release evidence check has no command: {check_id}")
        if not isinstance(evidence_ref, str) or not SAFE_NAME.fullmatch(evidence_ref):
            raise EvidenceError(f"release evidence check has an unsafe log reference: {check_id}")
        evidence_path = path.parent / evidence_ref
        if evidence_path.is_symlink() or not evidence_path.is_file() or evidence_path.stat().st_size == 0:
            raise EvidenceError(f"release evidence check log is missing or empty: {evidence_ref}")
    required_checks = REQUIRED_CHECKS[evidence_type]
    missing_checks = sorted(required_checks - check_ids)
    unexpected_checks = sorted(check_ids - required_checks)
    if missing_checks or unexpected_checks:
        raise EvidenceError(
            f"release evidence {evidence_type} check set differs: "
            f"missing={missing_checks}, unexpected={unexpected_checks}"
        )

    limitations = value.get("limitations")
    if not isinstance(limitations, list) or any(
        not isinstance(limitation, str) or not limitation.strip() for limitation in limitations
    ):
        raise EvidenceError("release evidence limitations must be an array of non-empty strings")
    return value


def _git_sha(root: Path) -> str:
    configured = os.environ.get("GITHUB_SHA", "")
    if configured:
        return configured
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise EvidenceError("cannot determine release evidence git SHA")
    return completed.stdout.strip()


def _read_check_plan(path: Path) -> list[dict[str, str]]:
    if path.is_symlink() or not path.is_file():
        raise EvidenceError(f"evidence check plan is missing or not regular: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise EvidenceError(f"invalid evidence check plan {path}: {error}") from error
    if not isinstance(value, list) or not value:
        raise EvidenceError("evidence check plan must be a non-empty array")
    checks: list[dict[str, str]] = []
    for entry in value:
        if not isinstance(entry, dict) or set(entry) != {"id", "command", "evidence_ref"}:
            raise EvidenceError("evidence check plan entries require id, command, and evidence_ref")
        if any(not isinstance(entry[key], str) or not entry[key].strip() for key in entry):
            raise EvidenceError("evidence check plan contains an empty value")
        checks.append(
            {
                "id": entry["id"],
                "status": "passed",
                "command": entry["command"],
                "evidence_ref": entry["evidence_ref"],
            }
        )
    return checks


def create_evidence(
    *,
    root: Path,
    evidence_type: str,
    subject: Path,
    output: Path,
    check_plan: Path,
    limitations: list[str],
    require_ci: bool,
) -> Path:
    if evidence_type not in REQUIRED_CHECKS:
        raise EvidenceError(f"unsupported release evidence type: {evidence_type}")
    if output.name != EVIDENCE_FILENAMES[evidence_type]:
        raise EvidenceError(
            f"release evidence output for {evidence_type} must be "
            f"{EVIDENCE_FILENAMES[evidence_type]}"
        )
    if output.exists() or output.is_symlink():
        raise EvidenceError(f"refusing to replace existing release evidence: {output}")
    if subject.is_symlink() or not subject.is_file():
        raise EvidenceError(f"release evidence subject is missing or not regular: {subject}")
    if not SAFE_NAME.fullmatch(subject.name):
        raise EvidenceError("release evidence subject filename is unsafe")
    if require_ci and any(
        not os.environ.get(name, "").strip()
        for name in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_WORKFLOW_REF")
    ):
        raise EvidenceError("release promotion requires retained CI evidence")
    output.parent.mkdir(parents=True, exist_ok=True)
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    timestamp = (
        dt.datetime.now(dt.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z")
    )
    workflow = os.environ.get("GITHUB_WORKFLOW_REF", "local-unattested")
    run_id = os.environ.get("GITHUB_RUN_ID", "local")
    value: dict[str, Any] = {
        "schema_version": 1,
        "evidence_type": evidence_type,
        "result": "passed",
        "git_sha": _git_sha(root),
        "product_version": version,
        "generated_at": timestamp,
        "producer": {
            "workflow": workflow,
            "run_id": run_id,
            "os": os.environ.get("RUNNER_OS", platform.system()),
            "os_version": platform.release(),
            "architecture": os.environ.get("RUNNER_ARCH", platform.machine()),
        },
        "subject": {"name": subject.name, "sha256": digest(subject)},
        "checks": _read_check_plan(check_plan),
        "limitations": limitations,
    }
    encoded = json.dumps(value, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=output.parent,
            prefix=f".{output.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            stream.write(encoded)
            temporary_name = stream.name
        temporary_path = Path(temporary_name)
        validate_evidence(
            temporary_path,
            expected_type=evidence_type,
            expected_git_sha=value["git_sha"],
            expected_product_version=version,
            expected_subjects={subject.name: digest(subject)},
            require_ci=require_ci,
        )
        os.replace(temporary_path, output)
    finally:
        if temporary_name is not None:
            Path(temporary_name).unlink(missing_ok=True)
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command")
    create = subparsers.add_parser("create")
    create.add_argument("--root", type=Path, default=Path.cwd())
    create.add_argument("--type", choices=sorted(REQUIRED_CHECKS), required=True)
    create.add_argument("--subject", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--check-plan", type=Path, required=True)
    create.add_argument("--limitation", action="append", default=[])
    create.add_argument("--require-ci", action="store_true")

    validate = subparsers.add_parser("validate")
    validate.add_argument("paths", nargs="+", type=Path)
    validate.add_argument("--require-ci", action="store_true")

    # Preserve the original `release_evidence.py [--require-ci] FILE...`
    # interface used by existing release automation.
    argv = sys.argv[1:]
    if argv and argv[0] not in {"create", "validate", "-h", "--help"}:
        argv.insert(0, "validate")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "create":
            create_evidence(
                root=arguments.root.resolve(),
                evidence_type=arguments.type,
                subject=arguments.subject,
                output=arguments.output,
                check_plan=arguments.check_plan,
                limitations=arguments.limitation,
                require_ci=arguments.require_ci,
            )
            count = 1
        elif arguments.command == "validate":
            for path in arguments.paths:
                validate_evidence(path, require_ci=arguments.require_ci)
            count = len(arguments.paths)
        else:
            parser.error("a command is required")
    except (EvidenceError, OSError) as error:
        print(f"release evidence FAILED: {error}", file=sys.stderr)
        return 1
    print(f"release evidence {arguments.command} OK ({count} file(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
