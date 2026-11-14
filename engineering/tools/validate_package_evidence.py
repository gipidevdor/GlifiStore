#!/usr/bin/env python3
"""Emit and validate GlifiStore package evidence, fail-closed.

Package evidence is the only channel through which a backend reports what it
did. It is bound to the release context (commit, product version, package
version, package revision) and, when the run produced an artifact, to that
artifact's SHA-256. A run that did not execute a check must say so with
NOT_RUN, BLOCKED or OPEN_GATE: a result may never be more favourable than the
checks that back it, and a PASS check must reference a retained log.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.generate_package_matrix import (
    PackageMatrixError,
    check_vocabulary,
    load_matrix,
    required_checks,
)
from engineering.tools.generate_release_context import ReleaseContextError, load_release_context
from engineering.tools.package_framework import (
    BACKENDS,
    LIFECYCLE_STATES,
    MATRIX_PATH,
    PROFILES,
    RESULTS,
    SAFE_NAME,
    SETTLED_RESULTS,
    STAGES,
    PackageFrameworkError,
    digest,
    encode_json,
    is_utc_timestamp,
    producer_identity,
    read_json,
    utc_now,
    validate_against_schema,
)


SCHEMA_NAME = "package-evidence.schema.json"
CHECK_FIELDS = {"id", "status", "command", "evidence_ref", "detail"}
# Optional so a backend whose matrix rows are not categorised yet still emits a
# valid plan; validate_evidence refuses any category that disagrees with the matrix.
OPTIONAL_CHECK_FIELDS = {"category"}


class PackageEvidenceError(RuntimeError):
    pass


def evidence_filename(backend: str, profile: str, stage: str) -> str:
    return f"{backend}-{profile}-{stage}-package-evidence.json"


def _read_check_plan(path: Path) -> list[dict[str, Any]]:
    if path.is_symlink() or not path.is_file():
        raise PackageEvidenceError(f"check plan is missing or not regular: {path}")
    value = _read_json_array(path)
    checks: list[dict[str, Any]] = []
    for entry in value:
        keys = set(entry) if isinstance(entry, dict) else set()
        if not isinstance(entry, dict) or not CHECK_FIELDS <= keys <= (
            CHECK_FIELDS | OPTIONAL_CHECK_FIELDS
        ):
            raise PackageEvidenceError(
                f"check plan entries require exactly {sorted(CHECK_FIELDS)} "
                f"plus optionally {sorted(OPTIONAL_CHECK_FIELDS)}"
            )
        check = {
            "id": entry["id"],
            "status": entry["status"],
            "command": entry["command"],
            "evidence_ref": entry["evidence_ref"],
            "detail": entry["detail"],
        }
        if "category" in entry:
            check["category"] = entry["category"]
        checks.append(check)
    return checks


def _read_json_array(path: Path) -> list[Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise PackageEvidenceError(f"invalid JSON {path}: {error}") from error
    if not isinstance(value, list) or not value:
        raise PackageEvidenceError(f"expected a non-empty JSON array: {path}")
    return value


def validate_evidence(
    path: Path,
    *,
    context: dict[str, Any] | None = None,
    matrix: dict[str, Any] | None = None,
    expected_backend: str | None = None,
    expected_profile: str | None = None,
    artifact_root: Path | None = None,
    require_pass: bool = False,
    require_ci: bool = False,
) -> dict[str, Any]:
    value = read_json(path)
    validate_against_schema(value, SCHEMA_NAME, f"package evidence {path.name}")

    backend, profile, stage = value["backend"], value["profile"], value["stage"]
    if expected_backend is not None and backend != expected_backend:
        raise PackageEvidenceError(f"evidence is for backend {backend}, expected {expected_backend}")
    if expected_profile is not None and profile != expected_profile:
        raise PackageEvidenceError(f"evidence is for profile {profile}, expected {expected_profile}")
    if path.name != evidence_filename(backend, profile, stage):
        raise PackageEvidenceError(
            f"package evidence must be named {evidence_filename(backend, profile, stage)}"
        )
    if not is_utc_timestamp(value["generated_at"]):
        raise PackageEvidenceError("package evidence timestamp is not an ISO-8601 UTC instant")
    producer = value["producer"]
    if require_ci and (producer["workflow"] == "local-unattested" or producer["run_id"] == "local"):
        raise PackageEvidenceError("retained CI evidence is required for this profile")

    checks = value["checks"]
    statuses: dict[str, str] = {}
    for check in checks:
        identifier = check["id"]
        if identifier in statuses:
            raise PackageEvidenceError(f"package evidence duplicates check id {identifier}")
        statuses[identifier] = check["status"]
        reference = check["evidence_ref"]
        if check["status"] == "PASS" and not reference:
            raise PackageEvidenceError(f"a passing check must retain a log: {identifier}")
        if reference is not None:
            log = path.parent / reference
            if log.is_symlink() or not log.is_file() or log.stat().st_size == 0:
                raise PackageEvidenceError(f"check log is missing or empty: {reference}")

    result = value["result"]
    reported = set(statuses.values())
    if "FAIL" in reported and result != "FAIL":
        raise PackageEvidenceError(f"a failing check forbids result {result}")
    if result == "PASS" and not reported <= SETTLED_RESULTS:
        unsettled = sorted(reported - SETTLED_RESULTS)
        raise PackageEvidenceError(f"result PASS contradicts unsettled checks: {unsettled}")
    if result in SETTLED_RESULTS and result != "PASS" and "PASS" in reported:
        raise PackageEvidenceError(f"result {result} understates checks that actually passed")
    if result == "PASS" and value["lifecycle_state"] == "NONE":
        raise PackageEvidenceError("result PASS requires a lifecycle state")
    if value["lifecycle_state"] != "NONE" and not any(
        status == "PASS" for status in statuses.values()
    ):
        raise PackageEvidenceError(
            f"lifecycle state {value['lifecycle_state']} is not backed by any passing check"
        )
    if require_pass and result != "PASS":
        raise PackageEvidenceError(f"this profile requires PASS, got {result}")

    if matrix is not None:
        try:
            declared = set(matrix_checks(matrix, backend))
            owed = required_checks(matrix, backend, profile)
            vocabulary = check_vocabulary(matrix)
        except PackageMatrixError as error:
            raise PackageEvidenceError(str(error)) from error
        unknown = sorted(set(statuses) - declared)
        if unknown:
            raise PackageEvidenceError(f"evidence reports undeclared checks: {unknown}")
        for check in checks:
            expected_category = vocabulary[check["id"]]["category"]
            if check.get("category") != expected_category:
                raise PackageEvidenceError(
                    f"check {check['id']} reports category {check.get('category')!r} while the "
                    f"matrix declares {expected_category!r}"
                )
        missing = sorted(set(owed) - set(statuses))
        if missing:
            raise PackageEvidenceError(f"evidence misses required checks for {profile}: {missing}")
        if result == "PASS":
            not_passing = sorted(check for check in owed if statuses[check] != "PASS")
            if not_passing:
                raise PackageEvidenceError(f"result PASS with non-passing required checks: {not_passing}")

    if context is not None:
        if value["git_sha"] != context["git"]["commit"]:
            raise PackageEvidenceError("package evidence is for a different commit")
        if value["product_version"] != context["product_version"]:
            raise PackageEvidenceError("package evidence is for a different product version")
        if value["package_revision"] != context["package_revision"]:
            raise PackageEvidenceError("package evidence is for a different package revision")
        expected_version = context["package_versions"][backend]["package_version"]
        if value["package_version"] != expected_version:
            raise PackageEvidenceError(
                f"package version {value['package_version']} disagrees with the release context "
                f"({expected_version})"
            )

    subject = value["subject"]
    if subject["kind"] == "artifact":
        if not subject["sha256"]:
            raise PackageEvidenceError("an artifact subject requires its SHA-256")
        if artifact_root is not None:
            artifact = artifact_root / subject["name"]
            if artifact.is_symlink() or not artifact.is_file():
                raise PackageEvidenceError(f"evidence subject is missing: {subject['name']}")
            if digest(artifact) != subject["sha256"]:
                raise PackageEvidenceError(f"evidence subject digest mismatch: {subject['name']}")
    elif subject["sha256"] is not None:
        raise PackageEvidenceError("a source-tree subject cannot carry an artifact digest")
    return value


def matrix_checks(matrix: dict[str, Any], backend: str) -> list[str]:
    for entry in matrix["backends"]:
        if entry["id"] == backend:
            return list(entry["checks"])
    raise PackageEvidenceError(f"unknown packaging backend: {backend}")


def emit_evidence(
    *,
    backend: str,
    profile: str,
    stage: str,
    result: str,
    lifecycle_state: str,
    context_path: Path,
    check_plan: Path,
    output: Path,
    subject_path: Path | None = None,
    limitations: list[str] | None = None,
    residuals: list[str] | None = None,
    matrix_path: Path = MATRIX_PATH,
    require_ci: bool = False,
) -> Path:
    if backend not in BACKENDS:
        raise PackageEvidenceError(f"unsupported packaging backend: {backend}")
    if profile not in PROFILES:
        raise PackageEvidenceError(f"unsupported CI profile: {profile}")
    if stage not in STAGES:
        raise PackageEvidenceError(f"unsupported lifecycle stage: {stage}")
    if result not in RESULTS:
        raise PackageEvidenceError(f"unsupported result: {result}")
    if lifecycle_state not in LIFECYCLE_STATES:
        raise PackageEvidenceError(f"unsupported lifecycle state: {lifecycle_state}")
    if output.name != evidence_filename(backend, profile, stage):
        raise PackageEvidenceError(
            f"package evidence output must be named {evidence_filename(backend, profile, stage)}"
        )
    if output.exists() or output.is_symlink():
        raise PackageEvidenceError(f"refusing to replace existing package evidence: {output}")

    context = load_release_context(context_path)
    matrix = load_matrix(matrix_path)
    if subject_path is None:
        subject = {"kind": "source_tree", "name": "source-tree", "sha256": None}
    else:
        if subject_path.is_symlink() or not subject_path.is_file():
            raise PackageEvidenceError(f"evidence subject is missing: {subject_path}")
        if SAFE_NAME.fullmatch(subject_path.name) is None:
            raise PackageEvidenceError(f"unsafe evidence subject name: {subject_path.name}")
        subject = {
            "kind": "artifact",
            "name": subject_path.name,
            "sha256": digest(subject_path),
        }

    value = {
        "schema_version": 1,
        "backend": backend,
        "profile": profile,
        "stage": stage,
        "result": result,
        "lifecycle_state": lifecycle_state,
        "git_sha": context["git"]["commit"],
        "product_version": context["product_version"],
        "package_version": context["package_versions"][backend]["package_version"],
        "package_revision": context["package_revision"],
        "generated_at": utc_now(),
        "producer": producer_identity(),
        "subject": subject,
        "checks": _read_check_plan(check_plan),
        "limitations": list(limitations or []),
        "residuals": [_residual(item) for item in residuals or []],
    }

    # Written under its final name so validation sees the filename contract and
    # resolves check logs against the directory the evidence will live in.
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(encode_json(value), encoding="utf-8")
    try:
        validate_evidence(
            output,
            context=context,
            matrix=matrix,
            expected_backend=backend,
            expected_profile=profile,
            require_ci=require_ci,
        )
    except (PackageEvidenceError, PackageFrameworkError):
        output.unlink(missing_ok=True)
        raise
    return output


def _residual(specification: str) -> dict[str, str]:
    identifier, separator, remainder = specification.partition("=")
    description, blocked_separator, blocked_by = remainder.partition("|")
    if not separator or not blocked_separator or not identifier.strip() or not description.strip():
        raise PackageEvidenceError("residuals must be given as id=description|blocked_by")
    if not blocked_by.strip():
        raise PackageEvidenceError("residuals must name what blocks them")
    if SAFE_NAME.fullmatch(identifier.strip()) is None:
        raise PackageEvidenceError(f"unsafe residual id: {identifier!r}")
    return {
        "id": identifier.strip(),
        "description": description.strip(),
        "blocked_by": blocked_by.strip(),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    emit = commands.add_parser("emit")
    emit.add_argument("--backend", required=True)
    emit.add_argument("--profile", choices=PROFILES, required=True)
    emit.add_argument("--stage", choices=STAGES, default="full")
    emit.add_argument("--result", choices=RESULTS, required=True)
    emit.add_argument("--lifecycle-state", choices=LIFECYCLE_STATES, default="NONE")
    emit.add_argument("--release-context", type=Path, required=True)
    emit.add_argument("--check-plan", type=Path, required=True)
    emit.add_argument("--subject", type=Path)
    emit.add_argument("--limitation", action="append", default=[])
    emit.add_argument("--residual", action="append", default=[])
    emit.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    emit.add_argument("--require-ci", action="store_true")
    emit.add_argument("--output", type=Path, required=True)

    validate = commands.add_parser("validate")
    validate.add_argument("paths", type=Path, nargs="+")
    validate.add_argument("--release-context", type=Path)
    validate.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    validate.add_argument("--backend")
    validate.add_argument("--profile", choices=PROFILES)
    validate.add_argument("--artifact-root", type=Path)
    validate.add_argument("--require-pass", action="store_true")
    validate.add_argument("--require-ci", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "emit":
            emit_evidence(
                backend=arguments.backend,
                profile=arguments.profile,
                stage=arguments.stage,
                result=arguments.result,
                lifecycle_state=arguments.lifecycle_state,
                context_path=arguments.release_context,
                check_plan=arguments.check_plan,
                output=arguments.output,
                subject_path=arguments.subject,
                limitations=arguments.limitation,
                residuals=arguments.residual,
                matrix_path=arguments.matrix,
                require_ci=arguments.require_ci,
            )
            print(arguments.output)
        else:
            context = (
                load_release_context(arguments.release_context)
                if arguments.release_context
                else None
            )
            matrix = load_matrix(arguments.matrix)
            for path in arguments.paths:
                validate_evidence(
                    path,
                    context=context,
                    matrix=matrix,
                    expected_backend=arguments.backend,
                    expected_profile=arguments.profile,
                    artifact_root=arguments.artifact_root,
                    require_pass=arguments.require_pass,
                    require_ci=arguments.require_ci,
                )
            print(f"package evidence OK ({len(arguments.paths)} file(s))")
    except (
        PackageEvidenceError,
        PackageFrameworkError,
        PackageMatrixError,
        ReleaseContextError,
        OSError,
    ) as error:
        print(f"package evidence FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
