from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from engineering.tools.package_framework import validate_against_schema  # noqa: E402


HARNESS = ROOT / "scripts" / "test-package-installed-sdk-matrix.sh"
SDK_ARCHIVES = (
    ("python", "sdk/python/dist", "glifistore-*.whl"),
    ("perl", "sdk/perl/dist", "GlifiStore-*.tar.gz"),
    ("ruby", "sdk/ruby/dist", "glifistore-*.gem"),
    ("go", "sdk/go/dist", "glifistore-go-*.tar.gz"),
    ("erlang", "sdk/erlang/dist", "glifistore-erlang-*.tar.gz"),
)


def sealed_sdk_archives_present() -> bool:
    return all(
        len(list((ROOT / directory).glob(pattern))) == 1 for _, directory, pattern in SDK_ARCHIVES
    )


class InstalledSdkMatrixHarnessTests(unittest.TestCase):
    """The harness may only claim a packaged daemon it can prove; everything else is honest."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-installed-sdk-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.prefix = self.work / "prefix"
        (self.prefix / "bin").mkdir(parents=True)
        self.daemon = self.prefix / "bin" / "glifistored"
        self.daemon.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.daemon.chmod(0o755)
        self.inventory = self.work / "pkg-contents.txt"
        self.inventory.write_text(f"{self.prefix}/bin\n{self.daemon}\n", encoding="utf-8")

    def run_harness(self, *, report: str = "report.json", **environment) -> tuple[int, Path, str]:
        path = self.work / report
        merged = dict(os.environ)
        merged.pop("GLIFISTORED", None)
        merged.pop("GLIFISTORE_PACKAGE_DAEMON", None)
        merged.pop("GLIFISTORE_PACKAGE_FILE_LIST", None)
        merged.pop("GLIFISTORE_PACKAGE_PREFIX", None)
        merged.update({key: str(value) for key, value in environment.items()})
        completed = subprocess.run(
            ["bash", str(HARNESS), "--report", str(path)],
            check=False,
            capture_output=True,
            text=True,
            env=merged,
        )
        return completed.returncode, path, completed.stdout + completed.stderr

    def report(self, path: Path) -> dict:
        value = json.loads(path.read_text(encoding="utf-8"))
        validate_against_schema(
            value, "installed-sdk-matrix.schema.json", "installed SDK matrix report"
        )
        return value

    def test_without_a_declared_packaged_daemon_the_matrix_is_not_run(self) -> None:
        status, path, _ = self.run_harness()
        self.assertEqual(status, 0)
        value = self.report(path)
        self.assertEqual(value["result"], "NOT_RUN")
        self.assertFalse(value["package_installed"])
        self.assertEqual(value["languages"], [])
        self.assertIn("neither is inferred", value["reason"])

    def test_a_daemon_the_package_inventory_does_not_own_is_blocked(self) -> None:
        self.inventory.write_text(f"{self.prefix}/bin/somethingelse\n", encoding="utf-8")
        status, path, _ = self.run_harness(
            GLIFISTORE_PACKAGE_DAEMON=self.daemon,
            GLIFISTORE_PACKAGE_FILE_LIST=self.inventory,
        )
        self.assertEqual(status, 1)
        value = self.report(path)
        self.assertEqual(value["result"], "BLOCKED")
        self.assertFalse(value["package_installed"])
        self.assertIn("does not own", value["reason"])

    def test_a_daemon_from_the_source_checkout_is_blocked(self) -> None:
        status, path, _ = self.run_harness(
            GLIFISTORE_PACKAGE_DAEMON=ROOT / "scripts" / "package-ci.sh",
            GLIFISTORE_PACKAGE_FILE_LIST=self.inventory,
        )
        self.assertEqual(status, 1)
        self.assertIn("inside the source checkout", self.report(path)["reason"])

    def test_a_daemon_outside_the_declared_prefix_is_blocked(self) -> None:
        other = self.work / "other-prefix"
        other.mkdir()
        status, path, _ = self.run_harness(
            GLIFISTORE_PACKAGE_DAEMON=self.daemon,
            GLIFISTORE_PACKAGE_FILE_LIST=self.inventory,
            GLIFISTORE_PACKAGE_PREFIX=other,
        )
        self.assertEqual(status, 1)
        self.assertIn("outside the declared installed prefix", self.report(path)["reason"])

    def test_a_non_executable_daemon_is_blocked(self) -> None:
        self.daemon.chmod(0o644)
        status, path, _ = self.run_harness(
            GLIFISTORE_PACKAGE_DAEMON=self.daemon,
            GLIFISTORE_PACKAGE_FILE_LIST=self.inventory,
        )
        self.assertEqual(status, 1)
        self.assertIn("not executable", self.report(path)["reason"])

    def test_a_missing_package_inventory_is_blocked(self) -> None:
        status, path, _ = self.run_harness(
            GLIFISTORE_PACKAGE_DAEMON=self.daemon,
            GLIFISTORE_PACKAGE_FILE_LIST=self.work / "absent.txt",
        )
        self.assertEqual(status, 1)
        self.assertIn("file inventory is missing", self.report(path)["reason"])

    @unittest.skipIf(
        sealed_sdk_archives_present(),
        "this checkout has every sealed SDK archive, so the delegated matrix would really run",
    )
    def test_without_sealed_sdk_archives_the_matrix_is_not_run(self) -> None:
        status, path, _ = self.run_harness(
            GLIFISTORE_PACKAGE_DAEMON=self.daemon,
            GLIFISTORE_PACKAGE_FILE_LIST=self.inventory,
        )
        self.assertEqual(status, 0)
        value = self.report(path)
        self.assertEqual(value["result"], "NOT_RUN")
        self.assertTrue(value["package_installed"])
        self.assertIn("no sealed SDK distribution archive", value["reason"])

    def test_an_existing_report_is_never_silently_replaced(self) -> None:
        path = self.work / "existing.json"
        path.write_text("do not replace\n", encoding="utf-8")
        completed = subprocess.run(
            ["bash", str(HARNESS), "--report", str(path)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("refusing to replace", completed.stderr)
        self.assertEqual(path.read_text(encoding="utf-8"), "do not replace\n")

    def test_the_harness_requires_a_report_destination(self) -> None:
        completed = subprocess.run(
            ["bash", str(HARNESS)], check=False, capture_output=True, text=True
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("usage:", completed.stderr)


if __name__ == "__main__":
    unittest.main()
