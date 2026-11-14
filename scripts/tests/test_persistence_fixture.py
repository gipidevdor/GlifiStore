from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "engineering/tools/persistence_fixture.py"


class PersistenceFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        (self.source / "manifest.glifi").write_bytes(b"manifest-v1")
        (self.source / "segment-0123456789abcdef-00000001.glifi").write_bytes(b"segment-v1")
        self.repository = self.root / "repository"
        self.repository.mkdir()
        subprocess.run(["git", "init", "-q", str(self.repository)], check=True)
        subprocess.run(
            ["git", "-C", str(self.repository), "config", "user.name", "Fixture Test"],
            check=True,
        )
        self.producer_artifact = self.root / "glifistore-release.tar.xz"
        self.producer_artifact.write_bytes(b"tagged release artifact")
        subprocess.run(
            ["git", "-C", str(self.repository), "config", "user.email", "fixture@example.invalid"],
            check=True,
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def run_tool(self, *arguments: str, success: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [sys.executable, str(TOOL), *arguments],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if success and result.returncode != 0:
            self.fail(f"tool failed: {result.stderr}")
        if not success and result.returncode == 0:
            self.fail(f"tool unexpectedly succeeded: {result.stdout}")
        return result

    def create(self, version: str, *, source: Path | None = None) -> Path:
        version_file = self.repository / "VERSION"
        version_file.write_text(version + "\n", encoding="utf-8")
        subprocess.run(
            ["git", "-C", str(self.repository), "add", "VERSION"], check=True
        )
        subprocess.run(
            ["git", "-C", str(self.repository), "commit", "-q", "-m", f"release {version}"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(self.repository), "tag", "-a", f"v{version}", "-m", f"v{version}"],
            check=True,
        )
        git_sha = subprocess.run(
            ["git", "-C", str(self.repository), "rev-parse", "HEAD"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        ).stdout.strip()
        output = self.root / "released" / version
        self.run_tool(
            "create",
            "--source",
            str(source or self.source),
            "--output",
            str(output),
            "--product-version",
            version,
            "--tag",
            f"v{version}",
            "--git-sha",
            git_sha,
            "--producer-artifact",
            str(self.producer_artifact),
            "--worker-count",
            "1",
            "--key-hex",
            "6b6579",
            "--value-hex",
            "76616c7565",
            "--packaged-at",
            "2026-08-26T12:00:00Z",
            "--repository",
            str(self.repository),
        )
        return output

    def test_create_produces_closed_valid_inventory(self) -> None:
        fixture = self.create("0.1.0")
        self.run_tool(
            "validate",
            str(fixture),
            "--before-version",
            "0.1.1",
            "--repository",
            str(self.repository),
        )
        metadata = json.loads((fixture / "STORE-FIXTURE.json").read_text(encoding="utf-8"))
        self.assertEqual(metadata["baseline_kind"], "tagged-release")
        self.assertEqual(metadata["product_version"], "0.1.0")
        recorded = {}
        for line in (fixture / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
            digest, name = line.split("  ", 1)
            recorded[name] = digest
        self.assertEqual(
            set(recorded),
            {
                "STORE-FIXTURE.json",
                "store/manifest.glifi",
                "store/segment-0123456789abcdef-00000001.glifi",
            },
        )
        for name, expected in recorded.items():
            self.assertEqual(hashlib.sha256((fixture / name).read_bytes()).hexdigest(), expected)

    def test_validation_rejects_same_version_tampering_and_extra_files(self) -> None:
        fixture = self.create("0.1.0")
        result = self.run_tool(
            "validate",
            str(fixture),
            "--before-version",
            "0.1.0",
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("not older", result.stderr)

        (fixture / "store/manifest.glifi").write_bytes(b"tampered")
        result = self.run_tool(
            "validate",
            str(fixture),
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("checksum mismatch", result.stderr)

        fixture = self.create("0.1.1")
        (fixture / "store/unrecorded").write_bytes(b"unexpected")
        result = self.run_tool(
            "validate",
            str(fixture),
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("checksum inventory differs", result.stderr)

    def test_validation_rejects_symlink_members(self) -> None:
        fixture = self.create("0.1.0")
        (fixture / "store/link").symlink_to("manifest.glifi")
        result = self.run_tool(
            "validate",
            str(fixture),
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("not a regular file", result.stderr)

    def test_validation_binds_metadata_to_annotated_tag_commit(self) -> None:
        fixture = self.create("0.1.0")
        metadata_path = fixture / "STORE-FIXTURE.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["git_sha"] = "3" * 40
        metadata_path.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        result = self.run_tool(
            "validate",
            str(fixture),
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("does not match annotated tag", result.stderr)

    def test_select_chooses_newest_strictly_older_valid_fixture(self) -> None:
        self.create("0.1.0")
        self.create("0.1.1")
        result = self.run_tool(
            "select",
            "--directory",
            str(self.root / "released"),
            "--before-version",
            "0.1.2",
            "--repository",
            str(self.repository),
        )
        self.assertEqual(Path(result.stdout.strip()).name, "0.1.1")

    def test_select_fails_closed_when_a_versioned_fixture_is_invalid(self) -> None:
        self.create("0.1.0")
        invalid = self.create("0.1.1")
        (invalid / "STORE-FIXTURE.json").write_text("{}\n", encoding="utf-8")
        result = self.run_tool(
            "select",
            "--directory",
            str(self.root / "released"),
            "--before-version",
            "0.1.2",
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("invalid released Store fixture", result.stderr)

    def test_create_rejects_source_symlink_and_existing_output(self) -> None:
        source_link = self.root / "source-link"
        source_link.symlink_to(self.source, target_is_directory=True)
        output = self.root / "released" / "0.1.0"
        result = self.run_tool(
            "create",
            "--source",
            str(source_link),
            "--output",
            str(output),
            "--product-version",
            "0.1.0",
            "--tag",
            "v0.1.0",
            "--git-sha",
            "1" * 40,
            "--producer-artifact",
            str(self.producer_artifact),
            "--worker-count",
            "1",
            "--key-hex",
            "00",
            "--value-hex",
            "00",
            "--packaged-at",
            "2026-08-26T12:00:00Z",
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("source Store must be a regular directory", result.stderr)

        self.create("0.1.0")
        result = self.run_tool(
            "create",
            "--source",
            str(self.source),
            "--output",
            str(output),
            "--product-version",
            "0.1.0",
            "--tag",
            "v0.1.0",
            "--git-sha",
            "1" * 40,
            "--producer-artifact",
            str(self.producer_artifact),
            "--worker-count",
            "1",
            "--key-hex",
            "00",
            "--value-hex",
            "00",
            "--packaged-at",
            "2026-08-26T12:00:00Z",
            "--repository",
            str(self.repository),
            success=False,
        )
        self.assertIn("output already exists", result.stderr)


if __name__ == "__main__":
    unittest.main()
