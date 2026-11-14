from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class DevVerifyTests(unittest.TestCase):
    def test_shell_syntax_error_stops_verification_before_python_checks(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-verify-") as temporary:
            root = Path(temporary)
            for directory in ("include", "src", "tests", "benchmarks", "fuzz", "scripts", "bin"):
                (root / directory).mkdir()
            shutil.copyfile(ROOT / "scripts/dev.sh", root / "scripts/dev.sh")
            (root / "scripts/invalid script.sh").write_text("if then\n", encoding="utf-8")
            formatter = root / "bin/clang-format"
            formatter.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            formatter.chmod(0o755)
            marker = root / "python-was-run"
            python = root / "bin/python"
            python.write_text(f'#!/bin/sh\ntouch "{marker}"\n', encoding="utf-8")
            python.chmod(0o755)
            environment = os.environ.copy()
            environment["PATH"] = f"{root / 'bin'}{os.pathsep}{environment['PATH']}"
            environment["PYTHON"] = str(python)

            result = subprocess.run(
                ["bash", str(root / "scripts/dev.sh"), "verify"],
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("invalid script.sh", result.stderr)
            self.assertFalse(marker.exists(), result.stdout + result.stderr)


class DevCommandLineTests(unittest.TestCase):
    def run_dev(self, *arguments: str, family: str | None = None) -> subprocess.CompletedProcess:
        environment = os.environ.copy()
        environment.pop("GLIFISTORE_PRESET_FAMILY", None)
        if family is not None:
            environment["GLIFISTORE_PRESET_FAMILY"] = family
        return subprocess.run(
            ["bash", str(ROOT / "scripts/dev.sh"), *arguments],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_help_lists_everyday_commands(self) -> None:
        result = self.run_dev("help")
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in ("doctor", "build", "test", "verify", "format-check", "benchmark"):
            self.assertIn(f"  {command}", result.stdout)

    def test_unknown_command_fails_with_usage_error(self) -> None:
        result = self.run_dev("no-such-command")
        self.assertEqual(result.returncode, 2)
        self.assertIn("unknown command 'no-such-command'", result.stderr)

    def test_preset_family_override_is_validated(self) -> None:
        self.assertIn("presets: unix-*", self.run_dev("help", family="unix").stdout)
        result = self.run_dev("help", family="windows")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GLIFISTORE_PRESET_FAMILY", result.stderr)


if __name__ == "__main__":
    unittest.main()
