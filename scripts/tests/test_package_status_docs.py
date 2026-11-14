from __future__ import annotations

import copy
import tempfile
import unittest
from pathlib import Path

import yaml

from engineering.tools.generate_package_matrix import load_matrix
from engineering.tools.generate_package_status import STATUS_PATH, main, render


ROOT = Path(__file__).resolve().parents[2]
MATRIX = ROOT / "engineering/distribution/package-matrix.yaml"


class GeneratedStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.matrix = load_matrix(MATRIX)
        cls.rendered = render(cls.matrix)

    def test_the_committed_status_document_matches_the_matrix(self) -> None:
        self.assertEqual(main([]), 0)
        self.assertEqual(STATUS_PATH.read_text(encoding="utf-8"), self.rendered)

    def test_the_document_says_it_is_generated_and_is_not_evidence(self) -> None:
        self.assertIn("GENERATED FILE. Do not edit by hand.", self.rendered)
        self.assertIn("it is never", self.rendered)
        self.assertIn("architectural prototype", self.rendered)

    def test_every_backend_target_and_check_is_rendered(self) -> None:
        for entry in self.matrix["backends"]:
            self.assertIn(f"`{entry['id']}`", self.rendered)
            self.assertIn(entry["display_name"], self.rendered)
            for limitation in entry["limitations"]:
                self.assertIn(" ".join(limitation.split())[:60], self.rendered)
            for target in entry["targets"]:
                self.assertIn(target["id"], self.rendered)
        for check in self.matrix["lifecycle_checks"]:
            self.assertIn(f"`{check['id']}`", self.rendered)

    def test_out_of_scope_targets_are_rendered_as_out_of_scope(self) -> None:
        section = self.rendered.split("## Out of scope", 1)[1]
        for identifier in ("apple-pkg", "msi", "windows"):
            self.assertIn(f"`{identifier}`", section)
        # Windows and Apple .pkg may only ever appear as refusals.
        for line in self.rendered.splitlines():
            lowered = line.lower()
            if "windows" in lowered or "apple" in lowered:
                self.assertNotIn("| yes |", line)

    def test_a_stale_document_fails_the_default_run(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "package-status.md"
            self.assertEqual(main(["--output", str(output)]), 1)
            output.write_text("stale\n", encoding="utf-8")
            self.assertEqual(main(["--output", str(output)]), 1)
            self.assertEqual(main(["--output", str(output), "--write-generated"]), 0)
            self.assertEqual(main(["--output", str(output)]), 0)

    def test_a_matrix_that_does_not_validate_is_refused(self) -> None:
        broken = copy.deepcopy(yaml.safe_load(MATRIX.read_text(encoding="utf-8")))
        broken["backends"][0]["status"] = "IMPLEMENTED"
        broken["backends"][0]["required_for_release"] = True
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "package-matrix.yaml"
            path.write_text(yaml.safe_dump(broken), encoding="utf-8")
            output = Path(directory) / "package-status.md"
            self.assertEqual(
                main(["--matrix", str(path), "--output", str(output), "--write-generated"]), 1
            )
            self.assertFalse(output.exists())

    def test_rendering_follows_the_matrix_instead_of_the_committed_page(self) -> None:
        promoted = copy.deepcopy(self.matrix)
        entry = next(item for item in promoted["backends"] if item["id"] == "deb")
        entry["lifecycle_state"] = "INSTALLED"
        rendered = render(promoted)
        self.assertNotEqual(rendered, self.rendered)
        self.assertIn("| `deb` | `deb` | `STRUCTURAL` | `INSTALLED` |", rendered)


if __name__ == "__main__":
    unittest.main()
