from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from engineering.tools.generate_release_context import (
    ReleaseContextError,
    build_context,
    load_release_context,
    validate_release_context,
)
from engineering.tools.package_framework import PackageFrameworkError


ROOT = Path(__file__).resolve().parents[2]
GIT_IDENTITY = (
    "-c",
    "user.email=tests@glifistore.invalid",
    "-c",
    "user.name=GlifiStore Tests",
    "-c",
    "commit.gpgsign=false",
    "-c",
    "tag.gpgsign=false",
)


def git(repository: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repository), *GIT_IDENTITY, *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def commit_version(repository: Path, version: str, abi: str = "1.0") -> None:
    (repository / "VERSION").write_text(f"{version}\n", encoding="utf-8")
    (repository / "ABI_VERSION").write_text(f"{abi}\n", encoding="utf-8")
    git(repository, "add", "VERSION", "ABI_VERSION")
    git(repository, "commit", "-q", "-m", f"chore(release): {version}")


def init_repository(repository: Path, version: str, abi: str = "1.0") -> None:
    subprocess.run(
        ["git", "-c", "init.defaultBranch=main", "init", "-q", str(repository)],
        check=True,
        capture_output=True,
    )
    commit_version(repository, version, abi)


class ReleaseContextTests(unittest.TestCase):
    def temporary_repository(self, version: str, abi: str = "1.0") -> Path:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-release-context-")
        self.addCleanup(directory.cleanup)
        repository = Path(directory.name) / "repository"
        repository.mkdir()
        init_repository(repository, version, abi)
        return repository

    def test_first_release_has_no_previous_baseline(self) -> None:
        repository = self.temporary_repository("0.1.0")
        context = build_context(repository)
        self.assertEqual(context["product_version"], "0.1.0")
        self.assertEqual(context["release_kind"], "initial")
        self.assertFalse(context["previous"]["available"])
        self.assertIsNone(context["previous"]["version"])
        self.assertIn("no annotated release tag precedes 0.1.0", context["previous"]["reason"])
        self.assertTrue(
            any("NOT_APPLICABLE_INITIAL_BASELINE" in item for item in context["limitations"])
        )

    def test_previous_release_is_semver_aware_not_current_minus_one(self) -> None:
        repository = self.temporary_repository("0.9.0")
        git(repository, "tag", "-a", "v0.9.0", "-m", "v0.9.0")
        commit_version(repository, "0.10.0")
        git(repository, "tag", "-a", "v0.10.0", "-m", "v0.10.0")
        commit_version(repository, "1.0.0")

        context = build_context(repository)
        self.assertEqual(context["release_kind"], "major")
        self.assertTrue(context["previous"]["available"])
        self.assertEqual(context["previous"]["version"], "0.10.0")
        self.assertEqual(context["previous"]["tag"], "v0.10.0")
        self.assertEqual(context["previous"]["abi_major"], 1)
        self.assertEqual(
            context["previous"]["git_sha"], git(repository, "rev-parse", "refs/tags/v0.10.0^{commit}")
        )

    def test_lightweight_tags_are_not_release_authorities(self) -> None:
        repository = self.temporary_repository("0.1.0")
        git(repository, "tag", "v0.1.0")
        commit_version(repository, "0.2.0")

        context = build_context(repository)
        self.assertFalse(context["previous"]["available"])
        self.assertEqual(context["release_kind"], "initial")
        self.assertFalse(context["git"]["tag_is_annotated"])

    def test_prereleases_are_excluded_from_the_baseline_unless_requested(self) -> None:
        repository = self.temporary_repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.2.0-rc.1")
        git(repository, "tag", "-a", "v0.2.0-rc.1", "-m", "v0.2.0-rc.1")
        commit_version(repository, "0.2.0")

        self.assertEqual(build_context(repository)["previous"]["version"], "0.1.0")
        self.assertEqual(
            build_context(repository, include_prereleases=True)["previous"]["version"], "0.2.0-rc.1"
        )

    def test_tag_identity_is_bound_to_the_version_authority(self) -> None:
        repository = self.temporary_repository("0.1.0")
        git(repository, "tag", "-a", "v0.4.0", "-m", "mislabelled")
        commit_version(repository, "0.5.0")
        with self.assertRaisesRegex(ReleaseContextError, "disagrees with its VERSION"):
            build_context(repository)

    def test_tag_state_is_reported_for_the_described_commit(self) -> None:
        repository = self.temporary_repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        context = build_context(repository)
        self.assertTrue(context["git"]["tag_is_annotated"])
        self.assertTrue(context["git"]["tag_matches_commit"])
        self.assertTrue(context["git"]["tree_clean"])
        self.assertEqual(context["git"]["tag"], "v0.1.0")

    def test_a_dirty_tree_is_reported_and_refused_by_the_release_profile(self) -> None:
        repository = self.temporary_repository("0.1.0")
        (repository / "VERSION").write_text("0.1.0\n\n", encoding="utf-8")
        context = build_context(repository)
        self.assertFalse(context["git"]["tree_clean"])
        self.assertTrue(any("not sealed" in item for item in context["limitations"]))
        with self.assertRaisesRegex(ReleaseContextError, "working tree"):
            build_context(repository, require_clean=True)

    def test_package_versions_are_derived_from_version_and_package_revision(self) -> None:
        repository = self.temporary_repository("0.1.0")
        context = build_context(repository, package_revision=2)
        self.assertEqual(context["package_revision"], 2)
        self.assertEqual(context["package_versions"]["deb"]["package_version"], "0.1.0-3")
        self.assertEqual(context["package_versions"]["openbsd"]["package_version"], "0.1.0p2")
        self.assertEqual(
            sorted(context["package_versions"]),
            ["deb", "freebsd", "homebrew", "macports", "openbsd", "rpm"],
        )

    def test_invalid_authorities_are_refused(self) -> None:
        repository = self.temporary_repository("0.1.0")
        (repository / "VERSION").write_text("0.1\n", encoding="utf-8")
        with self.assertRaisesRegex(ReleaseContextError, "VERSION authority is invalid"):
            build_context(repository)
        (repository / "VERSION").write_text("0.1.0\n", encoding="utf-8")
        (repository / "ABI_VERSION").write_text("one\n", encoding="utf-8")
        with self.assertRaisesRegex(ReleaseContextError, "ABI_VERSION"):
            build_context(repository)

    def test_negative_package_revision_is_refused(self) -> None:
        repository = self.temporary_repository("0.1.0")
        with self.assertRaisesRegex(ReleaseContextError, "non-negative"):
            build_context(repository, package_revision=-1)


class ReleaseContextValidationTests(unittest.TestCase):
    def context(self) -> dict:
        return build_context(ROOT)

    def test_the_repository_context_validates_against_its_schema(self) -> None:
        context = self.context()
        self.assertEqual(context["product_version"], (ROOT / "VERSION").read_text().strip())
        validate_release_context(context)

    def test_a_previous_release_without_identity_is_refused(self) -> None:
        context = copy.deepcopy(self.context())
        context["previous"]["available"] = True
        with self.assertRaisesRegex(ReleaseContextError, "misses part of its identity"):
            validate_release_context(context)

    def test_an_initial_kind_with_a_previous_release_is_refused(self) -> None:
        context = copy.deepcopy(self.context())
        context["previous"] = {
            "available": True,
            "version": "0.0.9",
            "tag": "v0.0.9",
            "git_sha": "0" * 40,
            "abi_major": 1,
            "reason": "fabricated",
        }
        context["release_kind"] = "initial"
        with self.assertRaisesRegex(ReleaseContextError, "cannot be initial"):
            validate_release_context(context)

    def test_schema_violations_are_refused(self) -> None:
        context = copy.deepcopy(self.context())
        context["release_kind"] = "hotfix"
        with self.assertRaisesRegex(PackageFrameworkError, "release-context.schema.json"):
            validate_release_context(context)

    def test_the_command_line_writes_a_loadable_context(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-context-cli-") as temporary:
            output = Path(temporary) / "release-context.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "engineering/tools/generate_release_context.py"),
                    "--root",
                    str(ROOT),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(output.read_text())["schema_version"], 1)
            load_release_context(output)


if __name__ == "__main__":
    unittest.main()
