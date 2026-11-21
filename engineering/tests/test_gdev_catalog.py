"""The derived GDev view must never become a second assurance authority."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[1] / "tools/export_gdev_catalog.py"
SPEC = importlib.util.spec_from_file_location("gdev_catalog", MODULE)
catalog = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(catalog)


class GDevCatalogTests(unittest.TestCase):
    def test_real_catalog_is_deterministic_and_keeps_states_and_links(self):
        first = catalog.render()
        self.assertEqual(first, catalog.render())
        data = json.loads(first)
        row = next(r for r in data["records"] if r["id"] == "GATE-COMPACTION-DELTA")
        self.assertEqual("IMPLEMENTATA", row["state"])
        self.assertEqual(["GS-PERSIST-DELTA-001"], row["data"]["requisiti"])
        self.assertTrue(row["data"]["evidenze"])

    def fixture(self, root):
        for path in catalog.DIRECTORIES:
            folder = root / path
            folder.mkdir(parents=True)
            if folder.name != "schemas":
                (folder / "empty.yaml").write_text(f"{folder.name}: []\n")
        script = root / catalog.FILES[0]
        script.parent.mkdir(parents=True)
        script.write_text("# exporter\n")

    def test_add_change_delete_are_visible_in_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            initial = catalog.build_catalog(root)
            path = root / "engineering/gates/new.yaml"
            path.write_text("gates: [{id: GATE-NEW, stato: NON_IMPLEMENTATA}]\n")
            added = catalog.build_catalog(root)
            self.assertNotEqual(initial["inputs"], added["inputs"])
            path.write_text("gates: [{id: GATE-NEW, stato: IMPLEMENTATA}]\n")
            self.assertNotEqual(added["inputs"], catalog.build_catalog(root)["inputs"])
            path.unlink()
            self.assertEqual(initial, catalog.build_catalog(root))

    def test_duplicate_and_external_source_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root)
            path = root / "engineering/gates/bad.yaml"
            path.write_text("gates: [{id: GATE-X}, {id: GATE-X}]\n")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                catalog.build_catalog(root)
            path.unlink()
            path.symlink_to(MODULE)
            with self.assertRaisesRegex(ValueError, "External"):
                catalog.build_catalog(root)


if __name__ == "__main__":
    unittest.main()
