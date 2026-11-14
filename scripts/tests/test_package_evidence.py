from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

from engineering.tools.generate_package_matrix import check_plan, load_matrix
from engineering.tools.generate_release_context import build_context
from engineering.tools.package_framework import PackageFrameworkError, encode_json
from engineering.tools.validate_package_evidence import (
    PackageEvidenceError,
    emit_evidence,
    evidence_filename,
    validate_evidence,
)


ROOT = Path(__file__).resolve().parents[2]
MATRIX = ROOT / "engineering/distribution/package-matrix.yaml"


class PackageEvidenceTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.context = build_context(ROOT)
        cls.matrix = load_matrix(MATRIX)

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-package-evidence-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.context_path = self.directory / "release-context.json"
        self.context_path.write_text(encode_json(self.context), encoding="utf-8")

    def emit(
        self,
        *,
        backend: str = "deb",
        profile: str = "pr",
        stage: str = "full",
        result: str = "OPEN_GATE",
        lifecycle_state: str = "STRUCTURAL",
        statuses: dict[str, str] | None = None,
        evidence_refs: dict[str, str] | None = None,
        subject_path: Path | None = None,
    ) -> Path:
        log = self.directory / "structural-metadata.log"
        log.write_text("structural metadata resolved\n", encoding="utf-8")
        plan_path = self.directory / f"{backend}-check-plan.json"
        plan = check_plan(
            self.matrix,
            backend,
            profile,
            default_status="NOT_RUN",
            statuses=statuses if statuses is not None else {"structural-metadata": "PASS"},
            evidence_refs=(
                evidence_refs
                if evidence_refs is not None
                else {"structural-metadata": "structural-metadata.log"}
            ),
            details={},
        )
        plan_path.write_text(encode_json(plan), encoding="utf-8")
        return emit_evidence(
            backend=backend,
            profile=profile,
            stage=stage,
            result=result,
            lifecycle_state=lifecycle_state,
            context_path=self.context_path,
            check_plan=plan_path,
            output=self.directory / evidence_filename(backend, profile, stage),
            subject_path=subject_path,
            limitations=["Wave A emits structural evidence only."],
            matrix_path=MATRIX,
        )

    def mutate(self, path: Path, **changes: Any) -> Path:
        value = json.loads(path.read_text(encoding="utf-8"))
        value.update(changes)
        path.write_text(encode_json(value), encoding="utf-8")
        return path

    def validate(self, path: Path, **keywords: Any) -> dict[str, Any]:
        keywords.setdefault("context", self.context)
        keywords.setdefault("matrix", self.matrix)
        return validate_evidence(path, **keywords)


class EmissionTests(PackageEvidenceTestCase):
    def test_a_structural_stub_reports_an_open_gate_not_a_pass(self) -> None:
        evidence = json.loads(self.emit().read_text(encoding="utf-8"))
        self.assertEqual(evidence["result"], "OPEN_GATE")
        self.assertEqual(evidence["lifecycle_state"], "STRUCTURAL")
        self.assertEqual(evidence["package_version"], "0.1.0-1")
        self.assertEqual(evidence["git_sha"], self.context["git"]["commit"])
        statuses = {check["id"]: check["status"] for check in evidence["checks"]}
        self.assertEqual(statuses["structural-metadata"], "PASS")
        self.assertEqual(
            {status for check, status in statuses.items() if check != "structural-metadata"},
            {"NOT_RUN"},
        )
        self.assertEqual(evidence["subject"], {"kind": "source_tree", "name": "source-tree", "sha256": None})

    def test_evidence_is_bound_to_an_artifact_when_one_exists(self) -> None:
        artifact = self.directory / "glifistore-0.1.0-example.deb"
        artifact.write_bytes(b"package payload")
        evidence = self.emit(subject_path=artifact)
        self.validate(evidence, artifact_root=self.directory)
        value = json.loads(evidence.read_text(encoding="utf-8"))
        self.assertEqual(value["subject"]["kind"], "artifact")
        self.assertEqual(value["subject"]["name"], artifact.name)

    def test_emission_refuses_replacing_existing_evidence(self) -> None:
        self.emit()
        with self.assertRaisesRegex(PackageEvidenceError, "refusing to replace"):
            self.emit()

    def test_emission_refuses_an_unknown_backend(self) -> None:
        plan = self.directory / "plan.json"
        plan.write_text(
            encode_json(
                check_plan(
                    self.matrix,
                    "deb",
                    "pr",
                    default_status="NOT_RUN",
                    statuses={},
                    evidence_refs={},
                    details={},
                )
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(PackageEvidenceError, "unsupported packaging backend"):
            emit_evidence(
                backend="windows",
                profile="pr",
                stage="full",
                result="OPEN_GATE",
                lifecycle_state="NONE",
                context_path=self.context_path,
                check_plan=plan,
                output=self.directory / "windows-pr-full-package-evidence.json",
                matrix_path=MATRIX,
            )


class NegativeValidationTests(PackageEvidenceTestCase):
    def test_a_tampered_artifact_digest_is_refused(self) -> None:
        artifact = self.directory / "glifistore-0.1.0-example.deb"
        artifact.write_bytes(b"package payload")
        evidence = self.emit(subject_path=artifact)
        artifact.write_bytes(b"different payload")
        with self.assertRaisesRegex(PackageEvidenceError, "subject digest mismatch"):
            self.validate(evidence, artifact_root=self.directory)

    def test_a_missing_artifact_is_refused(self) -> None:
        artifact = self.directory / "glifistore-0.1.0-example.deb"
        artifact.write_bytes(b"package payload")
        evidence = self.emit(subject_path=artifact)
        artifact.unlink()
        with self.assertRaisesRegex(PackageEvidenceError, "subject is missing"):
            self.validate(evidence, artifact_root=self.directory)

    def test_evidence_for_another_version_commit_or_revision_is_refused(self) -> None:
        # One stage per case: evidence is emitted once per (backend, profile, stage).
        for stage, field, value, message in (
            ("build", "product_version", "9.9.9", "different product version"),
            ("inspect", "git_sha", "0" * 40, "different commit"),
            ("install", "package_revision", 7, "different package revision"),
            ("verify", "package_version", "0.1.0-9", "disagrees with the release context"),
        ):
            with self.subTest(field=field):
                evidence = self.emit(stage=stage)
                self.mutate(evidence, **{field: value})
                with self.assertRaisesRegex(PackageEvidenceError, message):
                    self.validate(evidence)

    def test_incomplete_evidence_is_refused(self) -> None:
        evidence = self.emit(backend="freebsd")
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["checks"] = [
            check for check in value["checks"] if check["id"] != "reference-port-structure"
        ]
        evidence.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageEvidenceError, "misses required checks"):
            self.validate(evidence)

    def test_an_undeclared_check_is_refused(self) -> None:
        evidence = self.emit()
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["checks"].append(
            {
                "id": "self-congratulation",
                "status": "PASS",
                "command": "declare victory",
                "evidence_ref": "structural-metadata.log",
                "detail": None,
            }
        )
        evidence.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageEvidenceError, "undeclared checks"):
            self.validate(evidence)

    def test_a_pass_result_cannot_outrun_its_checks(self) -> None:
        evidence = self.emit()
        self.mutate(evidence, result="PASS")
        with self.assertRaisesRegex(PackageEvidenceError, "contradicts unsettled checks"):
            self.validate(evidence)

    def test_a_failing_check_forces_a_failing_result(self) -> None:
        statuses = {"structural-metadata": "PASS", "reference-port-structure": "FAIL"}
        references = {"structural-metadata": "structural-metadata.log"}
        with self.assertRaisesRegex(PackageEvidenceError, "forbids result"):
            self.emit(backend="freebsd", statuses=statuses, evidence_refs=references)

        evidence = self.emit(
            backend="freebsd",
            stage="build",
            result="FAIL",
            lifecycle_state="NONE",
            statuses=statuses,
            evidence_refs=references,
        )
        self.assertEqual(json.loads(evidence.read_text(encoding="utf-8"))["result"], "FAIL")

    def test_a_settled_result_cannot_understate_passing_checks(self) -> None:
        evidence = self.emit()
        self.mutate(evidence, result="NOT_APPLICABLE")
        with self.assertRaisesRegex(PackageEvidenceError, "understates checks"):
            self.validate(evidence)

    def test_a_passing_check_must_retain_a_log(self) -> None:
        with self.assertRaisesRegex(PackageEvidenceError, "must retain a log"):
            self.emit(statuses={"structural-metadata": "PASS"}, evidence_refs={})

    def test_a_referenced_log_must_exist(self) -> None:
        evidence = self.emit()
        (self.directory / "structural-metadata.log").unlink()
        with self.assertRaisesRegex(PackageEvidenceError, "log is missing or empty"):
            self.validate(evidence)

    def test_a_lifecycle_state_needs_a_passing_check(self) -> None:
        with self.assertRaisesRegex(PackageEvidenceError, "not backed by any passing check"):
            self.emit(statuses={}, evidence_refs={})

    def test_a_renamed_evidence_file_is_refused(self) -> None:
        evidence = self.emit()
        renamed = evidence.with_name("deb-release-full-package-evidence.json")
        evidence.rename(renamed)
        with self.assertRaisesRegex(PackageEvidenceError, "must be named"):
            self.validate(renamed)

    def test_a_non_utc_timestamp_is_refused(self) -> None:
        evidence = self.emit()
        self.mutate(evidence, generated_at="2026-09-11T12:00:00+02:00")
        with self.assertRaisesRegex(PackageEvidenceError, "UTC instant"):
            self.validate(evidence)

    def test_an_unknown_result_violates_the_schema(self) -> None:
        evidence = self.emit()
        self.mutate(evidence, result="MOSTLY_PASS")
        with self.assertRaisesRegex(PackageFrameworkError, "package-evidence.schema.json"):
            self.validate(evidence)

    def test_a_profile_that_requires_pass_refuses_an_open_gate(self) -> None:
        evidence = self.emit()
        with self.assertRaisesRegex(PackageEvidenceError, "requires PASS"):
            self.validate(evidence, require_pass=True)

    def test_the_wrong_backend_or_profile_is_refused(self) -> None:
        evidence = self.emit()
        with self.assertRaisesRegex(PackageEvidenceError, "expected rpm"):
            self.validate(evidence, expected_backend="rpm")
        with self.assertRaisesRegex(PackageEvidenceError, "expected main"):
            self.validate(evidence, expected_profile="main")


class EvidenceCommandLineTests(PackageEvidenceTestCase):
    def run_tool(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(ROOT / "engineering/tools/validate_package_evidence.py"),
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_validation_passes_and_then_catches_a_tampered_document(self) -> None:
        evidence = self.emit()
        accepted = self.run_tool(
            "validate", str(evidence), "--release-context", str(self.context_path)
        )
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("package evidence OK", accepted.stdout)

        self.mutate(evidence, product_version="9.9.9")
        refused = self.run_tool(
            "validate", str(evidence), "--release-context", str(self.context_path)
        )
        self.assertEqual(refused.returncode, 1)
        self.assertIn("different product version", refused.stderr)


if __name__ == "__main__":
    unittest.main()
