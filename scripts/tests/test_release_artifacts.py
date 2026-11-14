from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "engineering" / "tools"
sys.path.insert(0, str(TOOLS))

from release_bundle import (  # noqa: E402
    BundleError,
    bind_sbom,
    seal,
    validate_release_manifest,
    validate_release_policy,
    validate_build_metadata,
    validate_candidate_admission,
    validate_evidence_import,
    validate_sbom,
    verify_checksums,
    verify_seal,
    write_candidate_admission,
    write_checksums,
    write_evidence_import,
    write_release_manifest,
    write_verification,
)
from release_identity import ReleaseIdentityError, validate_release_identity  # noqa: E402
from prior_release import PriorReleaseError, select as select_prior_release  # noqa: E402
from abi_consumer_fixture import (  # noqa: E402
    ConsumerFixtureError,
    validate as validate_abi_consumer_fixture,
)
from wire_client_fixture import (  # noqa: E402
    WireClientFixtureError,
    validate as validate_wire_client_fixture,
)
from compare_release_rebuild import (  # noqa: E402
    RebuildComparisonError,
    compare as compare_release_rebuild,
)
from release_evidence import (  # noqa: E402
    EvidenceError,
    REQUIRED_CHECKS,
    create_evidence,
    validate_evidence,
)


def load_source_packager():
    path = ROOT / "scripts" / "package-source-release.py"
    spec = importlib.util.spec_from_file_location("package_source_release", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SOURCE_PACKAGER = load_source_packager()


def load_prefix_packager():
    path = ROOT / "scripts" / "package-install-prefix.py"
    spec = importlib.util.spec_from_file_location("package_install_prefix", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PREFIX_PACKAGER = load_prefix_packager()


def load_abi_consumer_packager():
    path = ROOT / "scripts" / "package-abi-consumer.py"
    spec = importlib.util.spec_from_file_location("package_abi_consumer", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ABI_CONSUMER_PACKAGER = load_abi_consumer_packager()


def load_wire_client_packager():
    path = ROOT / "scripts" / "package-wire-client.py"
    spec = importlib.util.spec_from_file_location("package_wire_client", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


WIRE_CLIENT_PACKAGER = load_wire_client_packager()


def run_git(root: Path, *arguments: str, env: dict[str, str] | None = None) -> None:
    subprocess.run(["git", "-C", str(root), *arguments], check=True, env=env,
                   stdout=subprocess.DEVNULL)


class ReleaseIdentityTests(unittest.TestCase):
    def make_repository(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-release-test-")
        root = Path(temporary.name)
        (root / "include/glifistore/abi").mkdir(parents=True)
        (root / "include/glifistore/server").mkdir(parents=True)
        files = {
            "VERSION": "0.1.0\n",
            "ABI_VERSION": "1.0\n",
            "CMakeLists.txt": "cmake_minimum_required(VERSION 3.25)\n",
            "LICENSE": "test license\n",
            "NOTICE": "test notice\n",
            "THIRD_PARTY_NOTICES.md": "none\n",
            "include/glifistore/abi/glifistore.h": "#pragma once\n",
            "include/glifistore/server/protocol.hpp": (
                "inline constexpr unsigned short kProtocolVersion = 2;\n"
            ),
        }
        for name, contents in files.items():
            (root / name).write_text(contents, encoding="utf-8")
        run_git(root, "init", "-q")
        run_git(root, "config", "user.name", "Release Test")
        run_git(root, "config", "user.email", "release-test@example.invalid")
        run_git(root, "add", ".")
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_DATE": "2026-01-01T00:00:00Z",
                "GIT_COMMITTER_DATE": "2026-01-01T00:00:00Z",
            }
        )
        run_git(root, "commit", "-q", "-m", "release source", env=environment)
        run_git(root, "tag", "-a", "v0.1.0", "-m", "v0.1.0", env=environment)
        return temporary, root

    def test_annotated_exact_tag_builds_identical_archives(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        identity = validate_release_identity(root, "v0.1.0")
        self.assertEqual(identity.product_version, "0.1.0")
        first = SOURCE_PACKAGER.build_archive(root, "v0.1.0", root / "out-a")
        second = SOURCE_PACKAGER.build_archive(root, "v0.1.0", root / "out-b")
        self.assertEqual(SOURCE_PACKAGER.sha256(first), SOURCE_PACKAGER.sha256(second))

    def test_version_mismatch_fails_closed(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        with self.assertRaisesRegex(ReleaseIdentityError, "tag/version mismatch"):
            validate_release_identity(root, "v0.2.0")

    def test_lightweight_tag_is_rejected(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        run_git(root, "tag", "v0.1.1")
        (root / "VERSION").write_text("0.1.1\n", encoding="utf-8")
        run_git(root, "add", "VERSION")
        run_git(root, "commit", "-q", "-m", "next")
        run_git(root, "tag", "-f", "v0.1.1")
        with self.assertRaisesRegex(ReleaseIdentityError, "annotated tag"):
            validate_release_identity(root, "v0.1.1")

    def test_tracked_dirty_tree_is_rejected(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        (root / "NOTICE").write_text("modified\n", encoding="utf-8")
        with self.assertRaisesRegex(ReleaseIdentityError, "tracked working tree"):
            validate_release_identity(root, "v0.1.0")

    def test_installed_prefix_archive_is_deterministic_and_requires_versioned_abi(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        prefix = root / "prefix"
        for directory in (
            prefix / "bin",
            prefix / "include/glifistore/abi",
            prefix / "share/GlifiStore",
            prefix / "lib",
        ):
            directory.mkdir(parents=True, exist_ok=True)
        (prefix / "bin/glifistored").write_bytes(b"binary")
        (prefix / "bin/glifistored").chmod(0o755)
        (prefix / "include/glifistore/abi/glifistore.h").write_text("#pragma once\n")
        (prefix / "share/GlifiStore/VERSION").write_text("0.1.0\n")
        (prefix / "share/GlifiStore/ABI_VERSION").write_text("1.0\n")
        (prefix / "lib/libglifistore.so.1.0").write_bytes(b"shared")
        (prefix / "lib/libglifistore.so.1").symlink_to("libglifistore.so.1.0")

        first = PREFIX_PACKAGER.package_prefix(
            root, "v0.1.0", prefix, root / "binary-a", "linux", "x86_64"
        )
        second = PREFIX_PACKAGER.package_prefix(
            root, "v0.1.0", prefix, root / "binary-b", "linux", "x86_64"
        )
        self.assertEqual(SOURCE_PACKAGER.sha256(first), SOURCE_PACKAGER.sha256(second))
        (prefix / "lib/libglifistore.so.1.0").unlink()
        (prefix / "lib/libglifistore.so.1").unlink()
        (prefix / "lib/libglifistore.so").write_bytes(b"unversioned linker input")
        with self.assertRaisesRegex(ReleaseIdentityError, "versioned shared C ABI"):
            PREFIX_PACKAGER.package_prefix(
                root, "v0.1.0", prefix, root / "binary-c", "linux", "x86_64"
            )

    def test_compiled_abi_consumer_archive_is_deterministic_and_identity_bound(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        consumer = root / "abi-consumer"
        consumer.write_bytes(b"compiled ABI consumer")
        consumer.chmod(0o755)
        first = ABI_CONSUMER_PACKAGER.package(
            root, "v0.1.0", consumer, "linux", "x86_64", root / "consumer-a"
        )
        second = ABI_CONSUMER_PACKAGER.package(
            root, "v0.1.0", consumer, "linux", "x86_64", root / "consumer-b"
        )
        self.assertEqual(SOURCE_PACKAGER.sha256(first), SOURCE_PACKAGER.sha256(second))
        import tarfile

        with tarfile.open(first, "r:xz") as archive:
            metadata_member = next(
                member for member in archive.getmembers() if member.name.endswith("ABI-CONSUMER.json")
            )
            metadata_file = archive.extractfile(metadata_member)
            self.assertIsNotNone(metadata_file)
            metadata = json.loads(metadata_file.read())
        self.assertEqual(metadata["git_sha"], validate_release_identity(root, "v0.1.0").git_sha)
        self.assertEqual(metadata["consumer_sha256"], SOURCE_PACKAGER.sha256(consumer))
        extracted = root / "extracted-abi-consumer"
        validate_abi_consumer_fixture(
            first,
            "0.1.0",
            metadata["git_sha"],
            1,
            extracted,
        )
        self.assertEqual(extracted.read_bytes(), consumer.read_bytes())
        self.assertTrue(extracted.stat().st_mode & 0o111)
        with self.assertRaisesRegex(ConsumerFixtureError, "metadata disagrees"):
            validate_abi_consumer_fixture(
                first,
                "0.1.0",
                "f" * 40,
                1,
                root / "wrong-identity-consumer",
            )
        with self.assertRaisesRegex(ConsumerFixtureError, "already exists"):
            validate_abi_consumer_fixture(
                first,
                "0.1.0",
                metadata["git_sha"],
                1,
                extracted,
            )

    def test_prior_release_selection_requires_annotated_older_matching_abi(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        self.assertEqual(select_prior_release(root, "0.2.0", 1), "v0.1.0")
        with self.assertRaisesRegex(PriorReleaseError, "no annotated prior release"):
            select_prior_release(root, "0.1.0", 1)
        with self.assertRaisesRegex(PriorReleaseError, "no annotated prior release"):
            select_prior_release(root, "0.2.0", 2)

    def test_compiled_wire_client_archive_is_deterministic_and_identity_bound(self) -> None:
        temporary, root = self.make_repository()
        self.addCleanup(temporary.cleanup)
        client = root / "wire-client"
        client.write_bytes(b"compiled wire client")
        client.chmod(0o755)
        first = WIRE_CLIENT_PACKAGER.package(
            root, "v0.1.0", client, "linux", "x86_64", root / "wire-a"
        )
        second = WIRE_CLIENT_PACKAGER.package(
            root, "v0.1.0", client, "linux", "x86_64", root / "wire-b"
        )
        self.assertEqual(SOURCE_PACKAGER.sha256(first), SOURCE_PACKAGER.sha256(second))
        import tarfile

        with tarfile.open(first, "r:xz") as archive:
            metadata_member = next(
                member for member in archive.getmembers() if member.name.endswith("WIRE-CLIENT.json")
            )
            metadata_file = archive.extractfile(metadata_member)
            self.assertIsNotNone(metadata_file)
            metadata = json.loads(metadata_file.read())
        self.assertEqual(metadata["wire_version"], 2)
        self.assertEqual(metadata["client_sha256"], SOURCE_PACKAGER.sha256(client))
        extracted = root / "extracted-wire-client"
        validate_wire_client_fixture(first, "0.1.0", metadata["git_sha"], 2, extracted)
        self.assertEqual(extracted.read_bytes(), client.read_bytes())
        self.assertTrue(extracted.stat().st_mode & 0o111)
        with self.assertRaisesRegex(WireClientFixtureError, "metadata disagrees"):
            validate_wire_client_fixture(
                first, "0.1.0", "f" * 40, 2, root / "wrong-wire-client"
            )
        with self.assertRaisesRegex(WireClientFixtureError, "already exists"):
            validate_wire_client_fixture(first, "0.1.0", metadata["git_sha"], 2, extracted)

    def test_independent_release_comparison_requires_exact_complete_identical_set(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-rebuild-compare-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        reference = root / "reference"
        rebuilt = root / "rebuilt"
        reference.mkdir()
        rebuilt.mkdir()
        names = (
            "GlifiStore-0.1.0.tar.xz",
            "glifistore-0.1.0-linux-x86_64.tar.xz",
            "glifistore-abi-v1-consumer-0.1.0-linux-x86_64.tar.xz",
            "glifistore-wire-v2-client-0.1.0-linux-x86_64.tar.xz",
        )
        for name in names:
            payload = f"deterministic {name}".encode("ascii")
            (reference / name).write_bytes(payload)
            (rebuilt / name).write_bytes(payload)
        authority = {
            "schema_version": 1,
            "product_version": "0.1.0",
            "abi": {"major": 1, "minor": 0},
            "wire_version": 2,
            "persistent_format_version": 1,
            "source": {"tag": "v0.1.0", "git_sha": "a" * 40},
            "target": {"os": "linux", "architecture": "x86_64"},
            "toolchain": {"compiler": "clang test", "linker": "ld test"},
            "build_options": ["CMAKE_BUILD_TYPE=Release"],
            "builder": {"runner": "intentionally different"},
        }
        reference_metadata = root / "reference-metadata.json"
        rebuilt_metadata = root / "rebuilt-metadata.json"
        reference_metadata.write_text(json.dumps(authority), encoding="utf-8")
        rebuilt_authority = dict(authority)
        rebuilt_authority["builder"] = {"runner": "independent"}
        rebuilt_metadata.write_text(json.dumps(rebuilt_authority), encoding="utf-8")
        compare_release_rebuild(
            reference,
            rebuilt,
            "0.1.0",
            1,
            2,
            "linux",
            "x86_64",
            reference_metadata,
            rebuilt_metadata,
        )

        rebuilt_authority["toolchain"] = {"compiler": "other", "linker": "ld test"}
        rebuilt_metadata.write_text(json.dumps(rebuilt_authority), encoding="utf-8")
        with self.assertRaisesRegex(RebuildComparisonError, "authority differs"):
            compare_release_rebuild(
                reference,
                rebuilt,
                "0.1.0",
                1,
                2,
                "linux",
                "x86_64",
                reference_metadata,
                rebuilt_metadata,
            )
        rebuilt_metadata.write_text(json.dumps(authority), encoding="utf-8")

        (rebuilt / names[-1]).write_bytes(b"different")
        with self.assertRaisesRegex(RebuildComparisonError, "differs"):
            compare_release_rebuild(reference, rebuilt, "0.1.0", 1, 2, "linux", "x86_64")
        (rebuilt / names[-1]).write_bytes((reference / names[-1]).read_bytes())
        (rebuilt / "unexpected.tar.xz").write_bytes(b"unexpected")
        with self.assertRaisesRegex(RebuildComparisonError, "set differs"):
            compare_release_rebuild(reference, rebuilt, "0.1.0", 1, 2, "linux", "x86_64")


def minimal_spdx(name: str = "artifact", packages: list[dict[str, object]] | None = None) -> dict[str, object]:
    return {
        "spdxVersion": "SPDX-2.3",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": name,
        "documentNamespace": f"https://glifistore.dev/sbom/{name}",
        "creationInfo": {"creators": ["Tool: test"]},
        "packages": packages or [],
    }


def write_bound_sbom(root: Path, artifact_name: str, metadata_path: Path) -> Path:
    input_path = root / "syft-input.json"
    output_path = root / f"{artifact_name}.spdx.json"
    input_path.write_text(json.dumps(minimal_spdx(artifact_name)), encoding="utf-8")
    bind_sbom(root / artifact_name, input_path, metadata_path, output_path)
    input_path.unlink()
    return output_path


def write_release_evidence(
    root: Path,
    filename: str,
    evidence_type: str,
    subject_name: str,
    *,
    omit_check: str | None = None,
    subject_root: Path | None = None,
) -> Path:
    log_name = f"{evidence_type}-evidence.log"
    (root / log_name).write_text(f"retained test log for {evidence_type}\n", encoding="utf-8")
    subject_path = (subject_root or root) / subject_name
    checks = [
        {
            "id": check_id,
            "status": "passed",
            "command": f"test-command --check {check_id}",
            "evidence_ref": log_name,
        }
        for check_id in sorted(REQUIRED_CHECKS[evidence_type])
        if check_id != omit_check
    ]
    value = {
        "schema_version": 1,
        "evidence_type": evidence_type,
        "result": "passed",
        "git_sha": "a" * 40,
        "product_version": "0.1.0",
        "generated_at": "2026-01-01T00:00:00Z",
        "producer": {
            "workflow": "release-evidence.yml@refs/tags/v0.1.0",
            "run_id": "123",
            "os": "test",
            "os_version": "1",
            "architecture": "x86_64",
        },
        "subject": {"name": subject_name, "sha256": SOURCE_PACKAGER.sha256(subject_path)},
        "checks": checks,
        "limitations": [],
    }
    output = root / filename
    output.write_text(json.dumps(value), encoding="utf-8")
    return output


class ReleaseBundleTests(unittest.TestCase):
    def make_bundle(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-bundle-test-")
        root = Path(temporary.name)
        (root / "GlifiStore-0.1.0.tar.xz").write_bytes(b"source")
        metadata = {
            "schema_version": 1,
            "product_version": "0.1.0",
            "abi": {"major": 1, "minor": 0},
            "wire_version": 2,
            "persistent_format_version": 1,
            "source": {
                "repository": "https://github.com/gpicchiarelli/GlifiStore",
                "tag": "v0.1.0",
                "git_sha": "a" * 40,
                "source_date_epoch": 1767225600,
            },
            "target": {
                "os": "linux",
                "os_version": "test",
                "architecture": "x86_64",
                "tls_backend": "none",
            },
            "toolchain": {
                "compiler": "clang test",
                "linker": "ld test",
                "cmake": "cmake test",
                "build_tool": "ninja test",
                "python": "3.13",
            },
            "build_options": ["CMAKE_BUILD_TYPE=Release"],
            "builder": {
                "workflow": "test-release.yml@refs/tags/v0.1.0",
                "run_id": "1",
                "runner_environment": "test",
            },
        }
        metadata_path = root / "build-metadata.json"
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        write_bound_sbom(root, "GlifiStore-0.1.0.tar.xz", metadata_path)
        (root / "candidate-seal.json").write_text("candidate seal subject\n", encoding="utf-8")
        return temporary, root

    def test_seal_rejects_changed_and_added_bytes(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        seal(root, "candidate-seal.json", "candidate")
        verify_seal(root, "candidate-seal.json")
        (root / "GlifiStore-0.1.0.tar.xz").write_bytes(b"tampered")
        with self.assertRaisesRegex(BundleError, "mismatch"):
            verify_seal(root, "candidate-seal.json")

        (root / "GlifiStore-0.1.0.tar.xz").write_bytes(b"source")
        (root / "unexpected.pkg").write_bytes(b"extra")
        with self.assertRaisesRegex(BundleError, "file set mismatch"):
            verify_seal(root, "candidate-seal.json")

    def test_admission_allows_evidence_augmentation_without_weakening_candidate(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        seal(root, "candidate-seal.json", "candidate")
        write_candidate_admission(root)
        validate_candidate_admission(root)

        staging_temporary = tempfile.TemporaryDirectory(prefix="glifistore-evidence-test-")
        self.addCleanup(staging_temporary.cleanup)
        staging = Path(staging_temporary.name)
        (staging / "post-admission-evidence.log").write_text(
            "retained evidence\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(BundleError, "file set mismatch"):
            verify_seal(root, "candidate-seal.json")
        write_evidence_import(root, [staging])
        validate_evidence_import(root)
        verification = write_verification(root, "candidate-seal.json")
        self.assertTrue(verification.is_file())

        (root / "GlifiStore-0.1.0.tar.xz").write_bytes(b"tampered")
        with self.assertRaisesRegex(BundleError, "mismatch"):
            write_verification(root, "candidate-seal.json")

    def test_verification_rejects_augmentation_without_prior_admission(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        seal(root, "candidate-seal.json", "candidate")
        (root / "post-build-evidence.log").write_text("unadmitted\n", encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "candidate-admission"):
            write_verification(root, "candidate-seal.json")

    def test_evidence_import_rejects_collision_protected_files_and_injection(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        seal(root, "candidate-seal.json", "candidate")
        write_candidate_admission(root)

        protected_temporary = tempfile.TemporaryDirectory(prefix="glifistore-protected-test-")
        self.addCleanup(protected_temporary.cleanup)
        protected = Path(protected_temporary.name)
        (protected / "verification.json").write_text("forged\n", encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "protected file"):
            write_evidence_import(root, [protected])

        first_temporary = tempfile.TemporaryDirectory(prefix="glifistore-first-test-")
        second_temporary = tempfile.TemporaryDirectory(prefix="glifistore-second-test-")
        self.addCleanup(first_temporary.cleanup)
        self.addCleanup(second_temporary.cleanup)
        first = Path(first_temporary.name)
        second = Path(second_temporary.name)
        (first / "same.log").write_text("first\n", encoding="utf-8")
        (second / "same.log").write_text("second\n", encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "duplicates filename"):
            write_evidence_import(root, [first, second])

        (second / "same.log").unlink()
        (second / "second.log").write_text("second\n", encoding="utf-8")
        write_evidence_import(root, [first, second])
        (root / "injected.log").write_text("not in receipt\n", encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "augmented candidate file set mismatch"):
            write_verification(root, "candidate-seal.json")

    def test_evidence_import_detects_tampered_imported_bytes(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        seal(root, "candidate-seal.json", "candidate")
        write_candidate_admission(root)
        staging_temporary = tempfile.TemporaryDirectory(prefix="glifistore-import-test-")
        self.addCleanup(staging_temporary.cleanup)
        staging = Path(staging_temporary.name)
        (staging / "evidence.log").write_text("original\n", encoding="utf-8")
        write_evidence_import(root, [staging])
        (root / "evidence.log").write_text("tampered\n", encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "digest mismatch"):
            validate_evidence_import(root)

    def test_admission_rejects_wrong_seal_kind_and_replay(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        seal(root, "candidate-seal.json", "verified")
        with self.assertRaisesRegex(BundleError, "kind mismatch"):
            write_candidate_admission(root)

        seal(root, "candidate-seal.json", "candidate")
        write_candidate_admission(root)
        with self.assertRaisesRegex(BundleError, "already exists"):
            write_candidate_admission(root)

    def test_manifest_checksums_and_sbom_are_bound(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        validate_sbom(root / "GlifiStore-0.1.0.tar.xz.spdx.json")
        write_release_manifest(root)
        validate_release_manifest(root)
        write_checksums(root)
        verify_checksums(root)

        (root / "GlifiStore-0.1.0.tar.xz.spdx.json").write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "SPDX 2.3"):
            validate_sbom(root / "GlifiStore-0.1.0.tar.xz.spdx.json")
        with self.assertRaisesRegex(BundleError, "mismatch"):
            verify_checksums(root)

    def test_wrong_manifest_digest_fails(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        manifest_path = write_release_manifest(root)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["artifacts"][0]["sha256"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "digest mismatch"):
            validate_release_manifest(root)

    def test_manifest_rejects_wrong_kind_and_evidence_index(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        manifest_path = write_release_manifest(root)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["artifacts"][0]["kind"] = "provenance"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "artifact metadata"):
            validate_release_manifest(root)

        manifest = json.loads(write_release_manifest(root).read_text(encoding="utf-8"))
        manifest["compatibility_evidence_refs"] = ["invented-evidence.json"]
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "evidence index"):
            validate_release_manifest(root)

    def test_build_metadata_rejects_unknown_or_incomplete_authority(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        metadata_path = root / "build-metadata.json"
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        metadata["source"]["tag"] = "v0.2.0"
        metadata["unreviewed_field"] = True
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaisesRegex(BundleError, "fields differ"):
            validate_build_metadata(metadata_path)

    def test_sbom_is_cryptographically_bound_to_subject(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        sbom = root / "GlifiStore-0.1.0.tar.xz.spdx.json"
        validate_sbom(sbom)
        (root / "GlifiStore-0.1.0.tar.xz").write_bytes(b"different bytes")
        with self.assertRaisesRegex(BundleError, "checksum disagrees"):
            validate_sbom(sbom)

    def test_sbom_rejects_dependency_without_resolved_license(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        raw = root / "unlicensed-input.json"
        raw.write_text(
            json.dumps(
                minimal_spdx(
                    packages=[
                        {
                            "name": "unknown-dependency",
                            "SPDXID": "SPDXRef-Package-unknown",
                            "licenseDeclared": "NOASSERTION",
                            "licenseConcluded": "NOASSERTION",
                        }
                    ]
                )
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(BundleError, "no resolved license"):
            bind_sbom(
                root / "GlifiStore-0.1.0.tar.xz",
                raw,
                root / "build-metadata.json",
                root / "GlifiStore-0.1.0.tar.xz.spdx.json",
            )

    def test_release_evidence_requires_complete_checks_and_retained_logs(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        evidence = write_release_evidence(
            root,
            "abi-compatibility-evidence.json",
            "abi_compatibility",
            "candidate-seal.json",
            omit_check="old-consumer-new-library",
        )
        with self.assertRaisesRegex(EvidenceError, "check set differs"):
            validate_evidence(evidence, require_ci=True)
        value = json.loads(evidence.read_text(encoding="utf-8"))
        value["checks"].append(
            {
                "id": "old-consumer-new-library",
                "status": "passed",
                "command": "test old binary",
                "evidence_ref": "missing.log",
            }
        )
        evidence.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(EvidenceError, "log is missing"):
            validate_evidence(evidence, require_ci=True)

    def test_release_evidence_writer_derives_identity_and_validates_before_publish(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        log_name = "abi-writer.log"
        (root / log_name).write_text("real retained execution log\n", encoding="utf-8")
        plan = root / "checks.json"
        plan.write_text(
            json.dumps(
                [
                    {
                        "id": check_id,
                        "command": f"test-command --check {check_id}",
                        "evidence_ref": log_name,
                    }
                    for check_id in sorted(REQUIRED_CHECKS["abi_compatibility"])
                ]
            ),
            encoding="utf-8",
        )
        output = root / "abi-compatibility-evidence.json"
        with mock.patch.dict(
            os.environ,
            {
                "GITHUB_SHA": "a" * 40,
                "GITHUB_RUN_ID": "456",
                "GITHUB_WORKFLOW_REF": "release.yml@refs/tags/v0.1.0",
                "RUNNER_OS": "Linux",
                "RUNNER_ARCH": "X64",
            },
        ):
            create_evidence(
                root=ROOT,
                evidence_type="abi_compatibility",
                subject=root / "candidate-seal.json",
                output=output,
                check_plan=plan,
                limitations=["first tagged baseline still pending"],
                require_ci=True,
            )
        value = validate_evidence(output, require_ci=True)
        self.assertEqual(value["producer"]["run_id"], "456")
        self.assertEqual(value["subject"]["name"], "candidate-seal.json")

    def test_release_evidence_writer_fails_closed_without_ci_identity(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        log_name = "repro.log"
        (root / log_name).write_text("retained\n", encoding="utf-8")
        plan = root / "checks.json"
        plan.write_text(
            json.dumps(
                [
                    {
                        "id": check_id,
                        "command": check_id,
                        "evidence_ref": log_name,
                    }
                    for check_id in sorted(REQUIRED_CHECKS["reproducibility"])
                ]
            ),
            encoding="utf-8",
        )
        output = root / "reproducibility-evidence.json"
        environment = os.environ.copy()
        for name in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_WORKFLOW_REF"):
            environment.pop(name, None)
        with mock.patch.dict(os.environ, environment, clear=True):
            with self.assertRaisesRegex(EvidenceError, "requires retained CI evidence"):
                create_evidence(
                    root=ROOT,
                    evidence_type="reproducibility",
                    subject=root / "candidate-seal.json",
                    output=output,
                    check_plan=plan,
                    limitations=[],
                    require_ci=True,
                )
        self.assertFalse(output.exists())

    def test_release_evidence_writer_rejects_noncanonical_or_existing_output(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        (root / "repro.log").write_text("retained\n", encoding="utf-8")
        plan = root / "checks.json"
        plan.write_text(
            json.dumps(
                [
                    {"id": check_id, "command": check_id, "evidence_ref": "repro.log"}
                    for check_id in sorted(REQUIRED_CHECKS["reproducibility"])
                ]
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(EvidenceError, "must be reproducibility-evidence.json"):
            create_evidence(
                root=ROOT,
                evidence_type="reproducibility",
                subject=root / "candidate-seal.json",
                output=root / "wrong-name.json",
                check_plan=plan,
                limitations=[],
                require_ci=False,
            )
        output = root / "reproducibility-evidence.json"
        output.write_text("do not replace\n", encoding="utf-8")
        with self.assertRaisesRegex(EvidenceError, "refusing to replace"):
            create_evidence(
                root=ROOT,
                evidence_type="reproducibility",
                subject=root / "candidate-seal.json",
                output=output,
                check_plan=plan,
                limitations=[],
                require_ci=False,
            )

    def test_release_policy_requires_real_platform_artifacts_and_evidence(self) -> None:
        temporary, root = self.make_bundle()
        self.addCleanup(temporary.cleanup)
        write_release_manifest(root)
        with self.assertRaisesRegex(BundleError, "candidate admission"):
            validate_release_policy(root)

        (root / "release-manifest.json").unlink()
        seal(root, "candidate-seal.json", "candidate")
        with mock.patch.dict(
            os.environ,
            {
                "GITHUB_RUN_ID": "123",
                "GITHUB_WORKFLOW_REF": "release.yml@refs/tags/v0.1.0",
            },
        ):
            write_candidate_admission(root)
        staging_temporary = tempfile.TemporaryDirectory(prefix="glifistore-policy-inputs-")
        self.addCleanup(staging_temporary.cleanup)
        staging = Path(staging_temporary.name)
        for artifact in (
            "glifistore-0.1.0-linux-x86_64.tar.xz",
            "glifistore-abi-v1-consumer-0.1.0-linux-x86_64.tar.xz",
            "glifistore-wire-v2-client-0.1.0-linux-x86_64.tar.xz",
            "glifistore-0.1.0-freebsd14.3-amd64.pkg",
            "glifistore-0.1.0-openbsd7.9-amd64.tgz",
        ):
            (staging / artifact).write_bytes(artifact.encode("ascii"))
            write_bound_sbom(staging, artifact, root / "build-metadata.json")
        evidence_specs = {
            "abi-compatibility-evidence.json": ("abi_compatibility", "candidate-seal.json"),
            "persistent-compatibility-evidence.json": (
                "persistent_compatibility",
                "candidate-seal.json",
            ),
            "sdk-installed-interop-evidence.json": ("sdk_installed_interop", "candidate-seal.json"),
            "security-matrix-evidence.json": ("security_matrix", "candidate-seal.json"),
            "wire-compatibility-evidence.json": ("wire_compatibility", "candidate-seal.json"),
            "freebsd-package-evidence.json": (
                "freebsd_package",
                "glifistore-0.1.0-freebsd14.3-amd64.pkg",
            ),
            "openbsd-package-evidence.json": (
                "openbsd_package",
                "glifistore-0.1.0-openbsd7.9-amd64.tgz",
            ),
            "reproducibility-evidence.json": ("reproducibility", "candidate-seal.json"),
        }
        for filename, (evidence_type, subject) in evidence_specs.items():
            subject_root = root if subject == "candidate-seal.json" else staging
            write_release_evidence(
                staging,
                filename,
                evidence_type,
                subject,
                subject_root=subject_root,
            )
        with mock.patch.dict(
            os.environ,
            {
                "GITHUB_RUN_ID": "123",
                "GITHUB_WORKFLOW_REF": "release.yml@refs/tags/v0.1.0",
            },
        ):
            write_evidence_import(root, [staging])
        write_release_manifest(root)
        validate_release_policy(root)

        evidence_path = root / "abi-compatibility-evidence.json"
        evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
        evidence["producer"]["run_id"] = "999"
        evidence_path.write_text(json.dumps(evidence), encoding="utf-8")
        receipt_path = root / "evidence-import.json"
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        for entry in receipt["files"]:
            if entry["name"] == evidence_path.name:
                entry["sha256"] = SOURCE_PACKAGER.sha256(evidence_path)
                entry["size"] = evidence_path.stat().st_size
        receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
        write_release_manifest(root)
        with self.assertRaisesRegex(BundleError, "different workflow run"):
            validate_release_policy(root)


if __name__ == "__main__":
    unittest.main()
