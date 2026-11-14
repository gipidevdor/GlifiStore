from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from engineering.tools.format_code import format_sources, source_files


class CodeFormatTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="glifistore-style-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        (self.root / ".clang-format").write_text("BasedOnStyle: LLVM\n", encoding="utf-8")
        (self.root / ".clang-format-version").write_text("21.1.8\n", encoding="ascii")
        self.formatter = self.root / "fake-clang-format"
        self.write_formatter("21.1.8")

    def write_formatter(self, version: str) -> None:
        self.formatter.write_text(
            "#!/usr/bin/env python3\n"
            "import sys\n"
            "if '--version' in sys.argv:\n"
            f"    print('clang-format version {version}')\n"
            "else:\n"
            "    source = sys.stdin.buffer.read()\n"
            "    if b'FORMAT_ERROR' in source:\n"
            "        sys.stderr.write('incomplete format')\n"
            "        sys.exit(1)\n"
            "    sys.stdout.buffer.write(source.replace(b'int  value', b'int value'))\n",
            encoding="utf-8",
        )
        self.formatter.chmod(0o755)

    def source(self, name: str, content: str = "int  value;\n") -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_discovery_covers_consumers_tools_probes_and_untracked_files(self) -> None:
        names = (
            "include/header with spaces.hpp", "src/kept.cpp", "tests/abi/test.c",
            "tools/utility.cpp", "benchmarks/work.cc", "fuzz/decoder.cpp",
            "scripts/probes/diagnostic.cpp", "packaging/common/consumer/abi.c",
        )
        for name in names:
            self.source(name)
        removed = self.source("src/removed.cpp")
        subprocess.run(["git", "add", "src/kept.cpp", "src/removed.cpp"], cwd=self.root, check=True)
        removed.unlink()
        self.source("src/ignored.cpp")
        self.source("src/notes.txt")
        self.source("build/generated.cpp")
        (self.root / ".gitignore").write_text("src/ignored.cpp\nsrc/kept.cpp\nbuild/\n", encoding="utf-8")

        self.assertEqual(sorted(Path(name) for name in names), source_files(self.root))

    def test_check_is_read_only_and_write_is_idempotent(self) -> None:
        source = self.source("tools/file with spaces.c")
        before = source.read_bytes()

        checked = format_sources(self.root, str(self.formatter), write=False)
        self.assertEqual((Path("tools/file with spaces.c"),), checked.changed)
        self.assertEqual(before, source.read_bytes())

        written = format_sources(self.root, str(self.formatter), write=True)
        self.assertFalse(written.errors)
        self.assertEqual(b"int value;\n", source.read_bytes())
        self.assertFalse(format_sources(self.root, str(self.formatter), write=False).changed)
        self.assertFalse(format_sources(self.root, str(self.formatter), write=True).changed)

    def test_wrong_formatter_version_cannot_edit_sources(self) -> None:
        source = self.source("src/version.cpp")
        self.write_formatter("22.0.0")
        with self.assertRaisesRegex(ValueError, "expected clang-format 21.1.8"):
            format_sources(self.root, str(self.formatter), write=True)
        self.assertEqual("int  value;\n", source.read_text(encoding="utf-8"))

    def test_formatter_failure_preserves_the_original_file(self) -> None:
        source = self.source("src/broken.cpp", "FORMAT_ERROR\n")
        report = format_sources(self.root, str(self.formatter), write=True)
        self.assertEqual(1, len(report.errors))
        self.assertIn("incomplete format", report.errors[0])
        self.assertEqual("FORMAT_ERROR\n", source.read_text(encoding="utf-8"))

    def test_write_refuses_to_replace_a_newer_editor_save(self) -> None:
        source = self.source("src/concurrent.cpp")
        original_run = subprocess.run

        def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess:
            if "input" in kwargs:
                source.write_text("int newer_value;\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, b"int value;\n", b"")
            return original_run(command, **kwargs)

        with patch("engineering.tools.format_code.subprocess.run", side_effect=run):
            report = format_sources(self.root, str(self.formatter), write=True)
        self.assertEqual(1, len(report.errors))
        self.assertIn("changed during formatting", report.errors[0])
        self.assertEqual("int newer_value;\n", source.read_text(encoding="utf-8"))

    def test_symlinks_and_empty_selection_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "no native source files"):
            format_sources(self.root, str(self.formatter), write=False)
        with tempfile.TemporaryDirectory() as outside:
            target = Path(outside) / "outside.cpp"
            target.write_text("int  value;\n", encoding="utf-8")
            (self.root / "src").mkdir()
            (self.root / "src/external.cpp").symlink_to(target)
            with self.assertRaisesRegex(ValueError, "regular repository-owned file"):
                format_sources(self.root, str(self.formatter), write=True)
            self.assertEqual("int  value;\n", target.read_text(encoding="utf-8"))

    def test_new_sources_cannot_turn_a_partial_inventory_into_a_pass(self) -> None:
        self.source("src/first.cpp", "int value;\n")
        original_run = subprocess.run

        def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess:
            if "input" in kwargs:
                self.source("src/new.cpp")
            return original_run(command, **kwargs)

        with patch("engineering.tools.format_code.subprocess.run", side_effect=run):
            report = format_sources(self.root, str(self.formatter), write=False)
        self.assertEqual(1, len(report.errors))
        self.assertIn("inventory changed", report.errors[0])


if __name__ == "__main__":
    unittest.main()
