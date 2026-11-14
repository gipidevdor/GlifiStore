from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

from engineering.tools.benchmark_subject_digest import subject_digest, tracked_subject_paths


class BenchmarkSubjectDigestTests(unittest.TestCase):
    def test_tracked_scope_excludes_retained_benchmark_results(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / ".github" / "workflows").mkdir(parents=True)
            (root / "benchmarks" / "results").mkdir(parents=True)
            (root / "docs").mkdir()
            (root / "engineering" / "tools").mkdir(parents=True)
            (root / "scripts").mkdir()
            (root / "src").mkdir()
            (root / ".github" / "workflows" / "benchmarks.yml").write_text(
                "name: benchmark\n", encoding="utf-8"
            )
            (root / "CMakeLists.txt").write_text("project(test)\n", encoding="utf-8")
            (root / "src" / "engine.cpp").write_text("engine\n", encoding="utf-8")
            (root / "benchmarks" / "bench.cpp").write_text("bench\n", encoding="utf-8")
            (root / "benchmarks" / "results" / "old.txt").write_text(
                "result\n", encoding="utf-8"
            )
            (root / "docs" / "benchmark.md").write_text("docs\n", encoding="utf-8")
            (root / "engineering" / "tools" / "benchmark_subject_digest.py").write_text(
                "tool\n", encoding="utf-8"
            )
            (root / "scripts" / "benchmark_report.py").write_text(
                "report\n", encoding="utf-8"
            )
            subprocess.run(["git", "init", "-q", root], check=True)
            subprocess.run(["git", "-C", root, "add", "."], check=True)

            self.assertEqual(
                tracked_subject_paths(root),
                [
                    ".github/workflows/benchmarks.yml",
                    "CMakeLists.txt",
                    "benchmarks/bench.cpp",
                    "src/engine.cpp",
                ],
            )

    def test_digest_is_order_independent_and_content_sensitive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "src" / "a.cpp").write_text("first\n", encoding="utf-8")
            (root / "src" / "b.cpp").write_text("second\n", encoding="utf-8")

            forward = subject_digest(root, ["src/a.cpp", "src/b.cpp"])
            reverse = subject_digest(root, ["src/b.cpp", "src/a.cpp"])
            self.assertEqual(forward, reverse)
            self.assertEqual(len(forward), 64)

            (root / "src" / "a.cpp").write_text("changed\n", encoding="utf-8")
            self.assertNotEqual(
                subject_digest(root, ["src/a.cpp", "src/b.cpp"]), forward
            )

    def test_path_and_executable_mode_are_part_of_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first"
            second = root / "second"
            first.write_bytes(b"same")
            second.write_bytes(b"same")
            first_digest = subject_digest(root, ["first"])
            self.assertNotEqual(subject_digest(root, ["second"]), first_digest)

            first.chmod(0o755)
            self.assertNotEqual(subject_digest(root, ["first"]), first_digest)

    def test_rejects_empty_escape_and_symlink_subjects(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "target"
            target.write_bytes(b"target")
            (root / "link").symlink_to(target)

            with self.assertRaisesRegex(ValueError, "contains no files"):
                subject_digest(root, [])
            with self.assertRaisesRegex(ValueError, "duplicate benchmark subject path"):
                subject_digest(root, ["target", "./target"])
            with self.assertRaisesRegex(ValueError, "invalid benchmark subject path"):
                subject_digest(root, ["../target"])
            with self.assertRaisesRegex(ValueError, "not a regular file"):
                subject_digest(root, ["link"])


if __name__ == "__main__":
    unittest.main()
