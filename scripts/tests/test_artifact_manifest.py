from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from engineering.tools.generate_artifact_manifest import (
    ArtifactManifestError,
    build_manifest,
    parse_artifact,
    validate_manifest,
)
from engineering.tools.generate_release_context import build_context
from engineering.tools.package_framework import PackageFrameworkError, digest, encode_json


ROOT = Path(__file__).resolve().parents[2]


class ArtifactManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.context = build_context(ROOT)

    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-artifact-manifest-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.context_path = self.directory / "release-context.json"
        self.context_path.write_text(encode_json(self.context), encoding="utf-8")
        self.source = self.directory / "GlifiStore-0.1.0.tar.xz"
        self.source.write_bytes(b"sealed source archive")
        self.package = self.directory / "glifistore-0.1.0-debian-12-amd64.deb"
        self.package.write_bytes(b"package payload")

    def manifest(self, *artifacts: str) -> dict:
        return build_manifest(
            context_path=self.context_path,
            profile="main",
            parent_source=self.source,
            artifacts=list(artifacts)
            or [
                f"id=deb-debian-12-amd64,kind=deb,platform=debian-12,arch=amd64,"
                f"backend=deb,path={self.package}"
            ],
        )

    def test_identity_binds_each_artifact_to_the_sealed_parent_source(self) -> None:
        manifest = self.manifest()
        self.assertEqual(manifest["parent_source"]["sha256"], digest(self.source))
        entry = manifest["artifacts"][0]
        self.assertEqual(entry["id"], "deb-debian-12-amd64")
        self.assertEqual(entry["kind"], "deb")
        self.assertEqual(entry["backend"], "deb")
        self.assertEqual(entry["platform"], "debian-12")
        self.assertEqual(entry["arch"], "amd64")
        self.assertEqual(entry["sha256"], digest(self.package))
        self.assertEqual(entry["size"], self.package.stat().st_size)
        self.assertEqual(entry["parent_source_sha256"], digest(self.source))
        self.assertEqual(manifest["git_sha"], self.context["git"]["commit"])
        self.assertEqual(manifest["product_version"], self.context["product_version"])

    def test_rebuilt_artifacts_are_caught_by_digest_verification(self) -> None:
        manifest = self.manifest()
        validate_manifest(manifest, artifact_root=self.directory)
        self.package.write_bytes(b"rebuilt payload")
        with self.assertRaisesRegex(ArtifactManifestError, "digest mismatch"):
            validate_manifest(manifest, artifact_root=self.directory)

    def test_an_artifact_from_another_source_is_refused(self) -> None:
        manifest = copy.deepcopy(self.manifest())
        manifest["artifacts"][0]["parent_source_sha256"] = "0" * 64
        with self.assertRaisesRegex(ArtifactManifestError, "not derived from the recorded parent"):
            validate_manifest(manifest)

    def test_duplicate_identities_are_refused(self) -> None:
        second = self.directory / "glifistore-0.1.0-fedora-43-amd64.rpm"
        second.write_bytes(b"rpm payload")
        with self.assertRaisesRegex(ArtifactManifestError, "duplicates an artifact id"):
            self.manifest(
                f"id=package,kind=deb,platform=debian-12,arch=amd64,backend=deb,path={self.package}",
                f"id=package,kind=rpm,platform=fedora-43,arch=amd64,backend=rpm,path={second}",
            )

    def test_a_manifest_for_another_release_is_refused(self) -> None:
        manifest = copy.deepcopy(self.manifest())
        manifest["git_sha"] = "0" * 40
        with self.assertRaisesRegex(ArtifactManifestError, "different commit"):
            validate_manifest(manifest, context=self.context)

    def test_an_empty_manifest_proves_nothing(self) -> None:
        with self.assertRaisesRegex(ArtifactManifestError, "proves nothing"):
            build_manifest(
                context_path=self.context_path,
                profile="main",
                parent_source=self.source,
                artifacts=[],
            )

    def test_unsupported_kinds_and_fields_are_refused(self) -> None:
        with self.assertRaisesRegex(ArtifactManifestError, "unsupported artifact field"):
            parse_artifact("id=x,kind=deb,platform=p,arch=amd64,path=/tmp/x,vendor=acme")
        with self.assertRaisesRegex(ArtifactManifestError, "misses required fields"):
            parse_artifact("id=x,kind=deb,platform=p")
        with self.assertRaisesRegex(ArtifactManifestError, "unsupported artifact kind"):
            self.manifest(
                f"id=x,kind=msi,platform=debian-12,arch=amd64,path={self.package}"
            )

    def test_an_unknown_architecture_violates_the_schema(self) -> None:
        with self.assertRaisesRegex(PackageFrameworkError, "artifact-manifest.schema.json"):
            self.manifest(
                f"id=x,kind=deb,platform=debian-12,arch=i386,backend=deb,path={self.package}"
            )

    def test_the_command_line_creates_and_revalidates_a_manifest(self) -> None:
        output = self.directory / "artifact-manifest.json"
        created = subprocess.run(
            [
                sys.executable,
                str(ROOT / "engineering/tools/generate_artifact_manifest.py"),
                "create",
                "--release-context",
                str(self.context_path),
                "--profile",
                "main",
                "--parent-source",
                str(self.source),
                "--artifact",
                f"id=deb-debian-12-amd64,kind=deb,platform=debian-12,arch=amd64,"
                f"backend=deb,path={self.package}",
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(created.returncode, 0, created.stderr)
        self.assertEqual(json.loads(output.read_text())["schema_version"], 1)

        validated = subprocess.run(
            [
                sys.executable,
                str(ROOT / "engineering/tools/generate_artifact_manifest.py"),
                "validate",
                "--manifest",
                str(output),
                "--artifact-root",
                str(self.directory),
                "--release-context",
                str(self.context_path),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(validated.returncode, 0, validated.stderr)


if __name__ == "__main__":
    unittest.main()
