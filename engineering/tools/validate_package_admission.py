#!/usr/bin/env python3
"""Bind package evidence to the exact sealed bytes before any release admission.

Publishing a package must never mean rebuilding it. This hook ties four
independent documents together and refuses the combination the moment they
disagree:

* the release context, which owns version, commit and package revision;
* the artifact manifest, whose parent source digest must be the digest of the
  sealed candidate source archive, not of a local checkout;
* every package evidence document, whose artifact subject must be present in the
  manifest with identical bytes, so a package rebuilt between verify and publish
  shows up as byte drift;
* the N-1 upgrade baseline, which decides the only honest package-upgrade status
  and therefore whether an initial-baseline claim is true.

The result is a report, not a gate promotion: `admitted` is true only when
nothing blocks, and every shortfall is named in `blocking` so an open gate stays
visible instead of being rounded up to a pass.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path
from typing import Any

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:  # release_bundle is a flat sibling module
    sys.path.insert(0, str(_TOOLS))
if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(_TOOLS.parents[1]))

from release_bundle import BundleError, verify_seal  # noqa: E402

from engineering.tools.generate_artifact_manifest import (  # noqa: E402
    ArtifactManifestError,
    validate_manifest,
)
from engineering.tools.generate_package_matrix import (  # noqa: E402
    PackageMatrixError,
    backend as matrix_backend,
    load_matrix,
)
from engineering.tools.generate_release_context import (  # noqa: E402
    ReleaseContextError,
    load_release_context,
)
from engineering.tools.package_framework import (  # noqa: E402
    MATRIX_PATH,
    PROFILES,
    PackageFrameworkError,
    digest,
    read_json,
    utc_now,
    validate_against_schema,
    write_json,
)
from engineering.tools.upgrade_baseline import (  # noqa: E402
    UpgradeBaselineError,
    load_baseline,
)
from engineering.tools.validate_package_evidence import (  # noqa: E402
    PackageEvidenceError,
    validate_evidence,
)


SCHEMA_NAME = "package-admission.schema.json"
SDK_MATRIX_SCHEMA_NAME = "installed-sdk-matrix.schema.json"
CANDIDATE_SEAL = "candidate-seal.json"
SEAL_ENVIRONMENT = "CANDIDATE_SEAL_SHA256"
UPGRADE_CHECK = "package-upgrade"


class PackageAdmissionError(RuntimeError):
    pass


def load_sdk_matrix_report(path: Path) -> dict[str, Any]:
    """The cross-SDK post-install report; a NOT_RUN is a fact, never a pass."""
    if path.is_symlink() or not path.is_file():
        raise PackageAdmissionError(f"installed SDK matrix report is missing: {path}")
    value = read_json(path)
    # The schema refuses a PASS without a package-installed daemon, so a report
    # that validates can never over-claim its own subject.
    validate_against_schema(value, SDK_MATRIX_SCHEMA_NAME, "installed SDK matrix report")
    return value


def admit_candidate(
    candidate: Path, product_version: str, *, seal_sha256: str | None = None
) -> dict[str, str]:
    """Admit the sealed candidate exactly as the native package producers do."""
    if candidate.is_symlink() or not candidate.is_dir():
        raise PackageAdmissionError("candidate directory must be regular")
    candidate = candidate.resolve()
    seal = candidate / CANDIDATE_SEAL
    if seal.is_symlink() or not seal.is_file():
        raise PackageAdmissionError(f"candidate seal is missing: {seal}")
    actual = digest(seal)
    expected = seal_sha256 or os.environ.get(SEAL_ENVIRONMENT)
    if expected and actual != expected:
        raise PackageAdmissionError(
            f"candidate seal digest mismatch: expected {expected}, got {actual}"
        )
    verify_seal(candidate, CANDIDATE_SEAL)
    source = candidate / f"GlifiStore-{product_version}.tar.xz"
    if source.is_symlink() or not source.is_file():
        raise PackageAdmissionError(f"sealed source archive is missing: {source.name}")
    return {
        "seal_sha256": actual,
        "source_name": source.name,
        "source_sha256": digest(source),
    }


def _upgrade_status(evidence: dict[str, Any]) -> str | None:
    for check in evidence["checks"]:
        if check["id"] == UPGRADE_CHECK:
            return str(check["status"])
    return None


def _upgrade_blocking(
    name: str, backend: str, declared: bool, status: str | None, baseline: dict[str, Any]
) -> list[dict[str, str]]:
    """The only place where an initial-baseline claim can be confirmed or refused."""
    if not declared:
        return []
    if status is None:
        return [
            {
                "id": "upgrade-status-missing",
                "detail": f"{name} omits the {UPGRADE_CHECK} row the {backend} backend declares",
            }
        ]
    expected = baseline["upgrade_check_status"]
    initial = "NOT_APPLICABLE_INITIAL_BASELINE"
    if expected == initial:
        if status != initial:
            return [
                {
                    "id": "upgrade-status-disagrees-with-baseline",
                    "detail": (
                        f"{name} reports {UPGRADE_CHECK}={status} while no release precedes "
                        f"{baseline['product_version']}"
                    ),
                }
            ]
        return []
    blocking: list[dict[str, str]] = []
    if status == initial:
        blocking.append(
            {
                "id": "upgrade-initial-baseline-contradicted",
                "detail": (
                    f"{name} claims an initial baseline while {baseline['reason']}"
                ),
            }
        )
    if expected == "BLOCKED":
        blocking.append(
            {
                "id": "upgrade-baseline-blocked",
                "detail": (
                    f"no sealed N-1 release is admissible for {name}: {baseline['reason']}"
                ),
            }
        )
        if status == "PASS":
            blocking.append(
                {
                    "id": "upgrade-passed-without-baseline",
                    "detail": f"{name} reports a passing upgrade with no admissible N-1 release",
                }
            )
    elif status != "PASS":
        blocking.append(
            {
                "id": "upgrade-not-executed",
                "detail": (
                    f"{name} reports {UPGRADE_CHECK}={status} while the sealed baseline "
                    f"{baseline['selected']['tag']} is admissible"
                ),
            }
        )
    return blocking


def build_report(
    *,
    context_path: Path,
    manifest_path: Path,
    evidence_paths: list[Path],
    candidate: Path,
    baseline_path: Path,
    profile: str,
    artifact_root: Path | None = None,
    matrix_path: Path = MATRIX_PATH,
    sdk_matrix_path: Path | None = None,
    seal_sha256: str | None = None,
) -> dict[str, Any]:
    if profile not in PROFILES:
        raise PackageAdmissionError(f"unsupported CI profile: {profile}")
    if not evidence_paths:
        raise PackageAdmissionError("an admission without package evidence proves nothing")
    context = load_release_context(context_path)
    matrix = load_matrix(matrix_path)
    baseline = load_baseline(baseline_path)
    if baseline["product_version"] != context["product_version"]:
        raise PackageAdmissionError("the upgrade baseline was resolved for a different version")
    if baseline["abi_major"] != context["abi"]["major"]:
        raise PackageAdmissionError("the upgrade baseline was resolved for a different ABI major")

    root = artifact_root or manifest_path.resolve().parent
    manifest = validate_manifest(
        read_json(manifest_path), artifact_root=root, context=context
    )
    if manifest["profile"] != profile:
        raise PackageAdmissionError(
            f"the artifact manifest was produced for profile {manifest['profile']}"
        )
    sealed = admit_candidate(candidate, context["product_version"], seal_sha256=seal_sha256)

    blocking: list[dict[str, str]] = []
    parent = manifest["parent_source"]
    if parent["name"] != sealed["source_name"] or parent["sha256"] != sealed["source_sha256"]:
        blocking.append(
            {
                "id": "candidate-source-mismatch",
                "detail": (
                    f"the artifact manifest was derived from {parent['name']}@"
                    f"{parent['sha256'][:12]} but the sealed candidate carries "
                    f"{sealed['source_name']}@{sealed['source_sha256'][:12]}"
                ),
            }
        )
    recorded = {entry["name"]: entry["sha256"] for entry in manifest["artifacts"]}

    evidence_rows: list[dict[str, Any]] = []
    for path in evidence_paths:
        # The manifest, already verified against the bytes on disk, is the digest
        # authority: evidence must agree with it rather than be re-hashed against a
        # directory it may not share.
        value = validate_evidence(path, context=context, matrix=matrix)
        backend = value["backend"]
        entry = matrix_backend(matrix, backend)
        required = bool(entry["required_for_release"])
        subject = value["subject"]
        status = _upgrade_status(value)
        name = path.name
        if subject["kind"] == "artifact":
            if subject["name"] not in recorded:
                blocking.append(
                    {
                        "id": "evidence-subject-not-in-manifest",
                        "detail": (
                            f"{name} reports {subject['name']}, which the artifact manifest does "
                            "not record"
                        ),
                    }
                )
            elif recorded[subject["name"]] != subject["sha256"]:
                blocking.append(
                    {
                        "id": "evidence-subject-byte-drift",
                        "detail": (
                            f"{name} reports {subject['name']}@{subject['sha256'][:12]} while the "
                            f"artifact manifest records @{recorded[subject['name']][:12]}"
                        ),
                    }
                )
        elif required:
            blocking.append(
                {
                    "id": "evidence-subject-is-a-source-tree",
                    "detail": (
                        f"{name} describes a source tree, so no {backend} artifact bytes can be "
                        "admitted from it"
                    ),
                }
            )
        producer = value["producer"]
        if profile == "release" and (
            producer["workflow"] == "local-unattested" or producer["run_id"] == "local"
        ):
            blocking.append(
                {
                    "id": "evidence-not-retained",
                    "detail": f"{name} was produced locally, so the release profile cannot cite it",
                }
            )
        if required and value["result"] != "PASS":
            blocking.append(
                {
                    "id": "evidence-not-passing",
                    "detail": (
                        f"{backend} is required for release but {name} reports "
                        f"{value['result']}"
                    ),
                }
            )
        blocking.extend(
            _upgrade_blocking(name, backend, UPGRADE_CHECK in entry["checks"], status, baseline)
        )
        evidence_rows.append(
            {
                "name": name,
                "backend": backend,
                "profile": value["profile"],
                "stage": value["stage"],
                "result": value["result"],
                "lifecycle_state": value["lifecycle_state"],
                "required_for_release": required,
                "subject_name": subject["name"],
                "subject_sha256": subject["sha256"],
                "upgrade_status": status,
            }
        )

    sdk_matrix: dict[str, Any] | None = None
    if sdk_matrix_path is None:
        blocking.append(
            {
                "id": "installed-sdk-matrix-missing",
                "detail": (
                    "no cross-SDK post-install report was supplied, so no SDK was proven against "
                    "a package-installed daemon"
                ),
            }
        )
    else:
        report = load_sdk_matrix_report(sdk_matrix_path)
        sdk_matrix = {
            "result": report["result"],
            "package_installed": report["package_installed"],
            "reason": report["reason"],
        }
        if not report["package_installed"]:
            blocking.append(
                {
                    "id": "installed-sdk-matrix-not-package-installed",
                    "detail": f"the cross-SDK matrix ran without a packaged daemon: {report['reason']}",
                }
            )
        elif report["result"] != "PASS":
            blocking.append(
                {
                    "id": "installed-sdk-matrix-not-passing",
                    "detail": f"the cross-SDK matrix reports {report['result']}: {report['reason']}",
                }
            )

    limitations = [
        "This report records whether bytes may be admitted; it promotes no assurance gate and "
        "closes no requirement on its own."
    ]
    if profile != "release":
        limitations.append(
            f"Computed for the {profile} profile: only the release profile binds publishable bytes."
        )
    optional = sorted(
        {row["backend"] for row in evidence_rows if not row["required_for_release"]}
    )
    if optional:
        limitations.append(
            "Backends not yet required for release cannot admit a release artifact: "
            + ", ".join(optional)
            + "."
        )
    limitations.extend(baseline["limitations"])

    report = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "profile": profile,
        "product_version": context["product_version"],
        "package_revision": context["package_revision"],
        "git_sha": context["git"]["commit"],
        "candidate": sealed,
        "artifact_manifest": {
            "name": manifest_path.name,
            "parent_source_name": parent["name"],
            "parent_source_sha256": parent["sha256"],
            "artifacts": [
                {
                    "id": entry["id"],
                    "name": entry["name"],
                    "sha256": entry["sha256"],
                    "backend": entry["backend"],
                }
                for entry in manifest["artifacts"]
            ],
        },
        "evidence": sorted(evidence_rows, key=lambda row: row["name"]),
        "upgrade_baseline": {
            "available": baseline["available"],
            "upgrade_check_status": baseline["upgrade_check_status"],
            "tag": None if baseline["selected"] is None else baseline["selected"]["tag"],
        },
        "installed_sdk_matrix": sdk_matrix,
        "admitted": not blocking,
        "blocking": sorted(blocking, key=lambda item: (item["id"], item["detail"])),
        "limitations": limitations,
    }
    validate_against_schema(report, SCHEMA_NAME, "package admission")
    return report


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--release-context", type=Path, required=True)
    result.add_argument("--artifact-manifest", type=Path, required=True)
    result.add_argument("--artifact-root", type=Path)
    result.add_argument("--evidence", type=Path, action="append", default=[])
    result.add_argument("--candidate", type=Path, required=True)
    result.add_argument("--candidate-seal-sha256")
    result.add_argument("--upgrade-baseline", type=Path, required=True)
    result.add_argument("--installed-sdk-report", type=Path)
    result.add_argument("--profile", choices=PROFILES, required=True)
    result.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    result.add_argument("--output", type=Path)
    result.add_argument("--replace", action="store_true")
    result.add_argument(
        "--allow-blocking",
        action="store_true",
        help="write the report and exit zero even when admission is blocked",
    )
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        report = build_report(
            context_path=arguments.release_context,
            manifest_path=arguments.artifact_manifest,
            evidence_paths=list(arguments.evidence),
            candidate=arguments.candidate,
            baseline_path=arguments.upgrade_baseline,
            profile=arguments.profile,
            artifact_root=arguments.artifact_root,
            matrix_path=arguments.matrix,
            sdk_matrix_path=arguments.installed_sdk_report,
            seal_sha256=arguments.candidate_seal_sha256,
        )
        if arguments.output is None:
            print(json.dumps(report, indent=2, sort_keys=True))
        else:
            write_json(arguments.output, report, replace=arguments.replace)
            print(arguments.output)
        if not report["admitted"]:
            for item in report["blocking"]:
                print(f"package admission BLOCKED [{item['id']}]: {item['detail']}", file=sys.stderr)
            return 0 if arguments.allow_blocking else 1
    except (
        ArtifactManifestError,
        BundleError,
        KeyError,
        OSError,
        PackageAdmissionError,
        PackageEvidenceError,
        PackageFrameworkError,
        PackageMatrixError,
        ReleaseContextError,
        UpgradeBaselineError,
    ) as error:
        print(f"package admission FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
