from __future__ import annotations

import re
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGE_CI = ROOT / ".github/workflows/package-ci.yml"
RELEASE = ROOT / ".github/workflows/release.yml"

sys.path.insert(0, str(ROOT))

from engineering.tools.generate_package_matrix import load_matrix  # noqa: E402
from engineering.tools.package_framework import BACKENDS  # noqa: E402


class PackageWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = PACKAGE_CI.read_text(encoding="utf-8")
        self.triggers = self.workflow[self.workflow.index("\non:\n") : self.workflow.index("\npermissions:")]
        self.jobs = self.workflow[self.workflow.index("\njobs:\n") :]

    def test_the_lifecycle_is_invoked_and_never_reimplemented_in_yaml(self) -> None:
        self.assertIn("./scripts/package-ci.sh \"${arguments[@]}\"", self.workflow)
        self.assertIn("./scripts/package-\"$wrapper\".sh", self.workflow)
        for forbidden in (
            "dpkg-buildpackage",
            "rpmbuild",
            "brew install",
            "port install",
            "docker run",
        ):
            with self.subTest(command=forbidden):
                self.assertNotIn(forbidden, self.workflow)

    def test_the_matrix_is_generated_not_restated(self) -> None:
        self.assertIn("engineering/tools/generate_package_matrix.py validate", self.workflow)
        self.assertIn("matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}", self.workflow)
        self.assertIn("runs-on: ${{ matrix.runner }}", self.workflow)

        matrix = load_matrix()
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        self.assertNotIn(version, self.workflow)
        for backend in matrix["backends"]:
            for target in backend["targets"]:
                with self.subTest(target=target["id"]):
                    self.assertNotIn(target["id"], self.workflow)
                    self.assertNotIn(target["platform"], self.workflow)
                    if target["container"]:
                        self.assertNotIn(target["container"], self.workflow)
                        self.assertNotIn(target["container_digest"], self.workflow)
        # Not even a single backend id is spelled out: every loop reads the plan.
        for backend in BACKENDS:
            with self.subTest(backend=backend):
                self.assertNotIn(f"--backend {backend}", self.workflow)

    def test_change_detection_is_centralised_and_fails_closed(self) -> None:
        self.assertNotIn("paths:", self.triggers)
        self.assertNotIn("paths-ignore:", self.triggers)
        self.assertIn("engineering/tools/package_ci_plan.py", self.workflow)
        self.assertIn("rm -f build/changed-paths.txt", self.workflow)
        self.assertIn("if [[ -f build/changed-paths.txt ]]; then", self.workflow)
        self.assertIn("if: needs.plan.outputs.run == 'true'", self.workflow)

    def test_every_job_and_blocking_step_is_bounded(self) -> None:
        jobs = re.findall(r"\n  ([a-z-]+):\n", self.jobs)
        self.assertEqual(sorted(jobs), ["backends", "closure", "extended", "plan"])
        # Every job plus the lifecycle, negative-suite and stage-sweep steps.
        self.assertEqual(self.workflow.count("timeout-minutes:"), len(jobs) + 3)
        self.assertIn("timeout-minutes: ${{ matrix.timeout_minutes }}", self.workflow)
        self.assertIn("timeout-minutes: ${{ matrix.step_timeout_minutes }}", self.workflow)

    def test_work_directories_are_isolated_per_run_outside_the_checkout(self) -> None:
        self.assertIn(
            'work="${RUNNER_TEMP}/package-ci/$(cat VERSION)/$(git rev-parse --short=12 HEAD)"',
            self.workflow,
        )
        self.assertIn(
            'work="${work}/${PROFILE}/${BACKEND}/${PLATFORM}/${GITHUB_RUN_ID}"',
            self.workflow,
        )

    def test_evidence_is_validated_and_retained_per_profile_policy(self) -> None:
        self.assertIn("validate_package_evidence.py validate", self.workflow)
        self.assertIn("--require-ci", self.workflow)
        self.assertIn("package_ci_plan.py close", self.workflow)
        self.assertIn("--expect-commit \"$GITHUB_SHA\"", self.workflow)
        self.assertEqual(
            self.workflow.count("retention-days: ${{ needs.plan.outputs.retention-days }}"), 4
        )
        self.assertIn("retention-days: ${{ steps.plan.outputs.retention-days }}", self.workflow)
        self.assertEqual(
            self.workflow.count("if-no-files-found: error"),
            self.workflow.count("uses: actions/upload-artifact@"),
        )
        # A cache is a toolchain convenience and is never an artifact store.
        self.assertNotIn("actions/cache", self.workflow)

    def test_wave_f_admission_runs_when_a_sealed_candidate_is_supplied(self) -> None:
        self.assertIn("engineering/tools/run_package_admission.py", self.workflow)
        self.assertIn("package-admission-${{ needs.plan.outputs.profile }}-${{ github.sha }}", self.workflow)
        self.assertIn("if: inputs.candidate-artifact != ''", self.workflow)
        self.assertIn("--allow-blocking", self.workflow)

    def test_container_rows_package_language_sdk_archives_for_the_installed_matrix(self) -> None:
        self.assertIn("package-language-sdk-archives.sh", self.workflow)
        self.assertIn("matrix.container_lifecycle == true", self.workflow)
        self.assertIn("actions/setup-go@", self.workflow)
        self.assertIn("ruby/setup-ruby@", self.workflow)
        self.assertTrue((ROOT / "scripts/package-language-sdk-archives.sh").is_file())
        self.assertTrue(
            (ROOT / "scripts/packaging/ensure-installed-sdk-matrix-tools.sh").is_file()
        )
        matrix_tools = (
            ROOT / "scripts/packaging/ensure-installed-sdk-matrix-tools.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("1.27.1", matrix_tools)
        self.assertIn("63d339f0da5ab53635a56f2490a7984dfe12dfcff22ad749f63edaf590168445", matrix_tools)
        self.assertIn("sha256sum -c -", matrix_tools)
        self.assertNotIn("https://mise.run", matrix_tools)
        self.assertIn("2026.9.5", matrix_tools)
        self.assertIn("d71e94e1ed59d4d0ca4ac847fa321d6d6615a8e613e9b468c9fb39f0dddd06d5", matrix_tools)
        self.assertIn("$work/mise/bin/mise", matrix_tools)

    def test_nothing_is_allowed_to_fail_softly(self) -> None:
        for forbidden in ("continue-on-error", "|| true", "set +e", "if: always() || "):
            with self.subTest(pattern=forbidden):
                self.assertNotIn(forbidden, self.workflow)
        self.assertEqual(self.workflow.count("set -euo pipefail"), self.workflow.count("run: |\n"))

    def test_every_action_is_pinned_to_a_commit_sha(self) -> None:
        for reference in re.findall(r"uses: (\S+)", self.workflow):
            with self.subTest(uses=reference):
                if reference.startswith("./"):
                    continue
                self.assertRegex(reference, r"@[0-9a-f]{40}$")


class ReleaseIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.release = RELEASE.read_text(encoding="utf-8")

    def test_the_release_hook_carries_the_sealed_candidate_and_stays_optional(self) -> None:
        self.assertIn("uses: ./.github/workflows/package-ci.yml", self.release)
        hook = self.release[self.release.index("  optional-package-backends:") :]
        hook = hook[: hook.index("\n  security-sanitizers:")]
        self.assertIn("profile: release", hook)
        self.assertIn('backends: optional', hook)
        self.assertIn(
            "candidate-artifact: ${{ needs.candidate.outputs.artifact-name }}", hook
        )
        self.assertIn(
            "candidate-seal-sha256: ${{ needs.candidate.outputs.candidate-seal-sha256 }}",
            hook,
        )
        self.assertNotIn("continue-on-error", hook)

    def test_the_hook_cannot_admit_an_artifact_or_relax_the_exact_byte_rules(self) -> None:
        verify = self.release[self.release.index("  verify:") :]
        needs = verify[: verify.index("\n    runs-on:")]
        self.assertNotIn("optional-package-backends", needs)
        # The admitted evidence namespace stays exactly what verify imports.
        self.assertNotIn("release-input-${{ github.sha }}-package", self.release)
        self.assertIn("release $RELEASE_TAG already exists; published assets are immutable", self.release)
        self.assertIn("validate-release-policy", self.release)
        self.assertNotIn("--clobber", self.release)

    def test_required_bsd_artifacts_keep_their_own_native_jobs(self) -> None:
        for job in ("freebsd-package-evidence:", "openbsd-package-evidence:"):
            self.assertIn(job, self.release)
        matrix = load_matrix()
        required = {
            backend["id"] for backend in matrix["backends"] if backend["required_for_release"]
        }
        self.assertEqual(required, {"freebsd", "openbsd"})

    def test_wave_f_admission_is_wired_for_required_packages(self) -> None:
        self.assertIn("  package-admission:", self.release)
        job = self.release[self.release.index("  package-admission:") :]
        job = job[: job.index("\n  security-sanitizers:")]
        self.assertIn("engineering/tools/run_package_admission.py", job)
        self.assertIn("--profile release", job)
        self.assertIn("--allow-blocking", job)
        self.assertIn("package-admission-release-${{ github.sha }}", job)
        verify = self.release[self.release.index("  verify:") :]
        needs = verify[: verify.index("\n    runs-on:")]
        self.assertNotIn("package-admission", needs)


class ContainerEvidenceIdentityTests(unittest.TestCase):
    """Container evidence is emitted inside the image, so it must inherit the run."""

    def test_the_container_dispatch_forwards_the_ci_identity(self) -> None:
        from engineering.tools import run_linux_package_backend as driver

        captured: list[list[str]] = []

        def capture(command, **_: object) -> bool:
            captured.append(list(command))
            return True

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            original = driver._run
            driver._run = capture  # type: ignore[assignment]
            environment = {
                "GITHUB_RUN_ID": "4242",
                "GITHUB_WORKFLOW_REF": "owner/repo/.github/workflows/package-ci.yml@refs/heads/main",
                "RUNNER_OS": "Linux",
                "RUNNER_ARCH": "X64",
            }
            previous = {name: driver.os.environ.get(name) for name in environment}
            driver.os.environ.update(environment)
            try:
                driver.dispatch_container(
                    driver.Recorder(directory),
                    backend="deb",
                    profile="main",
                    stage="full",
                    root=ROOT,
                    release_context=directory / "release-context.json",
                    target=driver.Target("deb-target", "docker.io/library/debian:12", "sha256:" + "0" * 64),
                    runtime="docker",
                )
            finally:
                driver._run = original  # type: ignore[assignment]
                for name, value in previous.items():
                    if value is None:
                        driver.os.environ.pop(name, None)
                    else:
                        driver.os.environ[name] = value

        self.assertEqual(len(captured), 5)
        remove_prior, create, wait_probe, execute, remove = (
            captured[0],
            captured[1],
            captured[2],
            captured[3],
            captured[4],
        )
        create_joined = " ".join(create)
        for name, value in environment.items():
            self.assertIn(f"{name}={value}", create)
        self.assertEqual(remove_prior[:3], ["docker", "rm", "-f"])
        self.assertIn("-d", create)
        self.assertIn("--privileged", create)
        self.assertIn("--cgroupns=host", create_joined)
        self.assertIn("linux-systemd-pid1.sh", create_joined)
        self.assertIn("test -d /run/systemd/system", " ".join(wait_probe))
        self.assertIn("linux-container-entry.sh", " ".join(execute))
        self.assertEqual(execute[:2], ["docker", "exec"])
        self.assertEqual(remove[:3], ["docker", "rm", "-f"])


if __name__ == "__main__":
    unittest.main()
