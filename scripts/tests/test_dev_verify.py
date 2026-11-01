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


if __name__ == "__main__":
    unittest.main()
