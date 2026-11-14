from __future__ import annotations

from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from engineering.tools.validate_documentation import validate_documentation


class DocumentationValidationTests(unittest.TestCase):
    def test_accepts_local_links_references_images_and_encoded_paths(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "docs/target.md").write_text("# Target\n", encoding="utf-8")
            (root / "docs/hello world.md").write_text("# Encoded\n", encoding="utf-8")
            (root / "image.png").write_bytes(b"image")
            (root / "README.md").write_text(
                "[inline](docs/target.md#section)\n"
                "[encoded](<docs/hello%20world.md>)\n"
                "![image](image.png)\n"
                "[reference][target]\n"
                "[target]: docs/target.md\n",
                encoding="utf-8",
            )

            report = validate_documentation(root, [Path("README.md"), Path("docs/target.md")])

            self.assertEqual(4, report.local_links)
            self.assertEqual((), report.issues)

    def test_ignores_external_anchor_only_and_fenced_example_links(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "README.md").write_text(
                "[external](https://example.com/missing)\n"
                "[anchor](#local-heading)\n"
                "```markdown\n[example](missing.md)\n```\n",
                encoding="utf-8",
            )

            report = validate_documentation(root, [Path("README.md")])

            self.assertEqual(0, report.local_links)
            self.assertEqual((), report.issues)

    def test_rejects_missing_outside_and_wrong_case_targets(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Target.md").write_text("# Target\n", encoding="utf-8")
            (root / "README.md").write_text(
                "[missing](missing.md)\n"
                "[outside](../../outside.md)\n"
                "[case](target.md)\n",
                encoding="utf-8",
            )

            report = validate_documentation(root, [Path("README.md")])

            self.assertEqual(3, report.local_links)
            self.assertEqual(
                ["target does not exist", "target escapes repository", "path case does not match"],
                [issue.reason for issue in report.issues],
            )

    def test_reports_missing_tracked_markdown_and_invalid_utf8(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "invalid.md").write_bytes(b"\xff")

            report = validate_documentation(root, [Path("missing.md"), Path("invalid.md")])

            self.assertEqual(
                ["tracked file is missing", "not valid UTF-8"],
                [issue.reason for issue in report.issues],
            )


if __name__ == "__main__":
    unittest.main()
