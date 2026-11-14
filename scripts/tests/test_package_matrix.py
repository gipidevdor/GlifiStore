from __future__ import annotations

import copy
import json
import re
import subprocess
import sys
import unittest
from pathlib import Path

from engineering.tools.generate_package_matrix import (
    RELEASE_POLICY_ARTIFACTS,
    PackageMatrixError,
    backend,
    check_plan,
    expand,
    load_matrix,
    profile_backends,
    required_checks,
    validate_matrix,
)
from engineering.tools.package_framework import LIFECYCLE_STATES


ROOT = Path(__file__).resolve().parents[2]
MATRIX = ROOT / "engineering/distribution/package-matrix.yaml"


class RepositoryMatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = load_matrix(MATRIX)

    def test_the_repository_matrix_is_valid(self) -> None:
        self.assertEqual(
            sorted(entry["id"] for entry in self.matrix["backends"]),
            ["deb", "freebsd", "homebrew", "macports", "openbsd", "rpm"],
        )
        self.assertEqual(list(self.matrix["lifecycle_states"]), list(LIFECYCLE_STATES))

    def test_windows_and_apple_pkg_stay_out_of_scope(self) -> None:
        out_of_scope = {entry["id"] for entry in self.matrix["out_of_scope"]}
        self.assertIn("windows", out_of_scope)
        self.assertIn("apple-pkg", out_of_scope)
        serialised = json.dumps(self.matrix)
        self.assertNotIn("windows-", serialised)
        for entry in self.matrix["backends"]:
            for target in entry["targets"]:
                self.assertNotIn("windows", target["platform"].lower())

    def test_only_backends_already_required_by_the_release_policy_are_required(self) -> None:
        required = sorted(
            entry["id"] for entry in self.matrix["backends"] if entry["required_for_release"]
        )
        self.assertEqual(required, ["freebsd", "openbsd"])
        for entry in self.matrix["backends"]:
            if entry["id"] in {"deb", "rpm", "macports", "homebrew"}:
                self.assertFalse(entry["required_for_release"])
            # Shipping packaging sources never promotes a backend past STRUCTURAL:
            # IMPLEMENTED means the lifecycle runs in CI and retains evidence.
            if entry["id"] in {"deb", "rpm", "macports", "homebrew"}:
                self.assertEqual(entry["status"], "STRUCTURAL")
                self.assertEqual(entry["lifecycle_state"], "STRUCTURAL")

    def test_release_policy_artifacts_mirror_the_release_bundle_authority(self) -> None:
        source = (ROOT / "engineering/tools/release_bundle.py").read_text(encoding="utf-8")
        block = source.split("target_patterns = {", 1)[1].split("\n    }", 1)[0]
        authority = sorted(set(re.findall(r'"([a-z_]+)":\s*re\.compile', block)))
        self.assertEqual(authority, sorted(RELEASE_POLICY_ARTIFACTS))
        self.assertEqual(
            sorted(entry["id"] for entry in self.matrix["release_policy_artifacts"]), authority
        )

    def test_profiles_expand_to_the_declared_targets(self) -> None:
        self.assertEqual(profile_backends(self.matrix, "pr"), ["deb", "freebsd", "macports", "openbsd"])
        self.assertEqual(
            profile_backends(self.matrix, "release"),
            ["deb", "freebsd", "homebrew", "macports", "openbsd", "rpm"],
        )
        rows = expand(self.matrix, "pr", ["deb"])
        self.assertEqual([row["id"] for row in rows], ["deb-debian-12-amd64"])
        self.assertEqual(rows[0]["profile"], "pr")
        self.assertEqual(
            rows[0]["required_checks"], ["structural-metadata", "package-metadata-render"]
        )
        rpm_rows = expand(self.matrix, "main", ["rpm"])
        self.assertEqual([row["id"] for row in rpm_rows], ["rpm-fedora-43-amd64"])
        self.assertEqual(rpm_rows[0]["platform"], "fedora-43")
        self.assertTrue(
            str(rpm_rows[0]["container_digest"]).startswith("sha256:37d4208f1aa0f4c883")
        )

    def test_expansion_refuses_a_backend_that_does_not_run_in_the_profile(self) -> None:
        with self.assertRaisesRegex(PackageMatrixError, "no target runs backends"):
            expand(self.matrix, "pr", ["rpm"])
        with self.assertRaisesRegex(PackageMatrixError, "unknown packaging backend"):
            expand(self.matrix, "pr", ["windows"])

    def test_bsd_backends_owe_their_reference_port_structure_in_every_profile(self) -> None:
        for identifier in ("freebsd", "openbsd"):
            for profile in ("pr", "main", "nightly", "release"):
                with self.subTest(backend=identifier, profile=profile):
                    self.assertIn(
                        "reference-port-structure",
                        required_checks(self.matrix, identifier, profile),
                    )

    def test_the_check_plan_covers_every_declared_check(self) -> None:
        plan = check_plan(
            self.matrix,
            "deb",
            "pr",
            default_status="NOT_RUN",
            statuses={"structural-metadata": "PASS"},
            evidence_refs={"structural-metadata": "structural-metadata.log"},
            details={},
        )
        self.assertEqual([entry["id"] for entry in plan], backend(self.matrix, "deb")["checks"])
        self.assertEqual(plan[0]["id"], "structural-metadata")
        self.assertEqual(plan[0]["status"], "PASS")
        self.assertEqual(plan[0]["evidence_ref"], "structural-metadata.log")
        self.assertTrue(all(entry["command"] for entry in plan))
        self.assertEqual({entry["status"] for entry in plan[1:]}, {"NOT_RUN"})

    def test_the_check_plan_refuses_unknown_checks_and_statuses(self) -> None:
        with self.assertRaisesRegex(PackageMatrixError, "does not declare"):
            check_plan(
                self.matrix,
                "deb",
                "pr",
                default_status="NOT_RUN",
                statuses={"reference-port-structure": "PASS"},
                evidence_refs={},
                details={},
            )
        with self.assertRaisesRegex(PackageMatrixError, "unsupported check status"):
            check_plan(
                self.matrix,
                "deb",
                "pr",
                default_status="SKIPPED",
                statuses={},
                evidence_refs={},
                details={},
            )


class MatrixValidationTests(unittest.TestCase):
    def matrix(self) -> dict:
        return copy.deepcopy(load_matrix(MATRIX))

    def test_a_windows_target_is_refused(self) -> None:
        matrix = self.matrix()
        matrix["backends"][0]["targets"][0]["platform"] = "windows-2022"
        with self.assertRaisesRegex(PackageMatrixError, "Windows is out of scope"):
            validate_matrix(matrix)

    def test_dropping_an_out_of_scope_decision_is_refused(self) -> None:
        matrix = self.matrix()
        matrix["out_of_scope"] = [
            entry for entry in matrix["out_of_scope"] if entry["id"] != "apple-pkg"
        ]
        with self.assertRaisesRegex(PackageMatrixError, "explicitly out of scope"):
            validate_matrix(matrix)

    def test_a_new_backend_cannot_claim_release_requirement(self) -> None:
        matrix = self.matrix()
        for entry in matrix["backends"]:
            if entry["id"] == "deb":
                entry["required_for_release"] = True
        with self.assertRaisesRegex(PackageMatrixError, "without a release policy artifact"):
            validate_matrix(matrix)

    def test_a_planned_backend_cannot_claim_a_lifecycle_state(self) -> None:
        matrix = self.matrix()
        for entry in matrix["backends"]:
            if entry["id"] == "deb":
                entry["status"] = "PLANNED"
                entry["lifecycle_state"] = "LIFECYCLE_VERIFIED"
        with self.assertRaisesRegex(PackageMatrixError, "is PLANNED and cannot claim"):
            validate_matrix(matrix)

    def test_an_undeclared_check_is_refused(self) -> None:
        matrix = self.matrix()
        for entry in matrix["backends"]:
            if entry["id"] == "deb":
                entry["checks"].append("package-blesses-itself")
        with self.assertRaisesRegex(PackageMatrixError, "outside the vocabulary"):
            validate_matrix(matrix)

    def test_an_implemented_backend_must_pin_its_container_digest(self) -> None:
        matrix = self.matrix()
        for entry in matrix["backends"]:
            if entry["id"] == "deb":
                entry["status"] = "IMPLEMENTED"
                entry["lifecycle_state"] = "BUILT"
                for target in entry["targets"]:
                    target["container_digest"] = None
        with self.assertRaisesRegex(PackageMatrixError, "pin its container digest"):
            validate_matrix(matrix)

    def test_an_unimplemented_backend_must_state_its_limitations(self) -> None:
        matrix = self.matrix()
        for entry in matrix["backends"]:
            if entry["id"] == "deb":
                entry["limitations"] = []
        with self.assertRaisesRegex(PackageMatrixError, "must state its limitations"):
            validate_matrix(matrix)


class MatrixCommandLineTests(unittest.TestCase):
    def run_tool(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(ROOT / "engineering/tools/generate_package_matrix.py"), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_validation_and_github_expansion(self) -> None:
        validated = self.run_tool("validate")
        self.assertEqual(validated.returncode, 0, validated.stderr)
        self.assertIn("package matrix OK", validated.stdout)

        expanded = self.run_tool("expand", "--profile", "pr", "--format", "github")
        self.assertEqual(expanded.returncode, 0, expanded.stderr)
        include = json.loads(expanded.stdout)["include"]
        self.assertTrue(include)
        self.assertEqual({row["profile"] for row in include}, {"pr"})
        self.assertTrue(all(row["runner"] for row in include))

    def test_an_unknown_profile_is_refused(self) -> None:
        refused = self.run_tool("expand", "--profile", "hotfix")
        self.assertNotEqual(refused.returncode, 0)


if __name__ == "__main__":
    unittest.main()
