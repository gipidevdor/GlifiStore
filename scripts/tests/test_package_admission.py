from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "engineering" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from release_bundle import seal  # noqa: E402

from engineering.tools.generate_artifact_manifest import (  # noqa: E402
    ArtifactManifestError,
    build_manifest,
)
from engineering.tools.generate_package_matrix import check_plan, load_matrix  # noqa: E402
from engineering.tools.generate_release_context import build_context  # noqa: E402
from engineering.tools.package_framework import (  # noqa: E402
    PackageFrameworkError,
    encode_json,
)
from engineering.tools.semver_policy import parse  # noqa: E402
from engineering.tools.upgrade_baseline import PublishedRelease, resolve_baseline  # noqa: E402
from engineering.tools.validate_package_admission import (  # noqa: E402
    PackageAdmissionError,
    build_report,
)
from engineering.tools.validate_package_evidence import (  # noqa: E402
    PackageEvidenceError,
    emit_evidence,
)


GIT_IDENTITY = (
    "-c",
    "user.email=tests@glifistore.invalid",
    "-c",
    "user.name=GlifiStore Tests",
    "-c",
    "commit.gpgsign=false",
    "-c",
    "tag.gpgsign=false",
)
def git(repository: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository), *GIT_IDENTITY, *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def sdk_matrix_report(
    *, result: str, package_installed: bool, languages: list[str] | None = None
) -> dict:
    return {
        "schema_version": 1,
        "generated_at": "2026-01-01T00:00:00Z",
        "result": result,
        "package_installed": package_installed,
        "daemon": "/usr/local/bin/glifistored" if package_installed else None,
        "prefix": "/usr/local" if package_installed else None,
        "file_list": "/tmp/pkg-contents.txt" if package_installed else None,
        "languages": (
            languages
            if languages is not None
            else (["cpp", "python", "go", "perl", "ruby", "erlang"] if result == "PASS" else [])
        ),
        "reason": f"synthetic fixture reporting {result}",
    }


class AdmissionFixture(unittest.TestCase):
    """A synthetic candidate, artifact graph and evidence set; never a real release."""

    version = "0.1.0"
    predecessors: tuple[str, ...] = ()

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-package-admission-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.matrix = load_matrix()
        self.package = f"glifistore-{self.version}-freebsd14.3-amd64.pkg"

        self.repository = self.work / "repository"
        self.repository.mkdir()
        subprocess.run(
            ["git", "-c", "init.defaultBranch=main", "init", "-q", str(self.repository)],
            check=True,
            capture_output=True,
        )
        (self.repository / "ABI_VERSION").write_text("1.0\n", encoding="utf-8")
        for version in (*self.predecessors, self.version):
            (self.repository / "VERSION").write_text(f"{version}\n", encoding="utf-8")
            git(self.repository, "add", "VERSION", "ABI_VERSION")
            git(self.repository, "commit", "-q", "-m", f"chore(release): {version}")
            if version != self.version:
                git(self.repository, "tag", "-a", f"v{version}", "-m", f"v{version}")

        self.context_path = self.work / "release-context.json"
        self.context = build_context(self.repository)
        self.context_path.write_text(encode_json(self.context), encoding="utf-8")

        self.candidate = self.work / "candidate"
        self.candidate.mkdir()
        (self.candidate / f"GlifiStore-{self.version}.tar.xz").write_bytes(b"sealed source bytes")
        seal(self.candidate, "candidate-seal.json", "candidate")

        self.out = self.work / "out"
        self.out.mkdir()
        (self.out / self.package).write_bytes(b"packaged bytes")
        (self.out / "run.log").write_text("retained execution log\n", encoding="utf-8")

        self.baseline_path = self.baseline(index={})

    def baseline(
        self, *, index: dict[str, PublishedRelease] | None, name: str = "upgrade-baseline.json"
    ) -> Path:
        value = resolve_baseline(
            self.repository,
            current=parse(self.version, allow_v_prefix=False),
            abi_major=1,
            index=index,
        )
        path = self.work / name
        path.write_text(encode_json(value), encoding="utf-8")
        return path

    def manifest(self, *, profile: str = "main", parent: Path | None = None) -> Path:
        value = build_manifest(
            context_path=self.context_path,
            profile=profile,
            parent_source=parent or self.candidate / f"GlifiStore-{self.version}.tar.xz",
            artifacts=[
                f"id=freebsd-pkg,kind=freebsd_pkg,platform=freebsd-14,arch=amd64,"
                f"backend=freebsd,path={self.out / self.package}"
            ],
        )
        path = self.out / f"artifact-manifest-{profile}.json"
        path.write_text(encode_json(value), encoding="utf-8")
        return path

    def evidence(
        self,
        *,
        upgrade: str,
        profile: str = "main",
        stage: str = "full",
        result: str = "PASS",
        lifecycle_state: str = "UPGRADE_VERIFIED",
        subject: Path | None = None,
        backend: str = "freebsd",
    ) -> Path:
        declared = next(
            entry["checks"] for entry in self.matrix["backends"] if entry["id"] == backend
        )
        statuses = {check: "PASS" for check in declared}
        statuses["package-upgrade"] = upgrade
        plan = self.work / f"check-plan-{backend}-{profile}-{stage}.json"
        plan.write_text(
            encode_json(
                check_plan(
                    self.matrix,
                    backend,
                    profile,
                    default_status="NOT_RUN",
                    statuses=statuses,
                    evidence_refs={
                        check: "run.log" for check, value in statuses.items() if value == "PASS"
                    },
                    details={},
                )
            ),
            encoding="utf-8",
        )
        output = self.out / f"{backend}-{profile}-{stage}-package-evidence.json"
        # Fixture evidence must stay local-unattested even when Assurance runs under
        # GITHUB_RUN_ID on Actions; otherwise --require-ci / release-retention checks
        # cannot observe the local producer path they are meant to refuse.
        environment = os.environ.copy()
        for name in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_WORKFLOW_REF"):
            environment.pop(name, None)
        with mock.patch.dict(os.environ, environment, clear=True):
            emit_evidence(
                backend=backend,
                profile=profile,
                stage=stage,
                result=result,
                lifecycle_state=lifecycle_state,
                context_path=self.context_path,
                check_plan=plan,
                output=output,
                subject_path=self.out / self.package if subject is None else subject,
            )
        return output

    def report(self, **keywords) -> dict:
        """Defaults are materialised lazily: an overridden input is never rewritten."""
        keywords.setdefault("context_path", self.context_path)
        keywords.setdefault("candidate", self.candidate)
        keywords.setdefault("baseline_path", self.baseline_path)
        keywords.setdefault("profile", "main")
        if "manifest_path" not in keywords:
            keywords["manifest_path"] = self.manifest()
        if "evidence_paths" not in keywords:
            keywords["evidence_paths"] = [
                self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")
            ]
        if "sdk_matrix_path" not in keywords:
            keywords["sdk_matrix_path"] = self.sdk_report(result="PASS", package_installed=True)
        return build_report(**keywords)

    def sdk_report(self, *, name: str = "installed-sdk-matrix.json", **keywords) -> Path:
        path = self.work / name
        path.write_text(encode_json(sdk_matrix_report(**keywords)), encoding="utf-8")
        return path

    def blocking(self, report: dict) -> list[str]:
        return sorted(item["id"] for item in report["blocking"])


class FirstReleaseAdmissionTests(AdmissionFixture):
    def test_a_first_release_admits_its_initial_baseline_upgrade_row(self) -> None:
        report = self.report()
        self.assertEqual(self.blocking(report), [])
        self.assertTrue(report["admitted"])
        self.assertEqual(
            report["upgrade_baseline"],
            {
                "available": False,
                "upgrade_check_status": "NOT_APPLICABLE_INITIAL_BASELINE",
                "tag": None,
            },
        )
        self.assertEqual(report["evidence"][0]["upgrade_status"], "NOT_APPLICABLE_INITIAL_BASELINE")
        self.assertEqual(report["candidate"]["source_name"], f"GlifiStore-{self.version}.tar.xz")
        self.assertEqual(
            report["artifact_manifest"]["parent_source_sha256"],
            report["candidate"]["source_sha256"],
        )
        self.assertTrue(
            any("promotes no assurance gate" in item for item in report["limitations"])
        )

    def test_an_upgrade_row_contradicting_the_initial_baseline_is_refused(self) -> None:
        report = self.report(evidence_paths=[self.evidence(upgrade="PASS")])
        self.assertEqual(self.blocking(report), ["upgrade-status-disagrees-with-baseline"])
        self.assertFalse(report["admitted"])

    def test_a_not_run_upgrade_row_is_refused_when_nothing_precedes_this_version(self) -> None:
        report = self.report(
            evidence_paths=[self.evidence(upgrade="NOT_RUN", result="OPEN_GATE")]
        )
        self.assertIn("upgrade-status-disagrees-with-baseline", self.blocking(report))


class ExactArtifactAdmissionTests(AdmissionFixture):
    def test_a_manifest_derived_from_unsealed_bytes_is_refused(self) -> None:
        rebuild = self.work / "rebuild"
        rebuild.mkdir()
        source = rebuild / f"GlifiStore-{self.version}.tar.xz"
        source.write_bytes(b"locally rebuilt source bytes")
        report = self.report(manifest_path=self.manifest(parent=source))
        self.assertEqual(self.blocking(report), ["candidate-source-mismatch"])

    def test_an_evidence_subject_outside_the_artifact_graph_is_refused(self) -> None:
        stray = self.out / "glifistore-0.1.0-freebsd14.3-amd64-stray.pkg"
        stray.write_bytes(b"unrecorded bytes")
        report = self.report(
            evidence_paths=[
                self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE", subject=stray)
            ]
        )
        self.assertEqual(self.blocking(report), ["evidence-subject-not-in-manifest"])

    def test_publish_byte_drift_between_evidence_and_the_manifest_is_refused(self) -> None:
        evidence = self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["subject"]["sha256"] = "b" * 64
        evidence.write_text(encode_json(value), encoding="utf-8")
        report = self.report(evidence_paths=[evidence])
        self.assertEqual(self.blocking(report), ["evidence-subject-byte-drift"])

    def test_a_source_tree_subject_cannot_admit_a_required_backend(self) -> None:
        declared = next(
            entry["checks"] for entry in self.matrix["backends"] if entry["id"] == "freebsd"
        )
        plan = self.work / "source-tree-plan.json"
        plan.write_text(
            encode_json(
                check_plan(
                    self.matrix,
                    "freebsd",
                    "main",
                    default_status="NOT_RUN",
                    statuses={check: "PASS" for check in declared}
                    | {"package-upgrade": "NOT_APPLICABLE_INITIAL_BASELINE"},
                    evidence_refs={check: "run.log" for check in declared},
                    details={},
                )
            ),
            encoding="utf-8",
        )
        output = self.out / "freebsd-main-metadata-package-evidence.json"
        emit_evidence(
            backend="freebsd",
            profile="main",
            stage="metadata",
            result="PASS",
            lifecycle_state="STRUCTURAL",
            context_path=self.context_path,
            check_plan=plan,
            output=output,
        )
        report = self.report(evidence_paths=[output])
        self.assertEqual(self.blocking(report), ["evidence-subject-is-a-source-tree"])

    def test_a_tampered_candidate_seal_digest_is_refused(self) -> None:
        with self.assertRaisesRegex(PackageAdmissionError, "seal digest mismatch"):
            self.report(seal_sha256="c" * 64)

    def test_a_manifest_for_another_profile_is_refused(self) -> None:
        with self.assertRaisesRegex(PackageAdmissionError, "produced for profile"):
            self.report(manifest_path=self.manifest(profile="nightly"))

    def test_a_manifest_whose_artifact_bytes_changed_is_refused(self) -> None:
        manifest = self.manifest()
        evidence = self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")
        (self.out / self.package).write_bytes(b"different packaged bytes")
        with self.assertRaisesRegex(ArtifactManifestError, "digest mismatch"):
            self.report(manifest_path=manifest, evidence_paths=[evidence])

    def test_an_admission_without_evidence_proves_nothing(self) -> None:
        with self.assertRaisesRegex(PackageAdmissionError, "proves nothing"):
            self.report(evidence_paths=[])

    def test_a_locally_produced_evidence_document_cannot_serve_the_release_profile(self) -> None:
        # The release profile owes package-upgrade, and an initial baseline is not a
        # pass for a required check, so this evidence can only be an open gate.
        report = self.report(
            profile="release",
            manifest_path=self.manifest(profile="release"),
            evidence_paths=[
                self.evidence(
                    upgrade="NOT_APPLICABLE_INITIAL_BASELINE",
                    profile="release",
                    result="OPEN_GATE",
                    lifecycle_state="LIFECYCLE_VERIFIED",
                )
            ],
        )
        self.assertIn("evidence-not-retained", self.blocking(report))

    def test_a_non_passing_required_backend_cannot_be_admitted(self) -> None:
        report = self.report(
            evidence_paths=[
                self.evidence(
                    upgrade="NOT_APPLICABLE_INITIAL_BASELINE",
                    result="OPEN_GATE",
                    lifecycle_state="STRUCTURAL",
                )
            ]
        )
        self.assertIn("evidence-not-passing", self.blocking(report))

    def test_an_optional_backend_is_reported_as_such(self) -> None:
        declared = next(entry["checks"] for entry in self.matrix["backends"] if entry["id"] == "deb")
        plan = self.work / "deb-plan.json"
        plan.write_text(
            encode_json(
                check_plan(
                    self.matrix,
                    "deb",
                    "main",
                    default_status="NOT_RUN",
                    statuses={"structural-metadata": "PASS", "package-metadata-render": "PASS"}
                    | {"package-upgrade": "NOT_APPLICABLE_INITIAL_BASELINE"},
                    evidence_refs={"structural-metadata": "run.log", "package-metadata-render": "run.log"},
                    details={},
                )
            ),
            encoding="utf-8",
        )
        self.assertIn("package-upgrade", declared)
        output = self.out / "deb-main-full-package-evidence.json"
        emit_evidence(
            backend="deb",
            profile="main",
            stage="full",
            result="OPEN_GATE",
            lifecycle_state="STRUCTURAL",
            context_path=self.context_path,
            check_plan=plan,
            output=output,
        )
        report = self.report(evidence_paths=[output])
        self.assertEqual(self.blocking(report), [])
        self.assertFalse(report["evidence"][0]["required_for_release"])
        self.assertTrue(
            any("not yet required for release" in item for item in report["limitations"])
        )


class BlockedBaselineAdmissionTests(AdmissionFixture):
    version = "0.2.0"
    predecessors = ("0.1.0",)

    def test_a_draft_predecessor_blocks_admission_instead_of_passing_as_initial(self) -> None:
        baseline = self.baseline(
            index={
                "v0.1.0": PublishedRelease(
                    tag="v0.1.0", draft=True, prerelease=False, assets=("SHA256SUMS",)
                )
            },
            name="blocked-baseline.json",
        )
        report = self.report(
            baseline_path=baseline,
            evidence_paths=[self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")],
        )
        self.assertEqual(
            self.blocking(report),
            ["upgrade-baseline-blocked", "upgrade-initial-baseline-contradicted"],
        )
        self.assertEqual(report["upgrade_baseline"]["upgrade_check_status"], "BLOCKED")

    def test_a_passing_upgrade_without_an_admissible_baseline_is_refused(self) -> None:
        baseline = self.baseline(index={}, name="unpublished-baseline.json")
        report = self.report(
            baseline_path=baseline, evidence_paths=[self.evidence(upgrade="PASS")]
        )
        self.assertIn("upgrade-passed-without-baseline", self.blocking(report))

    def test_a_baseline_resolved_for_another_version_is_refused(self) -> None:
        stale = self.work / "stale-baseline.json"
        value = json.loads(self.baseline_path.read_text(encoding="utf-8"))
        value["product_version"] = "0.9.0"
        value["reason"] = "resolved for a version this candidate is not"
        stale.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageAdmissionError, "different version"):
            self.report(baseline_path=stale)


class InstalledSdkMatrixAdmissionTests(AdmissionFixture):
    def test_a_missing_cross_sdk_report_blocks_admission(self) -> None:
        report = self.report(sdk_matrix_path=None)
        self.assertEqual(self.blocking(report), ["installed-sdk-matrix-missing"])
        self.assertIsNone(report["installed_sdk_matrix"])

    def test_a_matrix_that_never_saw_a_packaged_daemon_blocks_admission(self) -> None:
        report = self.report(
            sdk_matrix_path=self.sdk_report(
                name="not-run.json", result="NOT_RUN", package_installed=False
            )
        )
        self.assertEqual(self.blocking(report), ["installed-sdk-matrix-not-package-installed"])

    def test_a_failing_matrix_on_a_packaged_daemon_blocks_admission(self) -> None:
        report = self.report(
            sdk_matrix_path=self.sdk_report(
                name="failed.json", result="FAIL", package_installed=True
            )
        )
        self.assertEqual(self.blocking(report), ["installed-sdk-matrix-not-passing"])

    def test_a_report_claiming_a_pass_without_a_packaged_daemon_is_refused(self) -> None:
        path = self.work / "over-claim.json"
        value = sdk_matrix_report(result="PASS", package_installed=False)
        value["languages"] = ["cpp", "python", "go", "perl", "ruby", "erlang"]
        path.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageFrameworkError, "installed-sdk-matrix.schema.json"):
            self.report(sdk_matrix_path=path)

    def test_a_report_claiming_a_pass_for_fewer_languages_is_refused(self) -> None:
        path = self.work / "partial.json"
        path.write_text(
            encode_json(
                sdk_matrix_report(
                    result="PASS", package_installed=True, languages=["cpp", "python"]
                )
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(PackageFrameworkError, "installed-sdk-matrix.schema.json"):
            self.report(sdk_matrix_path=path)


class AdmissionCommandLineTests(AdmissionFixture):
    def run_tool(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(TOOLS / "validate_package_admission.py"),
                "--release-context",
                str(self.context_path),
                "--artifact-manifest",
                str(self.manifest()),
                "--evidence",
                str(self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")),
                "--candidate",
                str(self.candidate),
                "--upgrade-baseline",
                str(self.baseline_path),
                "--profile",
                "main",
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_a_blocked_admission_exits_non_zero_and_names_every_reason(self) -> None:
        output = self.work / "package-admission.json"
        completed = self.run_tool("--output", str(output))
        self.assertEqual(completed.returncode, 1, completed.stdout)
        self.assertIn("installed-sdk-matrix-missing", completed.stderr)
        self.assertFalse(json.loads(output.read_text(encoding="utf-8"))["admitted"])

    def test_the_report_can_be_retained_without_failing_the_step(self) -> None:
        output = self.work / "allowed-admission.json"
        completed = self.run_tool("--output", str(output), "--allow-blocking")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(output.is_file())

    def test_an_existing_report_is_never_silently_replaced(self) -> None:
        output = self.work / "existing.json"
        output.write_text("do not replace\n", encoding="utf-8")
        completed = self.run_tool("--output", str(output), "--allow-blocking")
        self.assertEqual(completed.returncode, 1)
        self.assertIn("refusing to replace", completed.stderr)


class EvidenceIntegrityTests(AdmissionFixture):
    def test_evidence_for_another_commit_is_refused(self) -> None:
        evidence = self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["git_sha"] = "d" * 40
        evidence.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageEvidenceError, "different commit"):
            self.report(evidence_paths=[evidence])

    def test_evidence_for_another_package_revision_is_refused(self) -> None:
        evidence = self.evidence(upgrade="NOT_APPLICABLE_INITIAL_BASELINE")
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["package_revision"] = 7
        evidence.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageEvidenceError, "package revision"):
            self.report(evidence_paths=[evidence])


if __name__ == "__main__":
    unittest.main()
