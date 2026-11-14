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

from engineering.tools.generate_package_matrix import check_plan, load_matrix  # noqa: E402
from engineering.tools.generate_release_context import build_context  # noqa: E402
from engineering.tools.package_framework import digest, encode_json  # noqa: E402
from engineering.tools.run_package_admission import (  # noqa: E402
    PackageAdmissionRunError,
    run_admission,
)
from engineering.tools.validate_package_evidence import emit_evidence  # noqa: E402


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


class PackageAdmissionOrchestratorTests(unittest.TestCase):
    """The CI orchestrator must call baseline/manifest/admission and retain blockers."""

    version = "0.1.0"

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-package-admission-run-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.matrix = load_matrix()

        self.repository = self.work / "repository"
        self.repository.mkdir()
        subprocess.run(
            ["git", "-c", "init.defaultBranch=main", "init", "-q", str(self.repository)],
            check=True,
            capture_output=True,
        )
        (self.repository / "ABI_VERSION").write_text("1.0\n", encoding="utf-8")
        (self.repository / "VERSION").write_text(f"{self.version}\n", encoding="utf-8")
        git(self.repository, "add", "VERSION", "ABI_VERSION")
        git(self.repository, "commit", "-q", "-m", f"chore(release): {self.version}")

        self.context_path = self.work / "release-context.json"
        self.context = build_context(self.repository)
        self.context_path.write_text(encode_json(self.context), encoding="utf-8")

        self.candidate = self.work / "candidate"
        self.candidate.mkdir()
        (self.candidate / f"GlifiStore-{self.version}.tar.xz").write_bytes(b"sealed source bytes")
        seal(self.candidate, "candidate-seal.json", "candidate")

        self.packages = self.work / "packages"
        self.packages.mkdir()
        self.package = self.packages / f"glifistore-{self.version}-freebsd14.3-amd64.pkg"
        self.package.write_bytes(b"packaged bytes")
        (self.packages / "run.log").write_text("retained execution log\n", encoding="utf-8")
        self.output = self.work / "admission"

    def emit_package_ci(self, *, profile: str = "main") -> Path:
        declared = next(
            entry["checks"] for entry in self.matrix["backends"] if entry["id"] == "freebsd"
        )
        statuses = {check: "PASS" for check in declared}
        statuses["package-upgrade"] = "NOT_APPLICABLE_INITIAL_BASELINE"
        plan = self.work / "check-plan.json"
        plan.write_text(
            encode_json(
                check_plan(
                    self.matrix,
                    "freebsd",
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
        output = self.packages / f"freebsd-{profile}-full-package-evidence.json"
        environment = os.environ.copy()
        for name in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_WORKFLOW_REF"):
            environment.pop(name, None)
        with mock.patch.dict(os.environ, environment, clear=True):
            emit_evidence(
                backend="freebsd",
                profile=profile,
                stage="full",
                result="PASS",
                lifecycle_state="UPGRADE_VERIFIED",
                context_path=self.context_path,
                check_plan=plan,
                output=output,
                subject_path=self.package,
            )
        return output

    def write_release_evidence(self) -> Path:
        path = self.packages / "freebsd-package-evidence.json"
        path.write_text(
            encode_json(
                {
                    "schema_version": 1,
                    "evidence_type": "freebsd_package",
                    "result": "passed",
                    "git_sha": self.context["git"]["commit"],
                    "product_version": self.version,
                    "generated_at": "2026-01-01T00:00:00Z",
                    "producer": {
                        "workflow": "owner/repo/.github/workflows/release.yml@refs/tags/v0.1.0",
                        "run_id": "4242",
                        "os": "FreeBSD",
                        "os_version": "14.3",
                        "architecture": "amd64",
                    },
                    "subject": {"name": self.package.name, "sha256": digest(self.package)},
                    "checks": [
                        {
                            "id": "package-build",
                            "command": "native build",
                            "result": "passed",
                            "evidence_ref": "run.log",
                        }
                    ],
                    "limitations": ["synthetic release_evidence fixture"],
                }
            ),
            encoding="utf-8",
        )
        return path

    def test_orchestrator_retains_a_blocked_report_for_package_ci_evidence(self) -> None:
        self.emit_package_ci(profile="main")
        report, code = run_admission(
            root=self.repository,
            profile="main",
            candidate=self.candidate,
            evidence_roots=[self.packages],
            artifact_roots=[self.packages],
            output_dir=self.output,
            context_path=self.context_path,
            seal_sha256=None,
            allow_blocking=True,
            replace=True,
        )
        self.assertEqual(code, 0)
        self.assertFalse(report["admitted"])
        self.assertIn(
            "installed-sdk-matrix-not-package-installed",
            {item["id"] for item in report["blocking"]},
        )
        retained = json.loads((self.output / "package-admission.json").read_text(encoding="utf-8"))
        self.assertEqual(retained["admitted"], report["admitted"])
        self.assertTrue((self.output / "upgrade-baseline.json").is_file())
        self.assertTrue((self.output / "artifact-manifest.json").is_file())
        self.assertTrue((self.output / "installed-sdk-matrix.json").is_file())

    def test_orchestrator_adapts_release_evidence_without_inventing_upgrade_pass(self) -> None:
        self.write_release_evidence()
        report, code = run_admission(
            root=self.repository,
            profile="release",
            candidate=self.candidate,
            evidence_roots=[self.packages],
            artifact_roots=[self.packages],
            output_dir=self.output,
            context_path=self.context_path,
            seal_sha256=None,
            allow_blocking=True,
            replace=True,
        )
        self.assertEqual(code, 0)
        self.assertFalse(report["admitted"])
        evidence = report["evidence"][0]
        self.assertEqual(evidence["name"], "freebsd-package-evidence.json")
        self.assertIsNone(evidence["upgrade_status"])
        blockers = {item["id"] for item in report["blocking"]}
        self.assertIn("upgrade-status-missing", blockers)
        self.assertIn("installed-sdk-matrix-not-package-installed", blockers)

    def test_orchestrator_refuses_an_empty_evidence_set(self) -> None:
        with self.assertRaisesRegex(PackageAdmissionRunError, "no package evidence"):
            run_admission(
                root=self.repository,
                profile="main",
                candidate=self.candidate,
                evidence_roots=[self.work / "empty"],
                artifact_roots=[self.packages],
                output_dir=self.output,
                context_path=self.context_path,
                seal_sha256=None,
                allow_blocking=True,
                replace=True,
            )

    def test_orchestrator_prefers_a_retained_installed_sdk_matrix(self) -> None:
        self.emit_package_ci(profile="main")
        retained = self.packages / "deb" / "installed-sdk-matrix.json"
        retained.parent.mkdir(parents=True, exist_ok=True)
        retained.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "generated_at": "2026-01-01T00:00:00Z",
                    "result": "NOT_RUN",
                    "package_installed": True,
                    "daemon": "/usr/bin/glifistored",
                    "prefix": "/usr",
                    "file_list": "/tmp/package-file-list.txt",
                    "languages": [],
                    "reason": "no sealed SDK distribution archive for: ruby erlang",
                }
            ),
            encoding="utf-8",
        )
        report, code = run_admission(
            root=self.repository,
            profile="main",
            candidate=self.candidate,
            evidence_roots=[self.packages],
            artifact_roots=[self.packages],
            output_dir=self.output,
            context_path=self.context_path,
            seal_sha256=None,
            allow_blocking=True,
            replace=True,
        )
        self.assertEqual(code, 0)
        self.assertFalse(report["admitted"])
        blockers = {item["id"] for item in report["blocking"]}
        self.assertIn("installed-sdk-matrix-not-passing", blockers)
        self.assertNotIn("installed-sdk-matrix-not-package-installed", blockers)
        copied = json.loads((self.output / "installed-sdk-matrix.json").read_text(encoding="utf-8"))
        self.assertTrue(copied["package_installed"])
        self.assertIn("no sealed SDK distribution archive", copied["reason"])


if __name__ == "__main__":
    raise SystemExit(unittest.main())
