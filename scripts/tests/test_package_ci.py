from __future__ import annotations

import json
import os
import platform
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGE_CI = ROOT / "scripts/package-ci.sh"
WRAPPERS = (
    "package-build.sh",
    "package-inspect.sh",
    "package-install.sh",
    "package-verify.sh",
    "package-upgrade.sh",
    "package-remove.sh",
)


def run(script: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    for name in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_WORKFLOW_REF"):
        environment.pop(name, None)
    return subprocess.run(
        ["bash", str(script), *arguments],
        check=False,
        capture_output=True,
        text=True,
        cwd=ROOT,
        env=environment,
    )


class PackageCiTests(unittest.TestCase):
    def output_directory(self) -> Path:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-package-ci-")
        self.addCleanup(directory.cleanup)
        return Path(directory.name) / "run"

    def evidence(self, directory: Path, backend: str, profile: str, stage: str) -> dict:
        path = directory / backend / stage / f"{backend}-{profile}-{stage}-package-evidence.json"
        self.assertTrue(path.is_file(), f"missing evidence: {path}")
        return json.loads(path.read_text(encoding="utf-8"))

    def test_the_deb_backend_renders_metadata_and_blocks_the_rest(self) -> None:
        directory = self.output_directory()
        completed = run(PACKAGE_CI, "--profile", "pr", "--backend", "deb", "--output-dir", str(directory))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        # Non-Linux hosts cannot build a .deb at all → BLOCKED. Linux hosts can,
        # but the container/native lifecycle is opt-in → NOT_RUN until the flag is set.
        expected = "BLOCKED" if platform.system() != "Linux" else "NOT_RUN"
        self.assertIn(f"PACKAGE-CI deb pr full {expected}", completed.stdout)

        self.assertTrue((directory / "release-context.json").is_file())
        self.assertTrue((directory / "package-matrix.json").is_file())
        evidence = self.evidence(directory, "deb", "pr", "full")
        self.assertEqual(evidence["result"], expected)
        self.assertEqual(evidence["lifecycle_state"], "STRUCTURAL")
        statuses = {check["id"]: check["status"] for check in evidence["checks"]}
        self.assertEqual(statuses["structural-metadata"], "PASS")
        self.assertEqual(statuses["package-metadata-render"], "PASS")
        self.assertEqual(statuses["package-build"], expected)
        self.assertEqual(statuses["package-install"], expected)
        self.assertEqual(statuses["package-upgrade"], "NOT_APPLICABLE_INITIAL_BASELINE")
        self.assertTrue(evidence["residuals"])
        self.assertTrue((directory / "deb/full/metadata/debian/control").is_file())

    def test_the_release_context_drives_the_package_version(self) -> None:
        directory = self.output_directory()
        completed = run(
            PACKAGE_CI,
            "--profile",
            "pr",
            "--backend",
            "deb",
            "--package-revision",
            "2",
            "--output-dir",
            str(directory),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        context = json.loads((directory / "release-context.json").read_text(encoding="utf-8"))
        evidence = self.evidence(directory, "deb", "pr", "full")
        self.assertEqual(evidence["package_revision"], 2)
        self.assertEqual(
            evidence["package_version"], context["package_versions"]["deb"]["package_version"]
        )
        self.assertEqual(evidence["product_version"], (ROOT / "VERSION").read_text().strip())

    def test_the_bsd_backends_run_the_reference_port_validator(self) -> None:
        directory = self.output_directory()
        completed = run(
            PACKAGE_CI, "--profile", "pr", "--backend", "freebsd", "--output-dir", str(directory)
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        evidence = self.evidence(directory, "freebsd", "pr", "full")
        statuses = {check["id"]: check["status"] for check in evidence["checks"]}
        self.assertEqual(statuses["reference-port-structure"], "PASS")
        self.assertEqual(evidence["result"], "OPEN_GATE")
        # The native rows stay BLOCKED until a native host, the upstream account
        # marker and a sealed candidate hold together (scripts/lib/package-backend-bsd.sh).
        self.assertEqual(statuses["package-install"], "BLOCKED")
        self.assertTrue(
            any("PORTS_ACCOUNT_REGISTERED" in item for item in evidence["limitations"])
        )
        self.assertFalse((ROOT / "packaging/freebsd/PORTS_ACCOUNT_REGISTERED").exists())

    def test_every_profile_backend_runs_with_all(self) -> None:
        directory = self.output_directory()
        completed = run(PACKAGE_CI, "--profile", "pr", "--all", "--output-dir", str(directory))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        reported = sorted(
            line.split()[1] for line in completed.stdout.splitlines() if line.startswith("PACKAGE-CI ")
        )
        self.assertEqual(reported, ["deb", "freebsd", "macports", "openbsd"])
        self.assertNotIn("PASS", completed.stdout)

    def test_unknown_or_out_of_profile_backends_are_refused(self) -> None:
        directory = self.output_directory()
        unknown = run(
            PACKAGE_CI, "--profile", "pr", "--backend", "windows", "--output-dir", str(directory)
        )
        self.assertEqual(unknown.returncode, 2)
        self.assertIn("does not run in profile", unknown.stderr)

        out_of_profile = run(
            PACKAGE_CI, "--profile", "pr", "--backend", "rpm", "--output-dir", str(directory)
        )
        self.assertEqual(out_of_profile.returncode, 2)
        self.assertIn("rpm", out_of_profile.stderr)

    def test_invalid_invocations_are_refused(self) -> None:
        for arguments in (
            ("--backend", "deb"),
            ("--profile", "hotfix", "--backend", "deb"),
            ("--profile", "pr", "--backend", "deb", "--stage", "bless"),
            ("--profile", "pr", "--all", "--backend", "deb"),
            ("--profile", "pr",),
        ):
            with self.subTest(arguments=arguments):
                self.assertEqual(run(PACKAGE_CI, *arguments).returncode, 2)

    def test_the_thin_wrappers_delegate_a_single_stage(self) -> None:
        directory = self.output_directory()
        completed = run(
            ROOT / "scripts/package-build.sh",
            "--profile",
            "pr",
            "--backend",
            "deb",
            "--output-dir",
            str(directory),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(self.evidence(directory, "deb", "pr", "build")["stage"], "build")

    def test_the_wrappers_stay_thin_and_no_version_is_hard_coded(self) -> None:
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        entry_point = PACKAGE_CI.read_text(encoding="utf-8")
        self.assertNotIn(version, entry_point)
        self.assertNotIn("continue-on-error", entry_point)
        self.assertIn("set -euo pipefail", entry_point)
        for name in WRAPPERS:
            with self.subTest(wrapper=name):
                path = ROOT / "scripts" / name
                self.assertTrue(path.stat().st_mode & 0o111)
                body = path.read_text(encoding="utf-8")
                self.assertIn('exec "$root/scripts/package-ci.sh" --stage', body)
                self.assertLess(len(body.splitlines()), 12)


if __name__ == "__main__":
    unittest.main()
