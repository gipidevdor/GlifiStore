from __future__ import annotations

import copy
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from engineering.tools.bsd_package_lifecycle import (
    NATIVE_STEPS,
    BsdLifecycleError,
    decide,
    emit_arguments,
    lifecycle_state,
    preflight,
)
from engineering.tools.generate_package_matrix import (
    PackageMatrixError,
    check_vocabulary,
    load_matrix,
    validate_matrix,
)
from engineering.tools.generate_release_context import build_context
from engineering.tools.package_framework import encode_json
from engineering.tools.validate_package_evidence import (
    PackageEvidenceError,
    emit_evidence,
    evidence_filename,
    validate_evidence,
)


ROOT = Path(__file__).resolve().parents[2]
MATRIX = ROOT / "engineering/distribution/package-matrix.yaml"
PACKAGE_CI = ROOT / "scripts/package-ci.sh"
MODULE = ROOT / "scripts/lib/package-backend-bsd.sh"
MARKERS = (
    ROOT / "packaging/freebsd/PORTS_ACCOUNT_REGISTERED",
    ROOT / "packaging/openbsd/PORTS_ACCOUNT_REGISTERED",
)
PREREQUISITES = dict(
    stage="full",
    host="FreeBSD",
    ports_account="present",
    sealed_source="verified",
    ports_tree="present",
    ports_root="/usr/ports",
    privileged="yes",
    lifecycle_script="present",
)


class BsdPackageCiTestCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = load_matrix(MATRIX)
        cls.context = build_context(ROOT)

    def directory(self) -> Path:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-bsd-package-ci-")
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def run_package_ci(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(PACKAGE_CI), *arguments],
            check=False,
            capture_output=True,
            text=True,
            cwd=ROOT,
        )

    def evidence(self, output: Path, backend: str, profile: str) -> dict:
        path = output / backend / "full" / evidence_filename(backend, profile, "full")
        self.assertTrue(path.is_file(), f"missing evidence: {path}")
        return json.loads(path.read_text(encoding="utf-8"))

    def statuses(self, evidence: dict) -> dict[str, str]:
        return {check["id"]: check["status"] for check in evidence["checks"]}

    def categories(self, evidence: dict) -> dict[str, str]:
        return {check["id"]: check["category"] for check in evidence["checks"]}


class EntryPointTests(BsdPackageCiTestCase):
    def test_the_bsd_backends_run_through_package_ci_on_any_host(self) -> None:
        output = self.directory() / "run"
        completed = self.run_package_ci(
            "--profile", "pr", "--backend", "freebsd", "--backend", "openbsd",
            "--output-dir", str(output),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        for backend in ("freebsd", "openbsd"):
            with self.subTest(backend=backend):
                self.assertIn(f"PACKAGE-CI {backend} pr full OPEN_GATE", completed.stdout)
                evidence = self.evidence(output, backend, "pr")
                statuses = self.statuses(evidence)
                self.assertEqual(evidence["result"], "OPEN_GATE")
                self.assertEqual(evidence["lifecycle_state"], "STRUCTURAL")
                self.assertEqual(statuses["structural-metadata"], "PASS")
                self.assertEqual(statuses["reference-port-structure"], "PASS")
                self.assertEqual(statuses["package-build"], "BLOCKED")
                self.assertEqual(statuses["package-install"], "BLOCKED")
                self.assertEqual(statuses["service-lifecycle"], "BLOCKED")
                self.assertEqual(
                    (output / backend / "full" / "native-prerequisites.log").is_file(), True
                )

    def test_every_lifecycle_dimension_is_a_separate_evidence_row(self) -> None:
        output = self.directory() / "run"
        completed = self.run_package_ci(
            "--profile", "pr", "--backend", "freebsd", "--output-dir", str(output)
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        evidence = self.evidence(output, "freebsd", "pr")
        categories = self.categories(evidence)
        self.assertEqual(
            set(categories.values()),
            {"structural", "native-build", "package", "service", "upstream-accepted"},
        )
        self.assertEqual(categories["reference-port-structure"], "structural")
        self.assertEqual(categories["package-build"], "native-build")
        self.assertEqual(categories["package-install"], "package")
        self.assertEqual(categories["put-get-erase"], "service")
        self.assertEqual(categories["upstream-ports-acceptance"], "upstream-accepted")
        # A structural pass must not leak into any other category.
        passing = {
            check["category"] for check in evidence["checks"] if check["status"] == "PASS"
        }
        self.assertEqual(passing, {"structural"})

    def test_the_upstream_rows_stay_open_and_no_marker_is_invented(self) -> None:
        output = self.directory() / "run"
        completed = self.run_package_ci(
            "--profile", "nightly", "--backend", "openbsd", "--output-dir", str(output)
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        statuses = self.statuses(self.evidence(output, "openbsd", "nightly"))
        self.assertEqual(statuses["ports-account-registration"], "OPEN_GATE")
        self.assertEqual(statuses["upstream-ports-acceptance"], "OPEN_GATE")
        for marker in MARKERS:
            self.assertFalse(marker.exists(), f"{marker} must never be created by CI")
        # The module only ever probes the marker; nothing writes to it.
        body = MODULE.read_text(encoding="utf-8")
        self.assertIn('[[ -f "$marker" ]]', body)
        self.assertIn("never creates or updates this marker", body)
        self.assertNotIn("touch", body)
        self.assertNotIn('>"$marker"', body)
        self.assertNotIn('> "$marker"', body)

    def test_openbsd_keeps_the_libressl_constraint(self) -> None:
        output = self.directory() / "run"
        completed = self.run_package_ci(
            "--profile", "pr", "--backend", "openbsd", "--output-dir", str(output)
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        evidence = self.evidence(output, "openbsd", "pr")
        self.assertTrue(any("LibreSSL" in item for item in evidence["limitations"]))
        for entry in self.matrix["backends"]:
            if entry["id"] == "openbsd":
                self.assertTrue(any("LibreSSL" in item for item in entry["limitations"]))

    def test_a_candidate_that_does_not_admit_fails_closed(self) -> None:
        output = self.directory() / "run"
        candidate = self.directory() / "candidate"
        candidate.mkdir()
        (candidate / "candidate-seal.json").write_text("{}\n", encoding="utf-8")
        completed = self.run_package_ci(
            "--profile", "pr", "--backend", "freebsd", "--candidate", str(candidate),
            "--output-dir", str(output),
        )
        self.assertEqual(completed.returncode, 1, completed.stdout)
        evidence = self.evidence(output, "freebsd", "pr")
        self.assertEqual(evidence["result"], "FAIL")
        self.assertEqual(self.statuses(evidence)["sealed-source-admission"], "FAIL")

    def test_a_missing_candidate_directory_is_refused(self) -> None:
        completed = self.run_package_ci(
            "--profile", "pr", "--backend", "freebsd", "--candidate", "/nonexistent-candidate"
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("candidate directory is missing", completed.stderr)

    def test_package_ci_delegates_the_bsd_backends_to_the_module(self) -> None:
        entry_point = PACKAGE_CI.read_text(encoding="utf-8")
        self.assertIn("scripts/lib/package-backend-bsd.sh", entry_point)
        self.assertIn("bsd_backend_run", entry_point)
        module = MODULE.read_text(encoding="utf-8")
        for script in (
            "scripts/test-freebsd-package-lifecycle.sh",
            "scripts/test-openbsd-package-lifecycle.sh",
        ):
            self.assertIn(script.removeprefix("scripts/"), module)
        # The release workflow keeps calling the native producers directly.
        release = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn("bash scripts/test-freebsd-package-lifecycle.sh", release)
        self.assertIn("bash scripts/test-openbsd-package-lifecycle.sh", release)


class PreflightTests(BsdPackageCiTestCase):
    def test_every_prerequisite_must_hold_before_the_native_lifecycle_runs(self) -> None:
        decision, reason = preflight(backend="freebsd", **PREREQUISITES)
        self.assertEqual((decision, reason), ("RUN", ""))

        for override, message in (
            ({"host": "Linux"}, "native FreeBSD host is required"),
            ({"ports_account": "absent"}, "PORTS_ACCOUNT_REGISTERED is absent"),
            ({"sealed_source": "absent"}, "no sealed candidate source archive"),
            ({"sealed_source": "unverified"}, "failed admission"),
            ({"ports_tree": "absent"}, "native ports tree is required"),
            ({"privileged": "no"}, "requires root"),
            ({"lifecycle_script": "absent"}, "test-freebsd-package-lifecycle.sh"),
            ({"stage": "build"}, "only runs in the full stage"),
        ):
            with self.subTest(override=override):
                decision, reason = preflight(
                    backend="freebsd", **{**PREREQUISITES, **override}
                )
                self.assertEqual(decision, "BLOCKED")
                self.assertIn(message, reason)

    def test_the_openbsd_preflight_expects_a_native_openbsd_host(self) -> None:
        decision, reason = preflight(backend="openbsd", **PREREQUISITES)
        self.assertEqual(decision, "BLOCKED")
        self.assertIn("native OpenBSD host is required and this runner is FreeBSD", reason)


class NativeMappingTests(BsdPackageCiTestCase):
    def populate(self, directory: Path, backend: str, *, steps: tuple[str, ...] | None = None) -> None:
        for name in (
            "structural-metadata.log",
            "reference-port-structure.log",
            "ports-account-registration.log",
            "upstream-ports-acceptance.log",
            "sealed-source-admission.log",
            "native-prerequisites.log",
            f"{backend}-native-lifecycle.log",
        ):
            (directory / name).write_text(f"{name} retained\n", encoding="utf-8")
        executed = steps
        if executed is None:
            executed = tuple(step for _, group in NATIVE_STEPS for step in group)
        prefix = "FREEBSD-PACKAGE" if backend == "freebsd" else "OPENBSD-PACKAGE"
        for step in executed:
            (directory / f"{backend}-{step}.log").write_text(
                f"{prefix} {step} PASSED\n", encoding="utf-8"
            )

    def decide(self, directory: Path, backend: str = "freebsd", **overrides) -> dict:
        arguments = dict(
            backend=backend,
            profile="release",
            stage="full",
            directory=directory,
            context=self.context,
            matrix=self.matrix,
            port_structure="PASS",
            ports_account="present",
            sealed_source="verified",
            native_lifecycle="passed",
            native_reason="",
        )
        arguments.update(overrides)
        return decide(**arguments)

    def test_a_complete_native_run_maps_every_retained_log(self) -> None:
        directory = self.directory()
        self.populate(directory, "freebsd")
        package = directory / "glifistore-0.1.0-freebsd14.3-amd64.pkg"
        package.write_bytes(b"native package payload")

        decision = self.decide(directory)
        statuses = decision["statuses"]
        for check, _ in NATIVE_STEPS:
            self.assertEqual(statuses[check], "PASS", check)
        # Upstream acceptance stays OPEN_GATE, so a complete native lifecycle is
        # LIFECYCLE_VERIFIED (external-consumer included) but never a PASS result.
        self.assertEqual(statuses["external-consumer"], "PASS")
        self.assertEqual(decision["lifecycle_state"], "LIFECYCLE_VERIFIED")
        self.assertEqual(decision["result"], "OPEN_GATE")
        self.assertEqual(decision["subject"], str(package))

    def test_a_complete_native_run_emits_valid_evidence_bound_to_the_package(self) -> None:
        directory = self.directory()
        self.populate(directory, "openbsd")
        package = directory / "glifistore-0.1.0-openbsd7.9-amd64.tgz"
        package.write_bytes(b"native package payload")
        decision = self.decide(directory, "openbsd")

        context_path = directory / "release-context.json"
        context_path.write_text(encode_json(self.context), encoding="utf-8")
        plan = directory / "check-plan.json"
        plan.write_text(encode_json(decision["plan"]), encoding="utf-8")
        arguments = emit_arguments(decision)
        self.assertIn("--subject", arguments)

        evidence = emit_evidence(
            backend="openbsd",
            profile="release",
            stage="full",
            result=decision["result"],
            lifecycle_state=decision["lifecycle_state"],
            context_path=context_path,
            check_plan=plan,
            output=directory / evidence_filename("openbsd", "release", "full"),
            subject_path=package,
            limitations=decision["limitations"],
            residuals=decision["residuals"],
            matrix_path=MATRIX,
        )
        value = validate_evidence(
            evidence,
            context=self.context,
            matrix=self.matrix,
            expected_backend="openbsd",
            expected_profile="release",
            artifact_root=directory,
        )
        self.assertEqual(value["subject"]["name"], package.name)
        self.assertEqual(value["result"], "OPEN_GATE")

    def test_a_missing_native_log_is_never_inferred_as_a_pass(self) -> None:
        directory = self.directory()
        self.populate(directory, "freebsd", steps=("package-build", "package-install"))
        decision = self.decide(directory, native_lifecycle="passed")
        statuses = decision["statuses"]
        self.assertEqual(statuses["package-build"], "PASS")
        self.assertEqual(statuses["package-install"], "PASS")
        self.assertEqual(statuses["package-inspect"], "FAIL")
        self.assertEqual(decision["result"], "FAIL")
        self.assertEqual(decision["lifecycle_state"], "BUILT")
        detail = next(
            check["detail"] for check in decision["plan"] if check["id"] == "package-inspect"
        )
        self.assertIn("never inferred", detail)

    def test_a_failed_native_run_fails_the_first_unproven_check(self) -> None:
        directory = self.directory()
        self.populate(
            directory,
            "freebsd",
            steps=("package-build", "package-install", "file-inventory", "service-start"),
        )
        decision = self.decide(directory, native_lifecycle="failed")
        statuses = decision["statuses"]
        self.assertEqual(statuses["package-inspect"], "PASS")
        self.assertEqual(statuses["external-consumer"], "FAIL")
        self.assertEqual(statuses["service-lifecycle"], "NOT_RUN")
        self.assertEqual(statuses["restart-recovery"], "NOT_RUN")
        self.assertEqual(statuses["package-remove"], "NOT_RUN")
        self.assertEqual(decision["result"], "FAIL")
        self.assertEqual(decision["lifecycle_state"], "INSTALLED")

    def test_a_failing_native_exit_code_cannot_be_hidden_by_complete_logs(self) -> None:
        directory = self.directory()
        self.populate(directory, "freebsd")
        decision = self.decide(directory, native_lifecycle="failed")
        self.assertEqual(decision["result"], "FAIL")
        self.assertTrue(
            any("exited non-zero" in item for item in decision["limitations"]),
            decision["limitations"],
        )

    def test_two_native_packages_are_refused_as_a_subject(self) -> None:
        directory = self.directory()
        self.populate(directory, "freebsd")
        for name in (
            "glifistore-0.1.0-freebsd14.3-amd64.pkg",
            "glifistore-0.1.0-freebsd14.2-amd64.pkg",
        ):
            (directory / name).write_bytes(b"payload")
        with self.assertRaisesRegex(BsdLifecycleError, "at most one native"):
            self.decide(directory)


class UpgradeHonestyTests(BsdPackageCiTestCase):
    def test_without_a_prior_release_the_upgrade_row_is_an_initial_baseline(self) -> None:
        directory = self.directory()
        (directory / "structural-metadata.log").write_text("retained\n", encoding="utf-8")
        decision = decide(
            backend="freebsd",
            profile="nightly",
            stage="full",
            directory=directory,
            context=self.context,
            matrix=self.matrix,
            port_structure="PASS",
            ports_account="absent",
            sealed_source="absent",
            native_lifecycle="skipped",
            native_reason="this runner is not FreeBSD",
        )
        self.assertFalse(self.context["previous"]["available"])
        self.assertEqual(
            decision["statuses"]["package-upgrade"], "NOT_APPLICABLE_INITIAL_BASELINE"
        )
        self.assertTrue(
            any(residual.startswith("package-upgrade-n1=") for residual in decision["residuals"])
        )

    def test_with_a_prior_release_the_upgrade_row_stays_not_run_until_sealed_bytes_exist(
        self,
    ) -> None:
        directory = self.directory()
        (directory / "structural-metadata.log").write_text("retained\n", encoding="utf-8")
        context = copy.deepcopy(self.context)
        context["previous"] = {
            "available": True,
            "version": "0.0.9",
            "tag": "v0.0.9",
            "git_sha": "0" * 40,
            "abi_major": 1,
            "reason": "test fixture",
        }
        context["release_kind"] = "patch"
        decision = decide(
            backend="openbsd",
            profile="nightly",
            stage="full",
            directory=directory,
            context=context,
            matrix=self.matrix,
            port_structure="PASS",
            ports_account="absent",
            sealed_source="absent",
            native_lifecycle="skipped",
            native_reason="this runner is not OpenBSD",
        )
        self.assertEqual(decision["statuses"]["package-upgrade"], "NOT_RUN")
        detail = next(
            check["detail"] for check in decision["plan"] if check["id"] == "package-upgrade"
        )
        self.assertIn("v0.0.9", detail)
        self.assertIn("GLIFISTORE_N1_PACKAGE_DIR", detail)
        self.assertIn("never rebuilds N-1 from HEAD", detail)

    def test_a_retained_native_upgrade_pass_maps_to_package_upgrade(self) -> None:
        directory = self.directory()
        (directory / "structural-metadata.log").write_text("retained\n", encoding="utf-8")
        (directory / "reference-port-structure.log").write_text("retained\n", encoding="utf-8")
        (directory / "ports-account-registration.log").write_text("retained\n", encoding="utf-8")
        (directory / "upstream-ports-acceptance.log").write_text("retained\n", encoding="utf-8")
        (directory / "sealed-source-admission.log").write_text("retained\n", encoding="utf-8")
        (directory / "freebsd-package-upgrade.log").write_text(
            "FREEBSD-PACKAGE package-upgrade PASSED\n", encoding="utf-8"
        )
        context = copy.deepcopy(self.context)
        context["previous"] = {
            "available": True,
            "version": "0.0.9",
            "tag": "v0.0.9",
            "git_sha": "0" * 40,
            "abi_major": 1,
            "reason": "test fixture",
        }
        n1 = self.directory()
        (n1 / "glifistore-0.0.9-freebsd14.3-amd64.pkg").write_bytes(b"n1")
        with mock.patch.dict(os.environ, {"GLIFISTORE_N1_PACKAGE_DIR": str(n1)}):
            decision = decide(
                backend="freebsd",
                profile="nightly",
                stage="full",
                directory=directory,
                context=context,
                matrix=self.matrix,
                port_structure="PASS",
                ports_account="absent",
                sealed_source="absent",
                native_lifecycle="skipped",
                native_reason="fixture retains the upgrade log only",
            )
        self.assertEqual(decision["statuses"]["package-upgrade"], "PASS")

    def test_the_upgrade_row_only_reaches_upgrade_verified_through_a_real_pass(self) -> None:
        proven = {
            "structural-metadata": "PASS",
            "reference-port-structure": "PASS",
            "sealed-source-admission": "PASS",
            "package-build": "PASS",
            "package-install": "PASS",
            "package-inspect": "PASS",
            "service-lifecycle": "PASS",
            "put-get-erase": "PASS",
            "restart-recovery": "PASS",
            "config-preservation": "PASS",
            "package-remove": "PASS",
            "external-consumer": "PASS",
        }
        for status in ("NOT_RUN", "NOT_APPLICABLE_INITIAL_BASELINE", "BLOCKED"):
            with self.subTest(status=status):
                self.assertEqual(
                    lifecycle_state({**proven, "package-upgrade": status}), "LIFECYCLE_VERIFIED"
                )
        self.assertEqual(
            lifecycle_state({**proven, "package-upgrade": "PASS"}), "UPGRADE_VERIFIED"
        )


class CategoryAuthorityTests(BsdPackageCiTestCase):
    def test_every_check_a_bsd_backend_declares_carries_a_category(self) -> None:
        vocabulary = check_vocabulary(self.matrix)
        for entry in self.matrix["backends"]:
            if entry["id"] not in {"freebsd", "openbsd"}:
                continue
            for check in entry["checks"]:
                with self.subTest(backend=entry["id"], check=check):
                    self.assertIsNotNone(vocabulary[check]["category"])

    def test_a_real_backend_cannot_declare_an_uncategorised_check(self) -> None:
        matrix = copy.deepcopy(load_matrix(MATRIX))
        for check in matrix["lifecycle_checks"]:
            if check["id"] == "reference-port-structure":
                del check["category"]
        with self.assertRaisesRegex(PackageMatrixError, "must categorise every check"):
            validate_matrix(matrix)

    def test_an_unknown_category_is_refused(self) -> None:
        matrix = copy.deepcopy(load_matrix(MATRIX))
        matrix["lifecycle_checks"][0]["category"] = "self-certified"
        with self.assertRaisesRegex(PackageMatrixError, "unsupported category"):
            validate_matrix(matrix)

    def test_a_relabelled_category_in_evidence_is_refused(self) -> None:
        directory = self.directory()
        (directory / "structural-metadata.log").write_text("retained\n", encoding="utf-8")
        (directory / "reference-port-structure.log").write_text("retained\n", encoding="utf-8")
        decision = decide(
            backend="freebsd",
            profile="pr",
            stage="full",
            directory=directory,
            context=self.context,
            matrix=self.matrix,
            port_structure="PASS",
            ports_account="absent",
            sealed_source="absent",
            native_lifecycle="skipped",
            native_reason="this runner is not FreeBSD",
        )
        context_path = directory / "release-context.json"
        context_path.write_text(encode_json(self.context), encoding="utf-8")
        plan = directory / "check-plan.json"
        plan.write_text(encode_json(decision["plan"]), encoding="utf-8")
        evidence = emit_evidence(
            backend="freebsd",
            profile="pr",
            stage="full",
            result=decision["result"],
            lifecycle_state=decision["lifecycle_state"],
            context_path=context_path,
            check_plan=plan,
            output=directory / evidence_filename("freebsd", "pr", "full"),
            limitations=decision["limitations"],
            residuals=decision["residuals"],
            matrix_path=MATRIX,
        )
        value = json.loads(evidence.read_text(encoding="utf-8"))
        for check in value["checks"]:
            if check["id"] == "reference-port-structure":
                check["category"] = "service"
        evidence.write_text(encode_json(value), encoding="utf-8")
        with self.assertRaisesRegex(PackageEvidenceError, "reports category"):
            validate_evidence(evidence, context=self.context, matrix=self.matrix)

    def test_the_module_must_classify_every_check_the_matrix_declares(self) -> None:
        matrix = copy.deepcopy(self.matrix)
        for entry in matrix["backends"]:
            if entry["id"] == "freebsd":
                entry["checks"].append("package-metadata-render")
        directory = self.directory()
        (directory / "structural-metadata.log").write_text("retained\n", encoding="utf-8")
        with self.assertRaisesRegex(BsdLifecycleError, "does not classify declared checks"):
            decide(
                backend="freebsd",
                profile="pr",
                stage="full",
                directory=directory,
                context=self.context,
                matrix=matrix,
                port_structure="PASS",
                ports_account="absent",
                sealed_source="absent",
                native_lifecycle="skipped",
                native_reason="this runner is not FreeBSD",
            )


if __name__ == "__main__":
    unittest.main()
