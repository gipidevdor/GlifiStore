from __future__ import annotations

import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from engineering.tools.run_clang_tidy_gate import clang_tidy_environment, production_sources, run_gate


class ClangTidyGateTests(unittest.TestCase):
    @staticmethod
    def write_database(root: Path) -> Path:
        build = root / "build"
        build.mkdir()
        entries = [
            {"directory": str(root), "file": str(root / "src/good.cpp"), "command": "c++ good.cpp"},
            {"directory": str(root), "file": str(root / "src/bad.cpp"), "command": "c++ bad.cpp"},
            {"directory": str(root), "file": str(root / "tests/test.cpp"), "command": "c++ test.cpp"},
            {"directory": str(root), "file": str(root / "src/good.cpp"), "command": "c++ good.cpp"},
        ]
        (build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")
        return build

    def test_selects_unique_compiled_production_sources(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "tests").mkdir()
            for name in ("src/good.cpp", "src/bad.cpp", "tests/test.cpp"):
                (root / name).write_text("", encoding="utf-8")
            build = self.write_database(root)

            sources = production_sources(root, build)

            self.assertEqual(
                [(root / "src/bad.cpp").resolve(), (root / "src/good.cpp").resolve()],
                sources,
            )

    def test_reports_only_failed_clang_tidy_invocations(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "tests").mkdir()
            for name in ("src/good.cpp", "src/bad.cpp", "tests/test.cpp"):
                (root / name).write_text("", encoding="utf-8")
            build = self.write_database(root)

            def fake_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                failed = command[-1].endswith("bad.cpp")
                return subprocess.CompletedProcess(
                    command,
                    int(failed),
                    "",
                    "high-signal failure" if failed else "",
                )

            with patch("engineering.tools.run_clang_tidy_gate.subprocess.run", side_effect=fake_run):
                report = run_gate(root, build, "clang-tidy", jobs=1)

            self.assertEqual(2, report.sources)
            self.assertEqual(1, len(report.failures))
            self.assertEqual(Path("src/bad.cpp"), report.failures[0].source)
            self.assertIn("high-signal failure", report.failures[0].output)

    def test_macos_environment_discovers_sdk_root(self) -> None:
        completed = subprocess.CompletedProcess(["xcrun"], 0, "/sdk\n", "")
        with (
            patch("engineering.tools.run_clang_tidy_gate.platform.system", return_value="Darwin"),
            patch("engineering.tools.run_clang_tidy_gate.os.environ", {}),
            patch("engineering.tools.run_clang_tidy_gate.shutil.which", return_value="/usr/bin/xcrun"),
            patch("engineering.tools.run_clang_tidy_gate.subprocess.run", return_value=completed),
        ):
            self.assertEqual("/sdk", clang_tidy_environment().get("SDKROOT"))


if __name__ == "__main__":
    unittest.main()
