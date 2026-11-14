#!/usr/bin/env python3
"""Resolve and close the GlifiStore packaging CI plan, fail-closed.

One tool decides everything a workflow would otherwise restate in YAML: which
profile a GitHub Actions event runs, whether a pull request may skip packaging
CI, how deep the lifecycle may go on a hosted runner, how long each profile
retains its evidence, and which matrix rows the strategy expands to. The rows
themselves come from engineering/tools/generate_package_matrix.py, so no
workflow ever carries a second copy of the backend matrix or of a version.

`close` is the other half of the contract: a profile run is only closed when
every planned row retained exactly one package evidence file that still
validates against the matrix and the release context that produced it. A missing
row is a failure, never a silent pass.
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
    expand,
    load_matrix,
)
from engineering.tools.generate_release_context import ReleaseContextError, load_release_context
from engineering.tools.package_framework import (
    MATRIX_PATH,
    PROFILES,
    PackageFrameworkError,
    write_json,
)
from engineering.tools.validate_package_evidence import (
    PackageEvidenceError,
    evidence_filename,
    validate_evidence,
)

# Which profile each event may run. A tag push is absent on purpose: tags belong
# to release.yml, which calls this workflow with an explicit release profile and
# the sealed candidate bytes.
EVENT_PROFILES = {"pull_request": "pr", "push": "main", "schedule": "nightly"}
MAIN_REFS = ("refs/heads/main", "main")
# A manual run may deepen a branch profile but may never claim the release
# profile: that one only exists with a sealed candidate behind it.
DISPATCH_PROFILES = ("pr", "main", "nightly")
CALL_PROFILES = PROFILES

# Evidence retention per profile: short on pull requests, long enough on main
# and nightly to diagnose a regression, and aligned with release.yml on release.
RETENTION_DAYS = {"pr": 7, "main": 30, "nightly": 30, "release": 14}

# Lifecycle depth per profile. Pull requests and main stay structural so a
# contributor (and the push-to-main gate) gets metadata feedback without the
# first unproven container install failing closed the whole tree. Nightly and
# release opt into the digest-pinned container — the only place a hosted runner
# may build and install a real package — once that path is retained as evidence.
CONTAINER_PROFILES = ("nightly", "release")
CONTAINER_BACKENDS = ("deb", "rpm")
# MacPorts and Homebrew need a host that already carries the package manager and
# accepts installs. No hosted runner is admitted automatically; a maintainer opts
# in per run with --allow-native and owns the consequences.
NATIVE_BACKENDS = ("homebrew", "macports")

STRUCTURAL_TIMEOUT_MINUTES = 25
CONTAINER_TIMEOUT_MINUTES = 150
NATIVE_TIMEOUT_MINUTES = 120
# The step must give up before the job does, so a hung phase still uploads what
# it produced instead of losing the whole run to a job-level cancellation.
STEP_TIMEOUT_MARGIN_MINUTES = 5

STAGE = "full"
# Which slice of the matrix a caller may ask for. "optional" is the slice release.yml
# uses: the backends that are required_for_release: false and therefore never admitted
# as a release artifact.
BACKEND_SELECTIONS = {"all": None, "optional": False, "required": True}

# Change detection, centralised. Anything under these paths can change what a
# package contains, how it is built, installed, verified or removed, so it always
# runs the packaging profiles.
PACKAGING_PATHS = (
    ".github/workflows/package-ci.yml",
    ".github/workflows/release-candidate.yml",
    ".github/workflows/release.yml",
    "ABI_VERSION",
    "CMakeLists.txt",
    "CMakePresets.json",
    "VERSION",
    "abi/",
    "cmake/",
    "engineering/distribution/",
    "engineering/schemas/",
    "engineering/tools/",
    "include/",
    "packaging/",
    "scripts/",
    "src/",
    "tests/abi/",
    "tests/consumer/",
)
# The only changes that may skip the packaging profiles. Everything not listed
# here runs them: the default is to run, and the skip list is the exception that
# has to be argued for.
DOCUMENTATION_ONLY_PATHS = (
    ".github/ISSUE_TEMPLATE/",
    ".github/PULL_REQUEST_TEMPLATE.md",
    "AGENTS.md",
    "CHANGELOG.md",
    "CODE_OF_CONDUCT.md",
    "CONTRIBUTING.md",
    "LICENSE",
    "LICENSES/",
    "NOTICE",
    "README.md",
    "SECURITY.md",
    "docs/",
)


class PackageCiPlanError(RuntimeError):
    pass


def _matches(path: str, prefixes: tuple[str, ...]) -> str | None:
    for prefix in prefixes:
        if path == prefix or (prefix.endswith("/") and path.startswith(prefix)):
            return prefix
    return None


def resolve_profile(event: str, *, ref: str, profile_input: str) -> str:
    """The single mapping from a GitHub event to a packaging profile."""
    if event == "workflow_dispatch":
        if profile_input not in DISPATCH_PROFILES:
            raise PackageCiPlanError(
                "a manual packaging run must choose one of "
                f"{list(DISPATCH_PROFILES)}; the release profile is driven by release.yml "
                "with sealed candidate bytes"
            )
        return profile_input
    if event == "workflow_call":
        if profile_input not in CALL_PROFILES:
            raise PackageCiPlanError(
                f"a called packaging run must pass one of {list(CALL_PROFILES)}"
            )
        return profile_input
    if event not in EVENT_PROFILES:
        raise PackageCiPlanError(f"no packaging profile is defined for event {event!r}")
    if event == "push" and ref not in MAIN_REFS:
        raise PackageCiPlanError(
            f"the main packaging profile only runs for {MAIN_REFS[0]}, not {ref!r}"
        )
    return EVENT_PROFILES[event]


def changed_paths(source: Path | None) -> list[str] | None:
    """The changed-path list, or None when change detection is unavailable."""
    if source is None:
        return None
    if source.is_symlink() or not source.is_file():
        return None
    paths = [line.strip() for line in source.read_text(encoding="utf-8").splitlines()]
    return [path for path in paths if path]


def scope(profile: str, paths: list[str] | None) -> tuple[bool, str]:
    """Whether this run must execute, and the reason it may be skipped."""
    if profile != "pr":
        return True, f"the {profile} profile always runs the full planned matrix"
    if paths is None:
        return True, "change detection was unavailable, so the packaging profile runs by default"
    if not paths:
        return True, "no changed path was reported, so the packaging profile runs by default"
    for path in paths:
        matched = _matches(path, PACKAGING_PATHS)
        if matched is not None:
            return True, f"{path} changes packaging-relevant authority ({matched})"
    outside = [path for path in paths if _matches(path, DOCUMENTATION_ONLY_PATHS) is None]
    if outside:
        return True, (
            "changed paths fall outside the documented documentation-only set: "
            f"{', '.join(sorted(outside)[:5])}"
        )
    return False, (
        f"all {len(paths)} changed paths are documentation-only and cannot alter a package"
    )


def _lifecycle_depth(row: dict[str, Any], profile: str, *, allow_native: bool) -> dict[str, Any]:
    container = (
        profile in CONTAINER_PROFILES
        and row["backend"] in CONTAINER_BACKENDS
        and bool(row["container"])
        and bool(row["container_digest"])
    )
    native = allow_native and row["backend"] in NATIVE_BACKENDS
    if native:
        timeout = NATIVE_TIMEOUT_MINUTES
        depth = "native"
    elif container:
        timeout = CONTAINER_TIMEOUT_MINUTES
        depth = "container"
    else:
        timeout = STRUCTURAL_TIMEOUT_MINUTES
        depth = "structural"
    return {
        "lifecycle_depth": depth,
        "container_lifecycle": container,
        "native_lifecycle": native,
        "timeout_minutes": timeout,
        "step_timeout_minutes": timeout - STEP_TIMEOUT_MARGIN_MINUTES,
        "stage": STAGE,
        "evidence_file": evidence_filename(row["backend"], profile, STAGE),
    }


def select_rows(
    matrix: dict[str, Any],
    profile: str,
    *,
    selection: str = "all",
    allow_native: bool = False,
) -> list[dict[str, Any]]:
    if selection not in BACKEND_SELECTIONS:
        raise PackageCiPlanError(
            f"--backends must be one of {sorted(BACKEND_SELECTIONS)}"
        )
    rows = expand(matrix, profile)
    wanted = BACKEND_SELECTIONS[selection]
    if wanted is not None:
        rows = [row for row in rows if row["required_for_release"] is wanted]
    if not rows:
        raise PackageCiPlanError(
            f"no packaging target matches profile {profile} with backend selection {selection}"
        )
    return [row | _lifecycle_depth(row, profile, allow_native=allow_native) for row in rows]


def artifact_name(prefix: str, profile: str, identifier: str, suffix: str) -> str:
    parts = [part for part in (prefix, profile, identifier, suffix) if part]
    return "-".join(parts)


def build_plan(
    *,
    event: str,
    ref: str,
    profile_input: str,
    changed_from: Path | None,
    selection: str,
    allow_native: bool,
    matrix_path: Path = MATRIX_PATH,
) -> dict[str, Any]:
    profile = resolve_profile(event, ref=ref, profile_input=profile_input)
    matrix = load_matrix(matrix_path)
    paths = changed_paths(changed_from)
    run, reason = scope(profile, paths)
    rows = select_rows(matrix, profile, selection=selection, allow_native=allow_native)
    return {
        "schema_version": 1,
        "event": event,
        "ref": ref,
        "profile": profile,
        "run": run,
        "reason": reason,
        "changed_paths": paths,
        "retention_days": RETENTION_DAYS[profile],
        "requires_sealed_artifacts": matrix["profiles"][profile]["requires_sealed_artifacts"],
        "backend_selection": selection,
        "backends": sorted({row["backend"] for row in rows}),
        "matrix": {"include": rows},
    }


def github_outputs(plan: dict[str, Any]) -> str:
    """The plan reduced to the handful of scalars a workflow strategy consumes."""
    return "".join(
        f"{name}={value}\n"
        for name, value in (
            ("profile", plan["profile"]),
            ("run", "true" if plan["run"] else "false"),
            ("reason", plan["reason"]),
            ("retention-days", plan["retention_days"]),
            ("backends", " ".join(plan["backends"])),
            ("matrix", json.dumps(plan["matrix"], sort_keys=True, separators=(",", ":"))),
        )
    )


def _evidence_path(directory: Path, filename: str) -> Path:
    matches = sorted(path for path in directory.rglob(filename) if not path.is_symlink())
    if len(matches) != 1:
        raise PackageCiPlanError(
            f"expected exactly one {filename} under {directory.name}, found {len(matches)}"
        )
    return matches[0]


def close(
    plan: dict[str, Any],
    *,
    evidence_root: Path,
    artifact_prefix: str,
    artifact_suffix: str,
    require_ci: bool,
    expect_commit: str | None,
    matrix_path: Path = MATRIX_PATH,
) -> dict[str, Any]:
    """Refuse to close a profile whose planned rows did not retain valid evidence."""
    if not plan["run"]:
        raise PackageCiPlanError("a skipped packaging plan has nothing to close")
    matrix = load_matrix(matrix_path)
    profile = plan["profile"]
    closed: list[dict[str, Any]] = []
    failures: list[str] = []
    for row in plan["matrix"]["include"]:
        name = artifact_name(artifact_prefix, profile, row["id"], artifact_suffix)
        directory = evidence_root / name
        if directory.is_symlink() or not directory.is_dir():
            failures.append(f"{row['id']}: no retained evidence directory {name}")
            continue
        try:
            path = _evidence_path(directory, row["evidence_file"])
            context_path = directory / "release-context.json"
            context = load_release_context(context_path) if context_path.is_file() else None
            if context is not None and expect_commit and context["git"]["commit"] != expect_commit:
                raise PackageCiPlanError(
                    f"retained evidence describes commit {context['git']['commit']}, "
                    f"not {expect_commit}"
                )
            evidence = validate_evidence(
                path,
                context=context,
                matrix=matrix,
                expected_backend=row["backend"],
                expected_profile=profile,
                artifact_root=path.parent,
                require_ci=require_ci,
            )
        except (
            PackageCiPlanError,
            PackageEvidenceError,
            PackageFrameworkError,
            PackageMatrixError,
            ReleaseContextError,
            OSError,
        ) as error:
            failures.append(f"{row['id']}: {error}")
            continue
        closed.append(
            {
                "id": row["id"],
                "backend": row["backend"],
                "platform": row["platform"],
                "arch": row["arch"],
                "lifecycle_depth": row["lifecycle_depth"],
                "artifact": name,
                "evidence": str(path.relative_to(evidence_root)),
                "result": evidence["result"],
                "lifecycle_state": evidence["lifecycle_state"],
                "required_for_release": row["required_for_release"],
            }
        )
        if evidence["result"] == "FAIL":
            failures.append(f"{row['id']}: package evidence reports FAIL")
    if failures:
        raise PackageCiPlanError(
            "the packaging profile did not close: " + "; ".join(failures)
        )
    return {
        "schema_version": 1,
        "profile": profile,
        "event": plan["event"],
        "commit": expect_commit,
        "rows": closed,
        "results": sorted({entry["result"] for entry in closed}),
    }


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    commands = result.add_subparsers(dest="command", required=True)

    planner = commands.add_parser("plan")
    planner.add_argument("--event", required=True)
    planner.add_argument("--ref", default="")
    planner.add_argument("--profile-input", default="")
    planner.add_argument("--changed-from", type=Path)
    planner.add_argument("--backends", choices=sorted(BACKEND_SELECTIONS), default="all")
    planner.add_argument("--allow-native", action="store_true")
    planner.add_argument("--output", type=Path)
    planner.add_argument("--github-output", type=Path)
    planner.add_argument("--replace", action="store_true")

    closer = commands.add_parser("close")
    closer.add_argument("--plan", type=Path, required=True)
    closer.add_argument("--evidence-root", type=Path, required=True)
    closer.add_argument("--artifact-prefix", default="package")
    closer.add_argument("--artifact-suffix", default="")
    closer.add_argument("--expect-commit")
    closer.add_argument("--require-ci", action="store_true")
    closer.add_argument("--output", type=Path)
    closer.add_argument("--replace", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "plan":
            plan = build_plan(
                event=arguments.event,
                ref=arguments.ref,
                profile_input=arguments.profile_input,
                changed_from=arguments.changed_from,
                selection=arguments.backends,
                allow_native=arguments.allow_native,
                matrix_path=arguments.matrix,
            )
            if arguments.github_output is not None:
                with arguments.github_output.open("a", encoding="utf-8") as stream:
                    stream.write(github_outputs(plan))
            if arguments.output is None:
                print(json.dumps(plan, indent=2, sort_keys=True))
            else:
                write_json(arguments.output, plan, replace=arguments.replace)
                print(
                    f"package CI plan: profile={plan['profile']} run={plan['run']} "
                    f"rows={len(plan['matrix']['include'])} reason={plan['reason']}"
                )
        else:
            plan = json.loads(arguments.plan.read_text(encoding="utf-8"))
            closure = close(
                plan,
                evidence_root=arguments.evidence_root,
                artifact_prefix=arguments.artifact_prefix,
                artifact_suffix=arguments.artifact_suffix,
                require_ci=arguments.require_ci,
                expect_commit=arguments.expect_commit,
                matrix_path=arguments.matrix,
            )
            for entry in closure["rows"]:
                print(
                    f"PACKAGE-CI-CLOSE {closure['profile']} {entry['id']} "
                    f"{entry['lifecycle_depth']} {entry['result']} {entry['lifecycle_state']}"
                )
            if arguments.output is not None:
                write_json(arguments.output, closure, replace=arguments.replace)
            print(f"package CI closure OK ({len(closure['rows'])} retained rows)")
    except (
        PackageCiPlanError,
        PackageEvidenceError,
        PackageFrameworkError,
        PackageMatrixError,
        ReleaseContextError,
        OSError,
        ValueError,
    ) as error:
        print(f"package CI plan FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
