from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from engineering.tools.validate_bsd_packaging import PackagingError, validate


ROOT = Path(__file__).resolve().parents[2]


class BsdPackagingTests(unittest.TestCase):
    def test_reference_ports_match_version_and_service_authorities(self) -> None:
        validate(ROOT)

    def test_release_mode_fails_without_native_tag_and_account_evidence(self) -> None:
        with self.assertRaisesRegex(PackagingError, "not proven"):
            validate(ROOT, release=True)

    def test_release_mode_does_not_require_a_circular_source_archive_distinfo(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-bsd-release-test-") as temporary:
            copied = Path(temporary)
            shutil.copy2(ROOT / "VERSION", copied / "VERSION")
            shutil.copy2(ROOT / "ABI_VERSION", copied / "ABI_VERSION")
            shutil.copy2(ROOT / "LICENSE", copied / "LICENSE")
            shutil.copytree(ROOT / "packaging", copied / "packaging")
            for platform in ("freebsd", "openbsd"):
                (copied / "packaging" / platform / "PORTS_ACCOUNT_REGISTERED").touch()

            validate(copied, release=True)
            self.assertFalse((copied / "packaging/freebsd/distinfo").exists())
            self.assertFalse((copied / "packaging/openbsd/distinfo").exists())

    def test_native_ci_consumes_installed_abi_and_retains_logs(self) -> None:
        freebsd_script = (ROOT / "scripts/ci-freebsd.sh").read_text(encoding="utf-8")
        openbsd_script = (ROOT / "scripts/ci-openbsd-libressl.sh").read_text(encoding="utf-8")
        for script in (freebsd_script, openbsd_script):
            self.assertIn("cmake --install", script)
            self.assertIn("tests/consumer/abi.c", script)
            self.assertIn("pkg-config --cflags --libs glifistore-abi", script)
            self.assertIn('LD_LIBRARY_PATH="$install_root/lib"', script)
        freebsd_workflow = (ROOT / ".github/workflows/freebsd.yml").read_text(encoding="utf-8")
        openbsd_workflow = (ROOT / ".github/workflows/openbsd-libressl.yml").read_text(
            encoding="utf-8"
        )
        for workflow in (freebsd_workflow, openbsd_workflow):
            self.assertIn("copyback: true", workflow)
            self.assertIn("        if: always()\n        uses: actions/upload-artifact@", workflow)
            self.assertIn("actions/upload-artifact@", workflow)
            self.assertIn("engineering/evidence/native-ci", workflow)
            self.assertIn("native_status=0", workflow)
            self.assertIn("|| native_status=$?", workflow)
            self.assertIn("if (( native_status != 0 )); then", workflow)
        self.assertIn("fail-fast: false", openbsd_workflow)
        self.assertEqual(openbsd_workflow.count("shard_index:"), 2)
        self.assertIn("GLIFISTORE_TEST_SHARD_COUNT=2", openbsd_workflow)
        self.assertIn("GLIFISTORE_TEST_SHARD_INDEX=${{ matrix.shard_index }}", openbsd_workflow)
        self.assertIn("openbsd-${{ matrix.shard_label }}.log", openbsd_workflow)
        self.assertIn("GLIFISTORE_TEST_SHARD_COUNT", openbsd_script)
        self.assertIn("GLIFISTORE_TEST_SHARD_INDEX", openbsd_script)
        self.assertIn('ctest --preset "$preset" --verbose --timeout 900', openbsd_script)
        self.assertIn("ensure_go_toolchain", openbsd_script)
        self.assertIn("1.27.1", openbsd_script)
        self.assertIn("go${version}.openbsd-${goarch}.tar.gz", openbsd_script)
        self.assertIn("db0566b3b3ddf6eda09cb3434a9401eb2583ffa539475e45592812813f11c792", openbsd_script)
        self.assertIn('actual_sha="$(sha256 -q "$work/$archive")"', openbsd_script)

    def test_abi_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-bsd-port-test-") as temporary:
            copied = Path(temporary)
            shutil.copy2(ROOT / "VERSION", copied / "VERSION")
            shutil.copy2(ROOT / "ABI_VERSION", copied / "ABI_VERSION")
            shutil.copy2(ROOT / "LICENSE", copied / "LICENSE")
            shutil.copytree(ROOT / "packaging", copied / "packaging")
            makefile = copied / "packaging/openbsd/Makefile"
            abi = (ROOT / "ABI_VERSION").read_text(encoding="utf-8").strip()
            makefile.write_text(
                makefile.read_text(encoding="utf-8").replace(
                    f"SHARED_LIBS +=  glifistore {abi}", "SHARED_LIBS +=  glifistore 2.0"
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(PackagingError, "OpenBSD ABI"):
                validate(copied)

    def test_license_metadata_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-bsd-license-test-") as temporary:
            copied = Path(temporary)
            shutil.copy2(ROOT / "VERSION", copied / "VERSION")
            shutil.copy2(ROOT / "ABI_VERSION", copied / "ABI_VERSION")
            shutil.copy2(ROOT / "LICENSE", copied / "LICENSE")
            shutil.copytree(ROOT / "packaging", copied / "packaging")
            makefile = copied / "packaging/freebsd/Makefile"
            makefile.write_text(
                makefile.read_text(encoding="utf-8").replace("BSD3CLAUSE", "APACHE20"),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(PackagingError, "FreeBSD BSD-3-Clause"):
                validate(copied)

    def test_openbsd_lifecycle_script_and_release_wiring_exist(self) -> None:
        script_path = ROOT / "scripts/test-openbsd-package-lifecycle.sh"
        self.assertTrue(script_path.is_file())
        self.assertTrue(script_path.stat().st_mode & 0o111)
        script = script_path.read_text(encoding="utf-8")
        release = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.assertIn('[[ "$(uname -s)" == "OpenBSD" ]]', script)
        self.assertIn("packaging/openbsd/PORTS_ACCOUNT_REGISTERED", script)
        self.assertIn("openbsd_package", script)
        self.assertIn("openbsd-package-evidence.json", script)
        self.assertIn("  openbsd-package-evidence:", release)
        self.assertIn("test-openbsd-package-lifecycle.sh", release)
        self.assertIn("vmactions/openbsd-vm@", release)
        self.assertIn("openbsd-package-evidence", release)
        freebsd = release.index("  freebsd-package-evidence:")
        openbsd = release.index("  openbsd-package-evidence:")
        sanitizers = release.index("  security-sanitizers:")
        self.assertLess(freebsd, openbsd)
        self.assertLess(openbsd, sanitizers)


if __name__ == "__main__":
    unittest.main()
