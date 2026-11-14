#!/usr/bin/env python3
"""Validate and expand the GlifiStore package matrix.

The YAML matrix is the only description of packaging backends, targets, CI
profiles and the check vocabulary. This tool validates it fail-closed, expands
it into the JSON that a GitHub Actions strategy or scripts/package-ci.sh
consumes, and materialises the per-backend check plan so that a backend cannot
invent a check id or silently omit one it owes.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.package_framework import (
    BACKENDS,
    LIFECYCLE_STATES,
    MATRIX_PATH,
    PROFILES,
    RESULTS,
    SAFE_NAME,
    STAGES,
    PackageFrameworkError,
    load_yaml,
    require_exact_keys,
    write_json,
)


ARCHITECTURES = ("amd64", "arm64", "noarch")
BACKEND_STATUSES = ("PLANNED", "STRUCTURAL", "IMPLEMENTED")
CHECK_CATEGORIES = ("structural", "native-build", "package", "service", "upstream-accepted")
OUT_OF_SCOPE_REQUIRED = ("apple-pkg", "windows")
RELEASE_POLICY_ARTIFACTS = (
    "abi_consumer",
    "freebsd",
    "linux",
    "openbsd",
    "source",
    "wire_client",
)
FORBIDDEN_PLATFORM_TOKENS = ("windows", "win32", "win64")


class PackageMatrixError(RuntimeError):
    pass


def _mapping(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PackageMatrixError(f"{context} must be a mapping")
    return value


def _string_list(value: Any, context: str) -> list[str]:
    if not isinstance(value, list) or not value:
        raise PackageMatrixError(f"{context} must be a non-empty list")
    if any(not isinstance(item, str) or not item.strip() for item in value):
        raise PackageMatrixError(f"{context} must contain non-empty strings")
    return value


def _validate_checks(matrix: dict[str, Any]) -> dict[str, dict[str, Any]]:
    checks: dict[str, dict[str, Any]] = {}
    entries = matrix.get("lifecycle_checks")
    if not isinstance(entries, list) or not entries:
        raise PackageMatrixError("matrix declares no lifecycle checks")
    for entry in entries:
        check = _mapping(entry, "lifecycle check")
        keys = set(check)
        if not {"id", "stage", "description"} <= keys or not keys <= {
            "id",
            "stage",
            "description",
            "category",
        }:
            raise PackageMatrixError(
                "lifecycle check fields differ: missing="
                f"{sorted({'id', 'stage', 'description'} - keys)}, "
                f"unexpected={sorted(keys - {'id', 'stage', 'description', 'category'})}"
            )
        identifier = check["id"]
        if not isinstance(identifier, str) or SAFE_NAME.fullmatch(identifier) is None:
            raise PackageMatrixError(f"unsafe lifecycle check id: {identifier!r}")
        if identifier in checks:
            raise PackageMatrixError(f"duplicate lifecycle check id: {identifier}")
        if check["stage"] not in STAGES:
            raise PackageMatrixError(f"unsupported stage for {identifier}: {check['stage']}")
        if not isinstance(check["description"], str) or not check["description"].strip():
            raise PackageMatrixError(f"lifecycle check {identifier} has no description")
        category = check.get("category")
        if category is not None and category not in CHECK_CATEGORIES:
            raise PackageMatrixError(
                f"lifecycle check {identifier} has an unsupported category: {category!r}"
            )
        checks[identifier] = {
            "stage": check["stage"],
            "description": check["description"].strip(),
            "category": category,
        }
    return checks


def _validate_target(target: Any, backend_profiles: list[str], status: str) -> dict[str, Any]:
    value = _mapping(target, "backend target")
    require_exact_keys(
        value,
        {"id", "platform", "arch", "runner", "container", "container_digest", "profiles"},
        "backend target",
    )
    identifier = value["id"]
    if not isinstance(identifier, str) or SAFE_NAME.fullmatch(identifier) is None:
        raise PackageMatrixError(f"unsafe target id: {identifier!r}")
    platform = value["platform"]
    if not isinstance(platform, str) or not platform.strip():
        raise PackageMatrixError(f"target {identifier} has no platform")
    if any(token in platform.lower() for token in FORBIDDEN_PLATFORM_TOKENS):
        raise PackageMatrixError(f"Windows is out of scope and cannot appear as a target: {platform}")
    if value["arch"] not in ARCHITECTURES:
        raise PackageMatrixError(f"target {identifier} has an unsupported architecture")
    if not isinstance(value["runner"], str) or not value["runner"].strip():
        raise PackageMatrixError(f"target {identifier} has no runner")
    for field in ("container", "container_digest"):
        if value[field] is not None and (
            not isinstance(value[field], str) or not value[field].strip()
        ):
            raise PackageMatrixError(f"target {identifier} has an invalid {field}")
    if status == "IMPLEMENTED" and value["container"] is not None and not value["container_digest"]:
        raise PackageMatrixError(
            f"target {identifier} runs an implemented backend and must pin its container digest"
        )
    profiles = _string_list(value["profiles"], f"target {identifier} profiles")
    unknown = sorted(set(profiles) - set(PROFILES))
    if unknown:
        raise PackageMatrixError(f"target {identifier} has unknown profiles: {unknown}")
    outside = sorted(set(profiles) - set(backend_profiles))
    if outside:
        raise PackageMatrixError(
            f"target {identifier} claims profiles its backend does not run: {outside}"
        )
    return value


def _validate_backend(
    backend: Any, checks: dict[str, dict[str, Any]], release_artifacts: set[str]
) -> dict[str, Any]:
    value = _mapping(backend, "backend")
    require_exact_keys(
        value,
        {
            "id",
            "display_name",
            "package_kind",
            "status",
            "lifecycle_state",
            "required_for_release",
            "wave",
            "profiles",
            "targets",
            "checks",
            "required_checks",
            "limitations",
        },
        "backend",
    )
    identifier = value["id"]
    if identifier not in BACKENDS:
        raise PackageMatrixError(f"unsupported packaging backend: {identifier!r}")
    if value["status"] not in BACKEND_STATUSES:
        raise PackageMatrixError(f"backend {identifier} has an unsupported status")
    if value["lifecycle_state"] not in LIFECYCLE_STATES:
        raise PackageMatrixError(f"backend {identifier} has an unsupported lifecycle state")
    if not isinstance(value["required_for_release"], bool):
        raise PackageMatrixError(f"backend {identifier} required_for_release must be a boolean")
    if value["required_for_release"] and identifier not in release_artifacts:
        raise PackageMatrixError(
            f"backend {identifier} claims required_for_release without a release policy artifact"
        )
    if value["status"] == "PLANNED" and value["lifecycle_state"] != "NONE":
        raise PackageMatrixError(
            f"backend {identifier} is PLANNED and cannot claim lifecycle state "
            f"{value['lifecycle_state']}"
        )

    profiles = _string_list(value["profiles"], f"backend {identifier} profiles")
    unknown = sorted(set(profiles) - set(PROFILES))
    if unknown:
        raise PackageMatrixError(f"backend {identifier} has unknown profiles: {unknown}")

    declared = _string_list(value["checks"], f"backend {identifier} checks")
    if len(set(declared)) != len(declared):
        raise PackageMatrixError(f"backend {identifier} duplicates a check id")
    unknown_checks = sorted(set(declared) - set(checks))
    if unknown_checks:
        raise PackageMatrixError(
            f"backend {identifier} declares checks outside the vocabulary: {unknown_checks}"
        )
    if value["status"] != "PLANNED":
        uncategorised = sorted(check for check in declared if checks[check]["category"] is None)
        if uncategorised:
            raise PackageMatrixError(
                f"backend {identifier} runs real checks and must categorise every check it "
                f"declares: {uncategorised}"
            )

    required = _mapping(value["required_checks"], f"backend {identifier} required_checks")
    if sorted(required) != sorted(profiles):
        raise PackageMatrixError(
            f"backend {identifier} must declare required checks for exactly its profiles"
        )
    for profile, ids in required.items():
        selected = _string_list(ids, f"backend {identifier} required checks for {profile}")
        missing = sorted(set(selected) - set(declared))
        if missing:
            raise PackageMatrixError(
                f"backend {identifier} requires undeclared checks in {profile}: {missing}"
            )

    targets = value["targets"]
    if not isinstance(targets, list) or not targets:
        raise PackageMatrixError(f"backend {identifier} declares no targets")
    seen: set[str] = set()
    for target in targets:
        expanded = _validate_target(target, profiles, value["status"])
        if expanded["id"] in seen:
            raise PackageMatrixError(f"backend {identifier} duplicates target {expanded['id']}")
        seen.add(expanded["id"])
    covered = {profile for target in targets for profile in target["profiles"]}
    uncovered = sorted(set(profiles) - covered)
    if uncovered:
        raise PackageMatrixError(f"backend {identifier} has profiles without a target: {uncovered}")

    limitations = value["limitations"]
    if not isinstance(limitations, list) or any(
        not isinstance(item, str) or not item.strip() for item in limitations
    ):
        raise PackageMatrixError(f"backend {identifier} limitations must be non-empty strings")
    if value["status"] != "IMPLEMENTED" and not limitations:
        raise PackageMatrixError(
            f"backend {identifier} is not implemented and must state its limitations"
        )
    return value


def validate_matrix(matrix: dict[str, Any]) -> dict[str, Any]:
    require_exact_keys(
        matrix,
        {
            "version",
            "profiles",
            "lifecycle_states",
            "lifecycle_checks",
            "out_of_scope",
            "release_policy_artifacts",
            "backends",
        },
        "package matrix",
    )
    if matrix["version"] != 1:
        raise PackageMatrixError("unsupported package matrix version")
    profiles = _mapping(matrix["profiles"], "package matrix profiles")
    if sorted(profiles) != sorted(PROFILES):
        raise PackageMatrixError(f"package matrix must describe exactly the profiles {list(PROFILES)}")
    for name, profile in profiles.items():
        require_exact_keys(
            _mapping(profile, f"profile {name}"),
            {"description", "requires_sealed_artifacts"},
            f"profile {name}",
        )
        if not isinstance(profile["requires_sealed_artifacts"], bool):
            raise PackageMatrixError(f"profile {name} requires_sealed_artifacts must be a boolean")
    if profiles["release"]["requires_sealed_artifacts"] is not True:
        raise PackageMatrixError("the release profile must require sealed artifacts")
    if list(matrix["lifecycle_states"]) != list(LIFECYCLE_STATES):
        raise PackageMatrixError("package matrix lifecycle states diverge from the framework chain")

    checks = _validate_checks(matrix)

    out_of_scope: set[str] = set()
    for entry in matrix["out_of_scope"]:
        value = _mapping(entry, "out of scope entry")
        require_exact_keys(value, {"id", "reason", "required_before_scope"}, "out of scope entry")
        if any(not isinstance(value[key], str) or not value[key].strip() for key in value):
            raise PackageMatrixError("out of scope entries require non-empty text")
        out_of_scope.add(value["id"])
    missing_scope = sorted(set(OUT_OF_SCOPE_REQUIRED) - out_of_scope)
    if missing_scope:
        raise PackageMatrixError(
            f"package matrix must keep these targets explicitly out of scope: {missing_scope}"
        )

    release_artifacts: set[str] = set()
    for entry in matrix["release_policy_artifacts"]:
        value = _mapping(entry, "release policy artifact")
        require_exact_keys(
            value, {"id", "description", "required_for_release"}, "release policy artifact"
        )
        if value["required_for_release"] is not True:
            raise PackageMatrixError(
                "release policy artifacts mirror validate_release_policy and are always required"
            )
        release_artifacts.add(value["id"])
    if sorted(release_artifacts) != sorted(RELEASE_POLICY_ARTIFACTS):
        raise PackageMatrixError(
            "release policy artifacts must mirror release_bundle.validate_release_policy: "
            f"{sorted(RELEASE_POLICY_ARTIFACTS)}"
        )

    backends = matrix["backends"]
    if not isinstance(backends, list) or not backends:
        raise PackageMatrixError("package matrix declares no backends")
    identifiers = []
    for backend in backends:
        expanded = _validate_backend(backend, checks, release_artifacts)
        if expanded["id"] in out_of_scope:
            raise PackageMatrixError(f"backend {expanded['id']} is declared out of scope")
        identifiers.append(expanded["id"])
    if len(set(identifiers)) != len(identifiers):
        raise PackageMatrixError("package matrix duplicates a backend id")
    unknown = sorted(set(identifiers) - set(BACKENDS))
    if unknown:
        raise PackageMatrixError(f"package matrix declares unknown backends: {unknown}")
    return matrix


def load_matrix(path: Path = MATRIX_PATH) -> dict[str, Any]:
    return validate_matrix(load_yaml(path))


def backend(matrix: dict[str, Any], identifier: str) -> dict[str, Any]:
    for entry in matrix["backends"]:
        if entry["id"] == identifier:
            return entry
    raise PackageMatrixError(f"unknown packaging backend: {identifier}")


def check_vocabulary(matrix: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Declared stage, description and category for every check id in the matrix."""
    return {
        check["id"]: {
            "stage": check["stage"],
            "description": check["description"],
            "category": check.get("category"),
        }
        for check in matrix["lifecycle_checks"]
    }


def required_checks(matrix: dict[str, Any], identifier: str, profile: str) -> list[str]:
    entry = backend(matrix, identifier)
    if profile not in entry["required_checks"]:
        raise PackageMatrixError(f"backend {identifier} does not run in profile {profile}")
    return list(entry["required_checks"][profile])


def expand(
    matrix: dict[str, Any], profile: str, backends: list[str] | None = None
) -> list[dict[str, Any]]:
    if profile not in PROFILES:
        raise PackageMatrixError(f"unknown CI profile: {profile}")
    selected = set(backends) if backends else None
    if selected is not None:
        unknown = sorted(selected - {entry["id"] for entry in matrix["backends"]})
        if unknown:
            raise PackageMatrixError(f"unknown packaging backend: {', '.join(unknown)}")
    rows: list[dict[str, Any]] = []
    for entry in matrix["backends"]:
        if selected is not None and entry["id"] not in selected:
            continue
        if profile not in entry["profiles"]:
            continue
        for target in entry["targets"]:
            if profile not in target["profiles"]:
                continue
            rows.append(
                {
                    "id": target["id"],
                    "backend": entry["id"],
                    "package_kind": entry["package_kind"],
                    "status": entry["status"],
                    "lifecycle_state": entry["lifecycle_state"],
                    "required_for_release": entry["required_for_release"],
                    "wave": entry["wave"],
                    "profile": profile,
                    "platform": target["platform"],
                    "arch": target["arch"],
                    "runner": target["runner"],
                    "container": target["container"],
                    "container_digest": target["container_digest"],
                    "required_checks": list(entry["required_checks"][profile]),
                }
            )
    if selected is not None and not rows:
        raise PackageMatrixError(
            f"no target runs backends {sorted(selected)} in profile {profile}"
        )
    return sorted(rows, key=lambda row: row["id"])


def profile_backends(matrix: dict[str, Any], profile: str) -> list[str]:
    return sorted({row["backend"] for row in expand(matrix, profile)})


def _pairs(values: list[str], context: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for item in values:
        key, separator, value = item.partition("=")
        if not separator or not key.strip() or not value.strip():
            raise PackageMatrixError(f"{context} must be given as id=value: {item!r}")
        if key in result:
            raise PackageMatrixError(f"{context} repeats id {key}")
        result[key] = value
    return result


def check_plan(
    matrix: dict[str, Any],
    identifier: str,
    profile: str,
    *,
    default_status: str,
    statuses: dict[str, str],
    evidence_refs: dict[str, str],
    details: dict[str, str],
) -> list[dict[str, Any]]:
    """Every check a backend owes, with its honest status; unknown ids are refused."""
    entry = backend(matrix, identifier)
    if profile not in entry["profiles"]:
        raise PackageMatrixError(f"backend {identifier} does not run in profile {profile}")
    declared = list(entry["checks"])
    for context, mapping in (
        ("status", statuses),
        ("evidence reference", evidence_refs),
        ("detail", details),
    ):
        unknown = sorted(set(mapping) - set(declared))
        if unknown:
            raise PackageMatrixError(f"{context} names checks {identifier} does not declare: {unknown}")
    for status in (default_status, *statuses.values()):
        if status not in RESULTS:
            raise PackageMatrixError(f"unsupported check status: {status}")
    vocabulary = check_vocabulary(matrix)
    return [
        {
            "id": check,
            "status": statuses.get(check, default_status),
            "command": vocabulary[check]["description"],
            "category": vocabulary[check]["category"],
            "evidence_ref": evidence_refs.get(check),
            "detail": details.get(check),
        }
        for check in declared
    ]


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    commands = result.add_subparsers(dest="command", required=True)

    commands.add_parser("validate")

    expansion = commands.add_parser("expand")
    expansion.add_argument("--profile", choices=PROFILES, required=True)
    expansion.add_argument("--backend", action="append", default=[])
    expansion.add_argument("--format", choices=("json", "github"), default="json")
    expansion.add_argument("--output", type=Path)
    expansion.add_argument("--replace", action="store_true")

    listing = commands.add_parser("backends")
    listing.add_argument("--profile", choices=PROFILES, required=True)

    plan = commands.add_parser("check-plan")
    plan.add_argument("--backend", required=True)
    plan.add_argument("--profile", choices=PROFILES, required=True)
    plan.add_argument("--default-status", choices=RESULTS, default="NOT_RUN")
    plan.add_argument("--status", action="append", default=[])
    plan.add_argument("--evidence-ref", action="append", default=[])
    plan.add_argument("--detail", action="append", default=[])
    plan.add_argument("--output", type=Path)
    plan.add_argument("--replace", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        matrix = load_matrix(arguments.matrix)
        if arguments.command == "validate":
            print(
                f"package matrix OK ({len(matrix['backends'])} backends, "
                f"{len(matrix['lifecycle_checks'])} checks)"
            )
        elif arguments.command == "expand":
            rows = expand(matrix, arguments.profile, arguments.backend or None)
            value: Any = {"include": rows} if arguments.format == "github" else rows
            if arguments.output is None:
                print(json.dumps(value, indent=2, sort_keys=True))
            else:
                write_json(arguments.output, value, replace=arguments.replace)
                print(arguments.output)
        elif arguments.command == "backends":
            for identifier in profile_backends(matrix, arguments.profile):
                print(identifier)
        else:
            plan = check_plan(
                matrix,
                arguments.backend,
                arguments.profile,
                default_status=arguments.default_status,
                statuses=_pairs(arguments.status, "check status"),
                evidence_refs=_pairs(arguments.evidence_ref, "check evidence reference"),
                details=_pairs(arguments.detail, "check detail"),
            )
            if arguments.output is None:
                print(json.dumps(plan, indent=2, sort_keys=True))
            else:
                write_json(arguments.output, plan, replace=arguments.replace)
                print(arguments.output)
    except (PackageFrameworkError, PackageMatrixError, OSError) as error:
        print(f"package matrix FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
