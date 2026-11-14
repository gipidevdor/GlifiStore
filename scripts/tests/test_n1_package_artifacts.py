"""Unit tests for sealed N-1 package directory selection."""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from engineering.tools.n1_package_artifacts import (
    CONTAINER_N1_MOUNT,
    N1_PACKAGE_DIR_ENVIRONMENT,
    N1PackageError,
    select_bsd_n1_packages,
    select_linux_n1_packages,
    select_macos_n1_source,
    supplied_n1_package_dir,
    upgrade_exercise_requested,
)
from engineering.tools.run_linux_package_backend import (
    Recorder,
    container_mount_and_environment,
    finalize_deferred_upgrade,
    run_upgrade,
)


ROOT = Path(__file__).resolve().parents[2]


class N1PackageArtifactsTests(unittest.TestCase):
    def directory(self) -> Path:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-n1-")
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def test_unset_environment_means_no_directory(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(N1_PACKAGE_DIR_ENVIRONMENT, None)
            self.assertIsNone(supplied_n1_package_dir())
            self.assertFalse(upgrade_exercise_requested({"available": True}))

    def test_a_missing_directory_is_refused(self) -> None:
        missing = self.directory() / "absent"
        with mock.patch.dict(os.environ, {N1_PACKAGE_DIR_ENVIRONMENT: str(missing)}):
            with self.assertRaisesRegex(N1PackageError, "not a regular directory"):
                supplied_n1_package_dir()

    def test_linux_selection_keeps_primary_packages_for_the_requested_version(self) -> None:
        root = self.directory()
        (root / "glifistore_0.0.9-1_amd64.deb").write_bytes(b"a")
        (root / "libglifistore1_0.0.9-1_amd64.deb").write_bytes(b"b")
        (root / "libglifistore-dev_0.0.9-1_amd64.deb").write_bytes(b"c")
        (root / "glifistore_0.0.9-1_amd64.deb.debuginfo").write_bytes(b"x")
        (root / "other_0.0.9-1_amd64.deb").write_bytes(b"y")
        (root / "glifistore_0.1.0-1_amd64.deb").write_bytes(b"z")
        selected = select_linux_n1_packages(root, "deb", "0.0.9")
        self.assertEqual(
            [path.name for path in selected],
            [
                "glifistore_0.0.9-1_amd64.deb",
                "libglifistore-dev_0.0.9-1_amd64.deb",
                "libglifistore1_0.0.9-1_amd64.deb",
            ],
        )

    def test_linux_selection_refuses_substring_version_false_positives(self) -> None:
        root = self.directory()
        (root / "glifistore_10.0.9-1_amd64.deb").write_bytes(b"a")
        (root / "glifistore_0.0.90-1_amd64.deb").write_bytes(b"b")
        with self.assertRaisesRegex(N1PackageError, "no sealed deb packages"):
            select_linux_n1_packages(root, "deb", "0.0.9")

    def test_linux_selection_refuses_partial_semver_prefix(self) -> None:
        root = self.directory()
        (root / "glifistore_0.1.0-1_amd64.deb").write_bytes(b"a")
        with self.assertRaisesRegex(N1PackageError, "no sealed deb packages"):
            select_linux_n1_packages(root, "deb", "0.1")

    def test_linux_selection_refuses_mixed_architectures_without_a_filter(self) -> None:
        root = self.directory()
        (root / "glifistore_0.0.9-1_amd64.deb").write_bytes(b"a")
        (root / "glifistore_0.0.9-1_arm64.deb").write_bytes(b"b")
        with self.assertRaisesRegex(N1PackageError, "multiple architectures"):
            select_linux_n1_packages(root, "deb", "0.0.9")
        selected = select_linux_n1_packages(root, "deb", "0.0.9", arch="amd64")
        self.assertEqual([path.name for path in selected], ["glifistore_0.0.9-1_amd64.deb"])

    def test_sha256sums_are_verified_when_present(self) -> None:
        root = self.directory()
        package = root / "glifistore_0.0.9-1_amd64.deb"
        package.write_bytes(b"sealed")
        digest = __import__("hashlib").sha256(b"sealed").hexdigest()
        (root / "SHA256SUMS").write_text(
            f"{digest}  glifistore_0.0.9-1_amd64.deb\n", encoding="utf-8"
        )
        selected = select_linux_n1_packages(root, "deb", "0.0.9")
        self.assertEqual([path.name for path in selected], [package.name])
        (root / "SHA256SUMS").write_text(
            f"{'0' * 64}  glifistore_0.0.9-1_amd64.deb\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(N1PackageError, "digest mismatch"):
            select_linux_n1_packages(root, "deb", "0.0.9")

    def test_linux_selection_refuses_an_empty_match(self) -> None:
        root = self.directory()
        (root / "glifistore_0.1.0-1_amd64.deb").write_bytes(b"z")
        with self.assertRaisesRegex(N1PackageError, "no sealed deb packages"):
            select_linux_n1_packages(root, "deb", "0.0.9")

    def test_rpm_selection_skips_debuginfo(self) -> None:
        root = self.directory()
        (root / "glifistore-0.0.9-1.x86_64.rpm").write_bytes(b"a")
        (root / "glifistore-debuginfo-0.0.9-1.x86_64.rpm").write_bytes(b"b")
        selected = select_linux_n1_packages(root, "rpm", "0.0.9")
        self.assertEqual([path.name for path in selected], ["glifistore-0.0.9-1.x86_64.rpm"])

    def test_freebsd_selection_keeps_versioned_packages(self) -> None:
        root = self.directory()
        (root / "glifistore-0.0.9-freebsd14.3-amd64.pkg").write_bytes(b"a")
        (root / "glifistore-0.1.0-freebsd14.3-amd64.pkg").write_bytes(b"b")
        (root / "other-0.0.9.pkg").write_bytes(b"c")
        selected = select_bsd_n1_packages(root, "freebsd", "0.0.9")
        self.assertEqual(
            [path.name for path in selected],
            ["glifistore-0.0.9-freebsd14.3-amd64.pkg"],
        )

    def test_openbsd_selection_keeps_versioned_tgz(self) -> None:
        root = self.directory()
        (root / "glifistore-0.0.9-openbsd7.7-amd64.tgz").write_bytes(b"a")
        selected = select_bsd_n1_packages(root, "openbsd", "0.0.9")
        self.assertEqual(
            [path.name for path in selected],
            ["glifistore-0.0.9-openbsd7.7-amd64.tgz"],
        )

    def test_macos_selection_requires_the_exact_source_archive(self) -> None:
        root = self.directory()
        (root / "GlifiStore-0.0.9.tar.xz").write_bytes(b"a")
        (root / "GlifiStore-0.1.0.tar.xz").write_bytes(b"b")
        selected = select_macos_n1_source(root, "0.0.9")
        self.assertEqual(selected.name, "GlifiStore-0.0.9.tar.xz")

    def test_macos_selection_refuses_duplicates(self) -> None:
        root = self.directory()
        (root / "a").mkdir()
        (root / "b").mkdir()
        (root / "a" / "GlifiStore-0.0.9.tar.xz").write_bytes(b"a")
        (root / "b" / "GlifiStore-0.0.9.tar.xz").write_bytes(b"b")
        with self.assertRaisesRegex(N1PackageError, "multiple sealed macOS source"):
            select_macos_n1_source(root, "0.0.9")


class LinuxUpgradeSelectionTests(unittest.TestCase):
    def recorder(self) -> Recorder:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-upgrade-")
        self.addCleanup(temporary.cleanup)
        return Recorder(Path(temporary.name))

    def test_initial_baseline_is_not_applicable(self) -> None:
        recorder = self.recorder()
        run_upgrade(
            recorder,
            {"previous": {"available": False, "reason": "nothing precedes"}},
        )
        self.assertEqual(
            recorder.statuses["package-upgrade"], "NOT_APPLICABLE_INITIAL_BASELINE"
        )

    def test_n1_directory_without_a_predecessor_is_refused(self) -> None:
        recorder = self.recorder()
        directory = tempfile.TemporaryDirectory(prefix="glifistore-n1-dir-")
        self.addCleanup(directory.cleanup)
        with mock.patch.dict(os.environ, {N1_PACKAGE_DIR_ENVIRONMENT: directory.name}):
            run_upgrade(
                recorder,
                {"previous": {"available": False, "reason": "nothing precedes"}},
            )
        self.assertEqual(recorder.statuses["package-upgrade"], "FAIL")
        self.assertIn(N1_PACKAGE_DIR_ENVIRONMENT, recorder.details["package-upgrade"])

    def test_a_predecessor_without_sealed_bytes_stays_not_run(self) -> None:
        recorder = self.recorder()
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop(N1_PACKAGE_DIR_ENVIRONMENT, None)
            run_upgrade(
                recorder,
                {
                    "previous": {
                        "available": True,
                        "tag": "v0.0.9",
                        "version": "0.0.9",
                        "reason": "fixture",
                    }
                },
            )
        self.assertEqual(recorder.statuses["package-upgrade"], "NOT_RUN")
        self.assertIn(N1_PACKAGE_DIR_ENVIRONMENT, recorder.details["package-upgrade"])

    def test_sealed_bytes_defer_recording_until_the_native_walk(self) -> None:
        recorder = self.recorder()
        directory = tempfile.TemporaryDirectory(prefix="glifistore-n1-dir-")
        self.addCleanup(directory.cleanup)
        context = {
            "previous": {
                "available": True,
                "tag": "v0.0.9",
                "version": "0.0.9",
                "reason": "fixture",
            }
        }
        with mock.patch.dict(os.environ, {N1_PACKAGE_DIR_ENVIRONMENT: directory.name}):
            run_upgrade(recorder, context)
            self.assertNotIn("package-upgrade", recorder.statuses)
            finalize_deferred_upgrade(recorder, context)
            self.assertEqual(recorder.statuses["package-upgrade"], "NOT_RUN")
            self.assertIn("did not run", recorder.details["package-upgrade"])

    def test_container_mount_forwards_the_n1_directory(self) -> None:
        mounts, environment = container_mount_and_environment(
            root=ROOT,
            output=ROOT / "build",
            release_context=ROOT / "VERSION",
            n1_packages="/host/n1",
        )
        joined_mounts = " ".join(mounts)
        self.assertIn(f"/host/n1:{CONTAINER_N1_MOUNT}:ro", joined_mounts)
        self.assertIn(f"{N1_PACKAGE_DIR_ENVIRONMENT}={CONTAINER_N1_MOUNT}", " ".join(environment))


if __name__ == "__main__":
    unittest.main()
