from __future__ import annotations

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

from scripts import build_docs


class DocumentationBuildTests(unittest.TestCase):
    def test_stages_nested_guides_and_commit_links_without_modifying_sources(
        self,
    ) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            site = root / "docs/doxygen"
            site.mkdir(parents=True)
            (root / "docs/reference").mkdir()
            guide = root / "docs/reference/api.md"
            guide.write_text("# API\n", encoding="utf-8")
            (root / "engineering").mkdir()
            (root / "engineering/requirements.yaml").touch()
            (root / "include").mkdir()
            header = root / "include/api.hpp"
            header.touch()
            original = (
                "# Home\n"
                "[guide](../reference/api.md#read)\n"
                "[contract](../../engineering/requirements.yaml)\n"
                "[directory](../../engineering/)\n"
                "[header](../../include/api.hpp)\n"
                "[ref]: ../reference/api.md\n"
                "[external](https://example.com/)\n"
                "```markdown\n[example](missing.md)\n```\n"
            )
            home = site / "index.md"
            home.write_text(original, encoding="utf-8")
            destination = root / "generated"
            with (
                patch.object(build_docs, "ROOT", root),
                patch.object(build_docs, "SITE", site),
            ):
                build_docs.stage_guides(
                    destination, "https://github.com/owner/repo", "abc123"
                )
            staged = (destination / "glifistore.md").read_text(encoding="utf-8")
            self.assertIn(f"{destination}/docs/reference/api.md#read", staged)
            self.assertIn(
                "https://github.com/owner/repo/blob/abc123/engineering/requirements.yaml",
                staged,
            )
            self.assertIn(
                "https://github.com/owner/repo/tree/abc123/engineering", staged
            )
            self.assertIn(header.as_posix(), staged)
            self.assertIn(f"[ref]: <{destination}/docs/reference/api.md>", staged)
            self.assertIn("[external](https://example.com/)", staged)
            self.assertIn("[example](missing.md)", staged)
            self.assertEqual(original, home.read_text(encoding="utf-8"))

    def test_validates_pages_assets_and_cross_page_fragments(self) -> None:
        with TemporaryDirectory() as directory:
            html = Path(directory).resolve()
            (html / "asset.css").touch()
            (html / "api.html").write_text('<h2 id="read">Read</h2>', encoding="utf-8")
            home = html / "index.html"
            home.write_text(
                '<link href="asset.css"><a href="api.html#read">API</a>'
                '<a href="https://example.com/">External</a><a id="here" href="#here">Here</a>',
                encoding="utf-8",
            )
            build_docs.validate_site(html)
            for target in ("missing.html", "api.html#absent", "../outside.css"):
                with self.subTest(target=target):
                    home.write_text(f'<a href="{target}">Broken</a>', encoding="utf-8")
                    with self.assertRaisesRegex(RuntimeError, "Broken generated links"):
                        build_docs.validate_site(html)


if __name__ == "__main__":
    unittest.main()
