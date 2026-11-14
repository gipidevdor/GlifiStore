from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/verify_hardening.py"


class VerifyHardeningTests(unittest.TestCase):
    def run_verifier(self, *arguments: str, path: Path | None = None) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        if path is not None:
            environment["PATH"] = f"{path}{os.pathsep}{environment.get('PATH', '')}"
        return subprocess.run(
            [sys.executable, str(SCRIPT), *arguments],
            cwd=ROOT,
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_binary_mode_inspects_exact_executable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-hardening-test-") as temporary:
            directory = Path(temporary)
            binary = directory / "glifistored"
            binary.write_bytes(b"ELF fixture")
            binary.chmod(0o755)
            readelf = directory / "readelf"
            readelf.write_text(
                """#!/bin/sh
case "$1" in
  -h) echo 'Type: DYN (Position-Independent Executable file)' ;;
  -l) echo 'GNU_RELRO' ;;
  -d) echo 'FLAGS BIND_NOW NOW' ;;
  -Ws) echo '__stack_chk_fail' ;;
  *) exit 2 ;;
esac
""",
                encoding="utf-8",
            )
            readelf.chmod(0o755)

            result = self.run_verifier("--binary", str(binary), path=directory)

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("distributed ELF properties", result.stdout)

    def test_binary_mode_rejects_symlink_before_resolution(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-hardening-test-") as temporary:
            directory = Path(temporary)
            target = directory / "target"
            target.write_bytes(b"ELF fixture")
            target.chmod(0o755)
            link = directory / "glifistored"
            link.symlink_to(target)

            result = self.run_verifier("--binary", str(link))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing or unsafe", result.stderr)

    def test_binary_mode_rejects_non_executable_file(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-hardening-test-") as temporary:
            binary = Path(temporary) / "glifistored"
            binary.write_bytes(b"ELF fixture")
            binary.chmod(0o644)

            result = self.run_verifier("--binary", str(binary))

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not executable", result.stderr)

    def test_cli_requires_exactly_one_input_mode(self) -> None:
        missing = self.run_verifier()
        both = self.run_verifier("build/unix-strict", "--binary", "glifistored")

        self.assertEqual(missing.returncode, 2)
        self.assertEqual(both.returncode, 2)


if __name__ == "__main__":
    unittest.main()
