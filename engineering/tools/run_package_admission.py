#!/usr/bin/env python3
"""Orchestrate Wave F package admission for CI (fail-closed, retained reports).

Resolves the SemVer N-1 baseline, builds the artifact identity graph from package
files found next to the evidence, prefers a retained cross-SDK matrix report from
evidence when present (otherwise records an honest NOT_RUN harness result), and
binds everything through validate_package_admission.py.

Accepts package-CI evidence (`backend-profile-stage-package-evidence.json`).
Native FreeBSD/OpenBSD release producers still emit release_evidence; those files
are adapted without inventing package-matrix PASS rows (package-upgrade stays
unset so the baseline check remains visible).
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.generate_artifact_manifest import (  # noqa: E402
    ArtifactManifestError,
    build_manifest,
    validate_manifest,
)
from engineering.tools.generate_package_matrix import (  # noqa: E402
    PackageMatrixError,
    backend as matrix_backend,
    load_matrix,
)
from engineering.tools.generate_release_context import (  # noqa: E402
    ReleaseContextError,
    build_context,
    load_release_context,
)
from engineering.tools.package_framework import (  # noqa: E402
    MATRIX_PATH,
    PROFILES,
    PackageFrameworkError,
    read_json,
    utc_now,
    validate_against_schema,
    write_json,
)
from engineering.tools.semver_policy import SemverError, parse  # noqa: E402
from engineering.tools.upgrade_baseline import (  # noqa: E402
    UpgradeBaselineError,
    load_baseline,
    resolve_baseline,
)
from engineering.tools.validate_package_admission import (  # noqa: E402
    PackageAdmissionError,
    admit_candidate,
    build_report,
    load_sdk_matrix_report,
    _upgrade_blocking,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
RELEASE_PACKAGE_TYPES = {"freebsd_package": "freebsd", "openbsd_package": "openbsd"}
PACKAGE_KIND = {
    "deb": "deb",
    "rpm": "rpm",
    "freebsd": "freebsd_pkg",
    "openbsd": "openbsd_tgz",
}
PACKAGE_GLOBS = {
    "deb": ("*.deb",),
    "rpm": ("*.rpm",),
    "freebsd": ("*.pkg",),
    "openbsd": ("*.tgz",),
}
DEBUG_PACKAGE = re.compile(
    r"(?:-devel|-libs|-dbgsym|-debuginfo|-debugsource|_dbgsym)", re.IGNORECASE
)


class PackageAdmissionRunError(RuntimeError):
    pass


def _file(path: Path, label: str) -> Path:
    if path.is_symlink() or not path.is_file():
        raise PackageAdmissionRunError(f"{label} is missing or not a regular file: {path}")
    return path


def write_sdk_matrix_not_run(output: Path, *, replace: bool) -> Path:
    output.parent.mkdir(parents=True, exist_ok=True)
    argv = [
        str(REPO_ROOT / "scripts" / "test-package-installed-sdk-matrix.sh"),
        "--report",
        str(output),
    ]
    if replace or output.exists():
        argv.append("--replace")
    completed = subprocess.run(argv, check=False, cwd=str(REPO_ROOT), capture_output=True, text=True)
    if not output.is_file():
        raise PackageAdmissionRunError(
            "installed SDK matrix harness wrote no report: "
            + (completed.stderr or completed.stdout or f"exit {completed.returncode}")
        )
    return output


SDK_MATRIX_RANK = {"PASS": 4, "FAIL": 3, "BLOCKED": 2, "NOT_RUN": 1}


def discover_sdk_matrix_reports(roots: list[Path]) -> list[Path]:
    found: list[Path] = []
    seen: set[Path] = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("installed-sdk-matrix.json")):
            resolved = path.resolve()
            if path.is_symlink() or not path.is_file() or resolved in seen:
                continue
            found.append(path)
            seen.add(resolved)
    return found


def prefer_or_write_sdk_matrix(
    *, evidence_roots: list[Path], output: Path, replace: bool
) -> Path:
    """Prefer a retained lifecycle report over inventing a bare NOT_RUN."""
    candidates: list[tuple[dict[str, Any], Path]] = []
    for path in discover_sdk_matrix_reports(evidence_roots):
        try:
            candidates.append((load_sdk_matrix_report(path), path))
        except (PackageFrameworkError, PackageAdmissionError):
            continue
    if not candidates:
        return write_sdk_matrix_not_run(output, replace=replace)

    def rank(item: tuple[dict[str, Any], Path]) -> tuple[int, int]:
        report, _ = item
        return (
            1 if report.get("package_installed") else 0,
            SDK_MATRIX_RANK.get(str(report.get("result")), 0),
        )

    best_report, best_path = max(candidates, key=rank)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.resolve() != best_path.resolve():
        if output.exists() and not replace:
            raise PackageAdmissionRunError(
                f"refusing to replace existing installed SDK matrix report: {output}"
            )
        write_json(output, best_report, replace=True)
    return output


def discover_evidence(roots: list[Path], profile: str, stage: str) -> tuple[list[Path], list[Path]]:
    """Return (package_ci_evidence, release_package_evidence)."""
    package_ci: list[Path] = []
    release_pkg: list[Path] = []
    expected_suffix = f"-{profile}-{stage}-package-evidence.json"
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*package-evidence.json")):
            if path.is_symlink() or not path.is_file():
                continue
            if path.name.endswith(expected_suffix):
                package_ci.append(path)
            elif path.name in ("freebsd-package-evidence.json", "openbsd-package-evidence.json"):
                release_pkg.append(path)
    return package_ci, release_pkg


def discover_artifacts(roots: list[Path], backends: list[str]) -> list[tuple[str, Path]]:
    found: list[tuple[str, Path]] = []
    seen: set[str] = set()
    for backend in backends:
        matches: list[Path] = []
        for root in roots:
            if not root.is_dir():
                continue
            for pattern in PACKAGE_GLOBS[backend]:
                matches.extend(
                    path for path in root.rglob(pattern) if path.is_file() and not path.is_symlink()
                )
        primary = [path for path in matches if DEBUG_PACKAGE.search(path.name) is None]
        for path in sorted(primary or matches, key=lambda item: item.name):
            if path.name in seen:
                continue
            found.append((backend, path))
            seen.add(path.name)
            break
    return found


def stage_artifacts(artifacts: list[tuple[str, Path]], staged: Path) -> list[tuple[str, Path]]:
    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir(parents=True, exist_ok=True)
    result: list[tuple[str, Path]] = []
    for backend, path in artifacts:
        destination = staged / path.name
        if destination.exists():
            raise PackageAdmissionRunError(f"duplicate packaged artifact name: {path.name}")
        shutil.copy2(path, destination)
        result.append((backend, destination))
    return result


def platforms_from_matrix(matrix: dict[str, Any]) -> dict[str, tuple[str, str]]:
    result: dict[str, tuple[str, str]] = {}
    for entry in matrix["backends"]:
        targets = entry.get("targets") or []
        if targets:
            result[entry["id"]] = (str(targets[0]["platform"]), str(targets[0]["arch"]))
    return result


def artifact_specs(
    artifacts: list[tuple[str, Path]], platforms: dict[str, tuple[str, str]]
) -> list[str]:
    specs: list[str] = []
    for backend, path in artifacts:
        platform, arch = platforms[backend]
        specs.append(
            f"id={backend}-package,kind={PACKAGE_KIND[backend]},platform={platform},"
            f"arch={arch},backend={backend},path={path},lifecycle_state=BUILT"
        )
    return specs


def adapt_release_row(
    path: Path, *, profile: str, context: dict[str, Any], matrix: dict[str, Any]
) -> dict[str, Any]:
    value = read_json(path)
    evidence_type = value.get("evidence_type")
    if evidence_type not in RELEASE_PACKAGE_TYPES:
        raise PackageAdmissionRunError(f"{path.name} is not freebsd/openbsd release evidence")
    backend = RELEASE_PACKAGE_TYPES[str(evidence_type)]
    if value.get("product_version") != context["product_version"]:
        raise PackageAdmissionRunError(f"{path.name} product_version mismatches the release context")
    if value.get("git_sha") != context["git"]["commit"]:
        raise PackageAdmissionRunError(f"{path.name} git_sha mismatches the release context")
    result = str(value["result"]).upper()
    if result == "PASSED":
        result = "PASS"
    subject = value["subject"]
    entry = matrix_backend(matrix, backend)
    return {
        "name": path.name,
        "backend": backend,
        "profile": profile,
        "stage": "full",
        "result": result,
        "lifecycle_state": "ADAPTED_RELEASE_EVIDENCE",
        "required_for_release": bool(entry["required_for_release"]),
        "subject_name": subject["name"],
        "subject_sha256": subject["sha256"],
        "upgrade_status": None,
        "producer": value.get("producer") or {},
    }


def blockers_for_adapted(
    row: dict[str, Any],
    *,
    recorded: dict[str, str],
    profile: str,
    matrix: dict[str, Any],
    baseline: dict[str, Any],
) -> list[dict[str, str]]:
    blocking: list[dict[str, str]] = []
    name = row["name"]
    if row["subject_name"] not in recorded:
        blocking.append(
            {
                "id": "evidence-subject-not-in-manifest",
                "detail": (
                    f"{name} reports {row['subject_name']}, which the artifact manifest does not record"
                ),
            }
        )
    elif recorded[row["subject_name"]] != row["subject_sha256"]:
        blocking.append(
            {
                "id": "evidence-subject-byte-drift",
                "detail": (
                    f"{name} reports {row['subject_name']}@{row['subject_sha256'][:12]} while the "
                    f"artifact manifest records @{recorded[row['subject_name']][:12]}"
                ),
            }
        )
    producer = row["producer"]
    if profile == "release" and (
        producer.get("workflow") == "local-unattested" or producer.get("run_id") == "local"
    ):
        blocking.append(
            {
                "id": "evidence-not-retained",
                "detail": f"{name} was produced locally, so the release profile cannot cite it",
            }
        )
    if row["required_for_release"] and row["result"] != "PASS":
        blocking.append(
            {
                "id": "evidence-not-passing",
                "detail": (
                    f"{row['backend']} is required for release but {name} reports {row['result']}"
                ),
            }
        )
    entry = matrix_backend(matrix, row["backend"])
    blocking.extend(
        _upgrade_blocking(
            name,
            row["backend"],
            "package-upgrade" in entry["checks"],
            row["upgrade_status"],
            baseline,
        )
    )
    return blocking


def run_admission(
    *,
    root: Path,
    profile: str,
    candidate: Path,
    evidence_roots: list[Path],
    artifact_roots: list[Path],
    output_dir: Path,
    context_path: Path | None,
    seal_sha256: str | None,
    allow_blocking: bool,
    replace: bool,
    matrix_path: Path = MATRIX_PATH,
) -> tuple[dict[str, Any], int]:
    if profile not in PROFILES:
        raise PackageAdmissionRunError(f"unsupported profile: {profile}")
    candidate = candidate.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    if context_path is None:
        context_path = output_dir / "release-context.json"
        write_json(context_path, build_context(root), replace=replace or not context_path.exists())
    else:
        context_path = _file(context_path, "release context")
    context = load_release_context(context_path)
    matrix = load_matrix(matrix_path)

    baseline_path = output_dir / "upgrade-baseline.json"
    baseline = resolve_baseline(
        root,
        current=parse(context["product_version"], allow_v_prefix=False),
        abi_major=context["abi"]["major"],
    )
    write_json(baseline_path, baseline, replace=replace)

    sdk_path = output_dir / "installed-sdk-matrix.json"
    prefer_or_write_sdk_matrix(
        evidence_roots=evidence_roots, output=sdk_path, replace=replace
    )

    package_ci_paths, release_paths = discover_evidence(evidence_roots, profile, "full")
    if not package_ci_paths and not release_paths:
        raise PackageAdmissionRunError("no package evidence was found to admit")

    backends = sorted(
        {
            *(read_json(path)["backend"] for path in package_ci_paths),
            *(RELEASE_PACKAGE_TYPES[str(read_json(path)["evidence_type"])] for path in release_paths),
        }
    )
    discovered = discover_artifacts(
        list(artifact_roots) + list(evidence_roots) + [candidate], backends
    )
    if not discovered:
        raise PackageAdmissionRunError("no package artifacts were found for the artifact manifest")
    staged_dir = output_dir / "artifacts"
    staged = stage_artifacts(discovered, staged_dir)

    parent_source = _file(
        candidate / f"GlifiStore-{context['product_version']}.tar.xz", "sealed source archive"
    )
    manifest_path = output_dir / "artifact-manifest.json"
    write_json(
        manifest_path,
        build_manifest(
            context_path=context_path,
            profile=profile,
            parent_source=parent_source,
            artifacts=artifact_specs(staged, platforms_from_matrix(matrix)),
            artifact_root=None,
        ),
        replace=replace,
    )

    adapted = [
        adapt_release_row(path, profile=profile, context=context, matrix=matrix)
        for path in release_paths
    ]

    if package_ci_paths:
        report = build_report(
            context_path=context_path,
            manifest_path=manifest_path,
            evidence_paths=package_ci_paths,
            candidate=candidate,
            baseline_path=baseline_path,
            profile=profile,
            artifact_root=staged_dir,
            matrix_path=matrix_path,
            sdk_matrix_path=sdk_path,
            seal_sha256=seal_sha256,
        )
    else:
        sealed = admit_candidate(candidate, context["product_version"], seal_sha256=seal_sha256)
        manifest = validate_manifest(
            read_json(manifest_path), artifact_root=staged_dir, context=context
        )
        parent = manifest["parent_source"]
        blocking: list[dict[str, str]] = []
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
        sdk = load_sdk_matrix_report(sdk_path)
        if not sdk["package_installed"]:
            blocking.append(
                {
                    "id": "installed-sdk-matrix-not-package-installed",
                    "detail": (
                        f"the cross-SDK matrix ran without a packaged daemon: {sdk['reason']}"
                    ),
                }
            )
        elif sdk["result"] != "PASS":
            blocking.append(
                {
                    "id": "installed-sdk-matrix-not-passing",
                    "detail": f"the cross-SDK matrix reports {sdk['result']}: {sdk['reason']}",
                }
            )
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
            "evidence": [],
            "upgrade_baseline": {
                "available": baseline["available"],
                "upgrade_check_status": baseline["upgrade_check_status"],
                "tag": None if baseline["selected"] is None else baseline["selected"]["tag"],
            },
            "installed_sdk_matrix": {
                "result": sdk["result"],
                "package_installed": sdk["package_installed"],
                "reason": sdk["reason"],
            },
            "admitted": False,
            "blocking": blocking,
            "limitations": [
                "This report records whether bytes may be admitted; it promotes no assurance gate "
                "and closes no requirement on its own.",
                "Admission used adapted FreeBSD/OpenBSD release_evidence without inventing "
                "package-matrix PASS rows.",
            ],
        }

    if adapted:
        recorded = {
            entry["name"]: entry["sha256"] for entry in report["artifact_manifest"]["artifacts"]
        }
        baseline_loaded = load_baseline(baseline_path)
        evidence = list(report["evidence"])
        blocking = list(report["blocking"])
        for row in adapted:
            blocking.extend(
                blockers_for_adapted(
                    row,
                    recorded=recorded,
                    profile=profile,
                    matrix=matrix,
                    baseline=baseline_loaded,
                )
            )
            evidence.append(
                {
                    "name": row["name"],
                    "backend": row["backend"],
                    "profile": row["profile"],
                    "stage": row["stage"],
                    "result": row["result"],
                    "lifecycle_state": row["lifecycle_state"],
                    "required_for_release": row["required_for_release"],
                    "subject_name": row["subject_name"],
                    "subject_sha256": row["subject_sha256"],
                    "upgrade_status": row["upgrade_status"],
                }
            )
        report = dict(report)
        report["evidence"] = sorted(evidence, key=lambda item: item["name"])
        report["blocking"] = sorted(blocking, key=lambda item: (item["id"], item["detail"]))
        report["admitted"] = not report["blocking"]
        report["limitations"] = list(report["limitations"]) + [
            "FreeBSD/OpenBSD release_evidence was adapted without inventing package-matrix PASS "
            "rows; package-upgrade stays unset until package-ci evidence covers those backends."
        ]

    if not report["evidence"]:
        raise PackageAdmissionRunError("admission produced no evidence rows")

    validate_against_schema(report, "package-admission.schema.json", "package admission")
    write_json(output_dir / "package-admission.json", report, replace=replace)
    write_json(output_dir / "release-context.json", context, replace=True)
    return report, (0 if report["admitted"] or allow_blocking else 1)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--root", type=Path, default=REPO_ROOT)
    result.add_argument("--profile", choices=PROFILES, required=True)
    result.add_argument("--candidate", type=Path, required=True)
    result.add_argument("--evidence-root", type=Path, action="append", default=[])
    result.add_argument("--artifact-root", type=Path, action="append", default=[])
    result.add_argument("--release-context", type=Path)
    result.add_argument("--candidate-seal-sha256")
    result.add_argument("--output-dir", type=Path, required=True)
    result.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    result.add_argument("--allow-blocking", action="store_true")
    result.add_argument("--replace", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    evidence_roots = list(arguments.evidence_root) or [arguments.candidate]
    artifact_roots = list(arguments.artifact_root) or evidence_roots
    try:
        report, code = run_admission(
            root=arguments.root,
            profile=arguments.profile,
            candidate=arguments.candidate,
            evidence_roots=evidence_roots,
            artifact_roots=artifact_roots,
            output_dir=arguments.output_dir,
            context_path=arguments.release_context,
            seal_sha256=arguments.candidate_seal_sha256,
            allow_blocking=arguments.allow_blocking,
            replace=arguments.replace,
            matrix_path=arguments.matrix,
        )
        print(
            f"package admission {'ADMITTED' if report['admitted'] else 'BLOCKED'} "
            f"-> {arguments.output_dir / 'package-admission.json'}"
        )
        for item in report["blocking"]:
            print(f"  [{item['id']}] {item['detail']}", file=sys.stderr)
        return code
    except (
        ArtifactManifestError,
        KeyError,
        OSError,
        PackageAdmissionError,
        PackageAdmissionRunError,
        PackageFrameworkError,
        PackageMatrixError,
        ReleaseContextError,
        SemverError,
        TypeError,
        UpgradeBaselineError,
    ) as error:
        print(f"package admission FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
