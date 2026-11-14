from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "engineering/tools/package_ci_plan.py"

sys.path.insert(0, str(ROOT))

from engineering.tools.package_ci_plan import (  # noqa: E402
    PackageCiPlanError,
    RETENTION_DAYS,
    build_plan,
    resolve_profile,
    scope,
)


def run(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(TOOL), *arguments],
        check=False,
        capture_output=True,
        text=True,
        cwd=ROOT,
    )


class ProfileResolutionTests(unittest.TestCase):
    def test_every_event_maps_to_exactly_one_profile(self) -> None:
        self.assertEqual(resolve_profile("pull_request", ref="refs/pull/7/merge", profile_input=""), "pr")
        self.assertEqual(resolve_profile("push", ref="refs/heads/main", profile_input=""), "main")
        self.assertEqual(resolve_profile("schedule", ref="refs/heads/main", profile_input=""), "nightly")
        self.assertEqual(
            resolve_profile("workflow_dispatch", ref="refs/heads/topic", profile_input="nightly"),
            "nightly",
        )
        self.assertEqual(
            resolve_profile("workflow_call", ref="refs/tags/v9.9.9", profile_input="release"),
            "release",
        )

    def test_unowned_events_and_refs_are_refused(self) -> None:
        # A tag push only reaches packaging CI through release.yml, which passes the
        # sealed candidate with it; nothing else may claim the release profile.
        with self.assertRaises(PackageCiPlanError):
            resolve_profile("push", ref="refs/tags/v0.1.0", profile_input="")
        with self.assertRaises(PackageCiPlanError):
            resolve_profile("workflow_dispatch", ref="refs/heads/main", profile_input="release")
        with self.assertRaises(PackageCiPlanError):
            resolve_profile("workflow_dispatch", ref="refs/heads/main", profile_input="")
        with self.assertRaises(PackageCiPlanError):
            resolve_profile("release", ref="refs/tags/v0.1.0", profile_input="")
        with self.assertRaises(PackageCiPlanError):
            resolve_profile("workflow_call", ref="refs/heads/main", profile_input="hotfix")


class ChangeDetectionTests(unittest.TestCase):
    def test_packaging_relevant_changes_always_run(self) -> None:
        for path in (
            "CMakeLists.txt",
            "VERSION",
            "ABI_VERSION",
            "cmake/ToolchainOptimizations.cmake",
            "include/glifistore/core/little_endian.hpp",
            "src/daemon/main.cpp",
            "packaging/debian/templates/control.in",
            "engineering/distribution/package-matrix.yaml",
            "engineering/tools/package_ci_plan.py",
            "scripts/package-ci.sh",
            ".github/workflows/release.yml",
        ):
            with self.subTest(path=path):
                run_it, reason = scope("pr", [path, "docs/readme.md"])
                self.assertTrue(run_it)
                self.assertIn(path, reason)

    def test_only_documentation_may_skip_and_anything_unknown_runs(self) -> None:
        skipped, reason = scope("pr", ["docs/distribution/bsd-packaging.md", "README.md"])
        self.assertFalse(skipped)
        self.assertIn("documentation-only", reason)

        unknown, reason = scope("pr", ["sdk/python/glifistore/client.py"])
        self.assertTrue(unknown)
        self.assertIn("outside the documented documentation-only set", reason)

    def test_change_detection_fails_closed(self) -> None:
        # No diff, an empty diff and any deeper profile all run the full matrix.
        for paths in (None, []):
            with self.subTest(paths=paths):
                self.assertTrue(scope("pr", paths)[0])
        for profile in ("main", "nightly", "release"):
            with self.subTest(profile=profile):
                self.assertTrue(scope(profile, ["docs/readme.md"])[0])


class PlanTests(unittest.TestCase):
    def test_the_matrix_is_generated_and_never_hard_coded(self) -> None:
        plan = build_plan(
            event="pull_request",
            ref="refs/pull/1/merge",
            profile_input="",
            changed_from=None,
            selection="all",
            allow_native=False,
        )
        rows = plan["matrix"]["include"]
        self.assertEqual(plan["profile"], "pr")
        self.assertEqual(plan["retention_days"], RETENTION_DAYS["pr"])
        self.assertFalse(plan["requires_sealed_artifacts"])
        self.assertEqual(
            plan["backends"], sorted({row["backend"] for row in rows})
        )
        for row in rows:
            self.assertEqual(row["profile"], "pr")
            self.assertEqual(row["stage"], "full")
            # Pull requests stay structural: no container, no native install.
            self.assertFalse(row["container_lifecycle"])
            self.assertFalse(row["native_lifecycle"])
            self.assertLess(row["step_timeout_minutes"], row["timeout_minutes"])

    def test_deeper_profiles_earn_the_container_lifecycle_and_longer_retention(self) -> None:
        # main stays structural until the container lifecycle has retained
        # evidence; nightly and release are the first profiles that opt in.
        main = build_plan(
            event="push",
            ref="refs/heads/main",
            profile_input="main",
            changed_from=None,
            selection="all",
            allow_native=False,
        )
        main_depth = {
            row["backend"]: row["container_lifecycle"] for row in main["matrix"]["include"]
        }
        self.assertFalse(main_depth["deb"])
        self.assertFalse(main_depth["rpm"])

        for profile in ("nightly", "release"):
            with self.subTest(profile=profile):
                plan = build_plan(
                    event="workflow_call",
                    ref="refs/tags/v9.9.9",
                    profile_input=profile,
                    changed_from=None,
                    selection="all",
                    allow_native=False,
                )
                self.assertGreaterEqual(plan["retention_days"], RETENTION_DAYS["pr"])
                depth = {
                    row["backend"]: row["container_lifecycle"]
                    for row in plan["matrix"]["include"]
                }
                self.assertTrue(depth["deb"])
                self.assertTrue(depth["rpm"])
                # A container is never invented for a backend that has no image.
                for backend in ("freebsd", "openbsd", "macports"):
                    self.assertFalse(depth.get(backend, False))

    def test_the_release_hook_can_select_the_optional_backends_only(self) -> None:
        plan = build_plan(
            event="workflow_call",
            ref="refs/tags/v9.9.9",
            profile_input="release",
            changed_from=None,
            selection="optional",
            allow_native=False,
        )
        self.assertTrue(plan["requires_sealed_artifacts"])
        self.assertEqual(plan["backends"], ["deb", "homebrew", "macports", "rpm"])
        for row in plan["matrix"]["include"]:
            self.assertFalse(row["required_for_release"])

    def test_native_lifecycle_is_opt_in_and_only_for_the_macos_backends(self) -> None:
        plan = build_plan(
            event="workflow_dispatch",
            ref="refs/heads/main",
            profile_input="nightly",
            changed_from=None,
            selection="all",
            allow_native=True,
        )
        native = {row["backend"]: row["native_lifecycle"] for row in plan["matrix"]["include"]}
        self.assertTrue(native["macports"])
        self.assertTrue(native["homebrew"])
        for backend in ("deb", "rpm", "freebsd", "openbsd"):
            self.assertFalse(native[backend])

    def test_github_outputs_carry_the_strategy_scalars(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            outputs = Path(temporary) / "github-output"
            outputs.touch()
            completed = run(
                "plan",
                "--event",
                "schedule",
                "--ref",
                "refs/heads/main",
                "--output",
                str(Path(temporary) / "plan.json"),
                "--github-output",
                str(outputs),
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            values = dict(
                line.split("=", 1) for line in outputs.read_text(encoding="utf-8").splitlines()
            )
            self.assertEqual(values["profile"], "nightly")
            self.assertEqual(values["run"], "true")
            self.assertEqual(values["retention-days"], str(RETENTION_DAYS["nightly"]))
            matrix = json.loads(values["matrix"])
            self.assertTrue(matrix["include"])
            self.assertEqual(len(values["matrix"].splitlines()), 1)


class ClosureTests(unittest.TestCase):
    """The closure refuses a profile whose planned rows did not retain evidence."""

    def evidence_root(self) -> tuple[Path, Path]:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-package-ci-plan-")
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        (root / "evidence").mkdir()
        return root, root / "evidence"

    def plan_for(self, root: Path, backends: tuple[str, ...]) -> Path:
        completed = run("plan", "--event", "pull_request", "--output", str(root / "plan.json"))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        plan = json.loads((root / "plan.json").read_text(encoding="utf-8"))
        plan["matrix"]["include"] = [
            row for row in plan["matrix"]["include"] if row["backend"] in backends
        ]
        self.assertEqual(len(plan["matrix"]["include"]), len(backends))
        (root / "plan.json").write_text(json.dumps(plan), encoding="utf-8")
        return root / "plan.json"

    def produce(self, evidence: Path, row_id: str, backend: str, suffix: str) -> None:
        # Closure's --require-ci refuses local-unattested producers. Clear the
        # Actions identity so this helper always stamps local evidence, even when
        # Assurance itself runs under GITHUB_RUN_ID on a hosted runner.
        environment = os.environ.copy()
        for name in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_WORKFLOW_REF"):
            environment.pop(name, None)
        completed = subprocess.run(
            [
                "bash",
                str(ROOT / "scripts/package-ci.sh"),
                "--profile",
                "pr",
                "--backend",
                backend,
                "--output-dir",
                str(evidence / f"package-pr-{row_id}-{suffix}"),
            ],
            check=False,
            capture_output=True,
            text=True,
            cwd=ROOT,
            env=environment,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_a_complete_profile_closes_with_every_row_accounted_for(self) -> None:
        root, evidence = self.evidence_root()
        plan = self.plan_for(root, ("deb", "freebsd"))
        self.produce(evidence, "deb-debian-12-amd64", "deb", "abc123")
        self.produce(evidence, "freebsd-14-amd64", "freebsd", "abc123")
        completed = run(
            "close",
            "--plan",
            str(plan),
            "--evidence-root",
            str(evidence),
            "--artifact-suffix",
            "abc123",
            "--output",
            str(root / "closure.json"),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        closure = json.loads((root / "closure.json").read_text(encoding="utf-8"))
        self.assertEqual(
            sorted(entry["id"] for entry in closure["rows"]),
            ["deb-debian-12-amd64", "freebsd-14-amd64"],
        )
        self.assertNotIn("PASS", closure["results"])

    def test_a_missing_row_a_foreign_commit_and_local_evidence_are_refused(self) -> None:
        root, evidence = self.evidence_root()
        plan = self.plan_for(root, ("deb", "freebsd"))
        self.produce(evidence, "deb-debian-12-amd64", "deb", "abc123")

        missing = run(
            "close", "--plan", str(plan), "--evidence-root", str(evidence),
            "--artifact-suffix", "abc123",
        )
        self.assertEqual(missing.returncode, 1)
        self.assertIn("freebsd-14-amd64: no retained evidence directory", missing.stderr)

        self.produce(evidence, "freebsd-14-amd64", "freebsd", "abc123")
        foreign = run(
            "close", "--plan", str(plan), "--evidence-root", str(evidence),
            "--artifact-suffix", "abc123", "--expect-commit", "0" * 40,
        )
        self.assertEqual(foreign.returncode, 1)
        self.assertIn("not 0000000000", foreign.stderr)

        # Locally produced evidence can never close a retained CI profile.
        local = run(
            "close", "--plan", str(plan), "--evidence-root", str(evidence),
            "--artifact-suffix", "abc123", "--require-ci",
        )
        self.assertEqual(local.returncode, 1)
        self.assertIn("retained CI evidence is required", local.stderr)

    def test_a_skipped_plan_cannot_be_closed(self) -> None:
        root, evidence = self.evidence_root()
        plan = self.plan_for(root, ("deb",))
        value = json.loads(plan.read_text(encoding="utf-8"))
        value["run"] = False
        plan.write_text(json.dumps(value), encoding="utf-8")
        refused = run(
            "close", "--plan", str(plan), "--evidence-root", str(evidence),
            "--artifact-suffix", "abc123",
        )
        self.assertEqual(refused.returncode, 1)
        self.assertIn("skipped packaging plan", refused.stderr)


if __name__ == "__main__":
    unittest.main()
