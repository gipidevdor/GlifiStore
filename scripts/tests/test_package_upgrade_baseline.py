from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "engineering" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from release_bundle import (  # noqa: E402
    BundleError,
    seal,
    write_checksums,
    write_release_manifest,
)

from engineering.tools.generate_release_context import build_context  # noqa: E402
from engineering.tools.package_framework import PackageFrameworkError  # noqa: E402
from engineering.tools.semver_policy import parse  # noqa: E402
from engineering.tools.upgrade_baseline import (  # noqa: E402
    PublishedRelease,
    UpgradeBaselineError,
    admit,
    load_release_index,
    resolve_baseline,
    validate_baseline,
    verify_downloaded_bundle,
)


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


def commit_version(repository: Path, version: str, abi: str = "1.0") -> str:
    (repository / "VERSION").write_text(f"{version}\n", encoding="utf-8")
    (repository / "ABI_VERSION").write_text(f"{abi}\n", encoding="utf-8")
    git(repository, "add", "VERSION", "ABI_VERSION")
    git(repository, "commit", "-q", "-m", f"chore(release): {version}")
    return git(repository, "rev-parse", "HEAD")


def complete_assets(version: str, *, abi: int = 1, wire: int = 2, arch: str = "amd64") -> list[str]:
    consumer = f"glifistore-abi-v{abi}-consumer-{version}-linux-{arch}.tar.xz"
    client = f"glifistore-wire-v{wire}-client-{version}-linux-{arch}.tar.xz"
    return [
        "SHA256SUMS",
        "build-metadata.json",
        "release-manifest.json",
        "verified-seal.json",
        f"GlifiStore-{version}.tar.xz",
        f"glifistore-{version}-linux-{arch}.tar.xz",
        consumer,
        f"{consumer}.spdx.json",
        client,
        f"{client}.spdx.json",
    ]


def published(
    tag: str,
    *,
    draft: bool = False,
    prerelease: bool = False,
    assets: list[str] | None = None,
) -> PublishedRelease:
    version = tag[1:]
    return PublishedRelease(
        tag=tag,
        draft=draft,
        prerelease=prerelease,
        assets=tuple(sorted(complete_assets(version) if assets is None else assets)),
    )


def build_metadata(version: str, git_sha: str, abi: tuple[int, int] = (1, 0)) -> dict:
    return {
        "schema_version": 1,
        "product_version": version,
        "abi": {"major": abi[0], "minor": abi[1]},
        "wire_version": 2,
        "persistent_format_version": 1,
        "source": {
            "repository": "https://github.com/gpicchiarelli/GlifiStore",
            "tag": f"v{version}",
            "git_sha": git_sha,
            "source_date_epoch": 1767225600,
        },
        "target": {
            "os": "linux",
            "os_version": "test",
            "architecture": "amd64",
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
            "workflow": "test-release.yml@refs/tags/v" + version,
            "run_id": "1",
            "runner_environment": "test",
        },
    }


def write_bundle(directory: Path, version: str, git_sha: str, *, arch: str = "amd64") -> Path:
    """A sealed prior-release bundle: real digests, real seal, real checksums."""
    directory.mkdir(parents=True)
    for name in complete_assets(version, arch=arch):
        if name in ("SHA256SUMS", "build-metadata.json", "release-manifest.json", "verified-seal.json"):
            continue
        if name.endswith(".spdx.json"):
            (directory / name).write_text(
                json.dumps({"spdxVersion": "SPDX-2.3", "name": name}), encoding="utf-8"
            )
        else:
            (directory / name).write_bytes(f"{name} bytes".encode("ascii"))
    (directory / "build-metadata.json").write_text(
        json.dumps(build_metadata(version, git_sha)), encoding="utf-8"
    )
    write_release_manifest(directory)
    write_checksums(directory)
    seal(directory, "verified-seal.json", "verified")
    return directory


class BaselineSelectionTests(unittest.TestCase):
    def repository(self, version: str, abi: str = "1.0") -> Path:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-upgrade-baseline-")
        self.addCleanup(temporary.cleanup)
        repository = Path(temporary.name) / "repository"
        repository.mkdir()
        subprocess.run(
            ["git", "-c", "init.defaultBranch=main", "init", "-q", str(repository)],
            check=True,
            capture_output=True,
        )
        commit_version(repository, version, abi)
        return repository

    def resolve(self, repository: Path, current: str, **keywords) -> dict:
        return resolve_baseline(
            repository,
            current=parse(current, allow_v_prefix=False),
            abi_major=keywords.pop("abi_major", 1),
            **keywords,
        )

    def refusals(self, baseline: dict) -> dict[str, str]:
        return {
            entry["tag"]: entry["reason"]
            for entry in baseline["rejected"]
            if entry["reason"] not in ("not_semver", "not_older")
        }

    def test_a_first_release_is_an_initial_baseline_with_no_refused_predecessor(self) -> None:
        repository = self.repository("0.1.0")
        baseline = self.resolve(repository, "0.1.0", index={})
        self.assertFalse(baseline["available"])
        self.assertEqual(baseline["upgrade_check_status"], "NOT_APPLICABLE_INITIAL_BASELINE")
        self.assertEqual(baseline["rejected"], [])
        self.assertIn("initial baseline", baseline["reason"])

    def test_the_greatest_admissible_release_is_selected_not_the_numerically_previous_one(
        self,
    ) -> None:
        repository = self.repository("0.9.0")
        git(repository, "tag", "-a", "v0.9.0", "-m", "v0.9.0")
        commit_version(repository, "0.10.0")
        git(repository, "tag", "-a", "v0.10.0", "-m", "v0.10.0")
        commit_version(repository, "1.0.0")

        baseline = self.resolve(
            repository,
            "1.0.0",
            index={
                "v0.9.0": published("v0.9.0"),
                "v0.10.0": published("v0.10.0"),
            },
        )
        self.assertTrue(baseline["available"])
        self.assertEqual(baseline["selected"]["version"], "0.10.0")
        self.assertEqual(baseline["upgrade_check_status"], "NOT_RUN")
        self.assertEqual(self.refusals(baseline), {"v0.9.0": "superseded"})
        self.assertEqual(
            baseline["selected"]["git_sha"],
            git(repository, "rev-parse", "refs/tags/v0.10.0^{commit}"),
        )

    def test_a_newer_release_is_never_taken_as_the_baseline(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.3.0")
        git(repository, "tag", "-a", "v0.3.0", "-m", "v0.3.0")

        baseline = self.resolve(
            repository,
            "0.2.0",
            index={"v0.1.0": published("v0.1.0"), "v0.3.0": published("v0.3.0")},
        )
        self.assertEqual(baseline["selected"]["tag"], "v0.1.0")
        self.assertIn(
            {
                "tag": "v0.3.0",
                "version": "0.3.0",
                "reason": "not_older",
                "detail": "0.3.0 does not precede 0.2.0",
            },
            baseline["rejected"],
        )

    def test_a_draft_predecessor_blocks_the_upgrade_instead_of_faking_a_first_release(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.2.0")

        baseline = self.resolve(
            repository, "0.2.0", index={"v0.1.0": published("v0.1.0", draft=True)}
        )
        self.assertFalse(baseline["available"])
        self.assertEqual(baseline["upgrade_check_status"], "BLOCKED")
        self.assertEqual(self.refusals(baseline), {"v0.1.0": "draft_release"})

    def test_an_unpublished_annotated_tag_is_not_a_sealed_release(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.2.0")

        baseline = self.resolve(repository, "0.2.0", index={})
        self.assertEqual(baseline["upgrade_check_status"], "BLOCKED")
        self.assertEqual(self.refusals(baseline), {"v0.1.0": "no_published_release"})

    def test_a_lightweight_tag_is_not_a_release_authority(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "v0.1.0")
        commit_version(repository, "0.2.0")

        baseline = self.resolve(repository, "0.2.0", index={"v0.1.0": published("v0.1.0")})
        self.assertEqual(self.refusals(baseline), {"v0.1.0": "tag_not_annotated"})

    def test_an_index_tag_without_a_repository_tag_is_refused(self) -> None:
        repository = self.repository("0.2.0")
        baseline = self.resolve(repository, "0.2.0", index={"v0.1.0": published("v0.1.0")})
        self.assertEqual(self.refusals(baseline), {"v0.1.0": "tag_not_annotated"})
        self.assertEqual(baseline["upgrade_check_status"], "BLOCKED")

    def test_an_incomplete_asset_set_cannot_support_an_upgrade(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.2.0")
        for dropped in (
            "SHA256SUMS",
            "release-manifest.json",
            "build-metadata.json",
            "GlifiStore-0.1.0.tar.xz",
            "glifistore-0.1.0-linux-amd64.tar.xz",
            "glifistore-abi-v1-consumer-0.1.0-linux-amd64.tar.xz.spdx.json",
        ):
            assets = [name for name in complete_assets("0.1.0") if name != dropped]
            baseline = self.resolve(
                repository, "0.2.0", index={"v0.1.0": published("v0.1.0", assets=assets)}
            )
            self.assertEqual(
                self.refusals(baseline), {"v0.1.0": "incomplete_assets"}, f"dropped {dropped}"
            )

    def test_a_prerelease_is_not_a_stable_upgrade_baseline(self) -> None:
        repository = self.repository("0.1.0")
        commit_version(repository, "0.2.0-rc.1")
        git(repository, "tag", "-a", "v0.2.0-rc.1", "-m", "v0.2.0-rc.1")
        commit_version(repository, "0.2.0")
        index = {"v0.2.0-rc.1": published("v0.2.0-rc.1", prerelease=True)}

        baseline = self.resolve(repository, "0.2.0", index=index)
        self.assertEqual(baseline["upgrade_check_status"], "BLOCKED")
        self.assertEqual(self.refusals(baseline), {"v0.2.0-rc.1": "prerelease_excluded"})

        requested = self.resolve(repository, "0.2.0", index=index, include_prereleases=True)
        self.assertEqual(requested["selected"]["version"], "0.2.0-rc.1")
        self.assertTrue(
            any("not a supported upgrade source" in item for item in requested["limitations"])
        )

    def test_a_prerelease_published_as_stable_is_refused(self) -> None:
        repository = self.repository("0.1.0")
        commit_version(repository, "0.2.0-rc.1")
        git(repository, "tag", "-a", "v0.2.0-rc.1", "-m", "v0.2.0-rc.1")
        commit_version(repository, "0.2.0")

        baseline = self.resolve(
            repository,
            "0.2.0",
            index={"v0.2.0-rc.1": published("v0.2.0-rc.1", prerelease=False)},
            include_prereleases=True,
        )
        self.assertEqual(
            self.refusals(baseline), {"v0.2.0-rc.1": "prerelease_published_as_stable"}
        )

    def test_a_stable_release_published_as_a_prerelease_is_refused(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.2.0")

        baseline = self.resolve(
            repository, "0.2.0", index={"v0.1.0": published("v0.1.0", prerelease=True)}
        )
        self.assertEqual(self.refusals(baseline), {"v0.1.0": "stable_published_as_prerelease"})

    def test_an_incompatible_abi_major_is_refused(self) -> None:
        repository = self.repository("0.1.0", "1.0")
        git(repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        commit_version(repository, "0.2.0", "2.0")

        baseline = self.resolve(
            repository, "0.2.0", abi_major=2, index={"v0.1.0": published("v0.1.0")}
        )
        self.assertEqual(self.refusals(baseline), {"v0.1.0": "incompatible_abi"})

    def test_a_tag_disagreeing_with_its_version_authority_is_refused(self) -> None:
        repository = self.repository("0.1.0")
        git(repository, "tag", "-a", "v0.4.0", "-m", "mislabelled")
        commit_version(repository, "0.5.0")

        baseline = self.resolve(repository, "0.5.0", index={"v0.4.0": published("v0.4.0")})
        self.assertEqual(self.refusals(baseline), {"v0.4.0": "tag_version_mismatch"})

    def test_the_repository_resolves_its_own_honest_baseline(self) -> None:
        context = build_context(ROOT)
        baseline = resolve_baseline(
            ROOT,
            current=parse(context["product_version"], allow_v_prefix=False),
            abi_major=context["abi"]["major"],
        )
        self.assertEqual(baseline["available"], context["previous"]["available"])
        if not context["previous"]["available"]:
            self.assertEqual(
                baseline["upgrade_check_status"], "NOT_APPLICABLE_INITIAL_BASELINE"
            )


class BaselineHonestyTests(unittest.TestCase):
    def blocked(self) -> dict:
        return {
            "schema_version": 1,
            "generated_at": "2026-01-01T00:00:00Z",
            "product_version": "0.2.0",
            "abi_major": 1,
            "wire_version": 2,
            "include_prereleases": False,
            "available": False,
            "upgrade_check_status": "BLOCKED",
            "reason": "1 tag(s) precede 0.2.0 but none is an admissible sealed release",
            "selected": None,
            "rejected": [
                {
                    "tag": "v0.1.0",
                    "version": "0.1.0",
                    "reason": "draft_release",
                    "detail": "the release is still a draft",
                }
            ],
            "limitations": [],
        }

    def test_an_initial_baseline_cannot_be_claimed_over_a_refused_predecessor(self) -> None:
        baseline = self.blocked()
        baseline["upgrade_check_status"] = "NOT_APPLICABLE_INITIAL_BASELINE"
        with self.assertRaisesRegex(UpgradeBaselineError, "cannot coexist"):
            validate_baseline(baseline)

    def test_blocked_requires_a_refused_predecessor(self) -> None:
        baseline = self.blocked()
        baseline["rejected"] = []
        with self.assertRaisesRegex(UpgradeBaselineError, "at least one refused predecessor"):
            validate_baseline(baseline)

    def test_not_run_requires_an_admissible_baseline(self) -> None:
        baseline = self.blocked()
        baseline["upgrade_check_status"] = "NOT_RUN"
        with self.assertRaisesRegex(UpgradeBaselineError, "NOT_RUN requires"):
            validate_baseline(baseline)

    def test_a_selected_baseline_may_not_report_anything_but_not_run(self) -> None:
        baseline = self.blocked()
        baseline["available"] = True
        baseline["selected"] = {
            "version": "0.1.0",
            "tag": "v0.1.0",
            "git_sha": "a" * 40,
            "abi_major": 1,
            "assets": {
                "manifest": "release-manifest.json",
                "seal": "verified-seal.json",
                "checksums": "SHA256SUMS",
                "provenance": None,
                "source_archive": "GlifiStore-0.1.0.tar.xz",
                "install_archive": "glifistore-0.1.0-linux-amd64.tar.xz",
                "abi_consumer_archive": "glifistore-abi-v1-consumer-0.1.0-linux-amd64.tar.xz",
                "wire_client_archive": "glifistore-wire-v2-client-0.1.0-linux-amd64.tar.xz",
            },
        }
        with self.assertRaisesRegex(UpgradeBaselineError, "only report NOT_RUN"):
            validate_baseline(baseline)

    def test_an_availability_flag_disagreeing_with_the_selection_is_refused(self) -> None:
        baseline = self.blocked()
        baseline["available"] = True
        with self.assertRaisesRegex(UpgradeBaselineError, "availability disagrees"):
            validate_baseline(baseline)

    def test_schema_violations_are_refused(self) -> None:
        baseline = self.blocked()
        baseline["upgrade_check_status"] = "SKIPPED"
        with self.assertRaisesRegex(PackageFrameworkError, "upgrade-baseline.schema.json"):
            validate_baseline(baseline)

    def test_a_release_index_is_validated_fail_closed(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-release-index-")
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "index.json"
        valid = {
            "schema_version": 1,
            "releases": [
                {"tag": "v0.1.0", "draft": False, "prerelease": False, "assets": ["SHA256SUMS"]}
            ],
        }
        path.write_text(json.dumps(valid), encoding="utf-8")
        self.assertEqual(sorted(load_release_index(path)), ["v0.1.0"])

        for mutation, expected in (
            ({"schema_version": 2}, "schema version"),
            ({"releases": {}}, "releases array"),
            (
                {"releases": [{"tag": "v0.1.0", "draft": False, "assets": []}]},
                "require exactly",
            ),
            (
                {
                    "releases": [
                        {"tag": "../escape", "draft": False, "prerelease": False, "assets": []}
                    ]
                },
                "unsafe release tag",
            ),
            (
                {
                    "releases": [
                        {
                            "tag": "v0.1.0",
                            "draft": False,
                            "prerelease": False,
                            "assets": ["../escape"],
                        }
                    ]
                },
                "unsafe asset name",
            ),
            (
                {
                    "releases": [
                        {
                            "tag": "v0.1.0",
                            "draft": "no",
                            "prerelease": False,
                            "assets": [],
                        }
                    ]
                },
                "non-boolean",
            ),
        ):
            broken = copy.deepcopy(valid)
            broken.update(mutation)
            path.write_text(json.dumps(broken), encoding="utf-8")
            with self.assertRaisesRegex(UpgradeBaselineError, expected):
                load_release_index(path)


class DownloadedBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="glifistore-prior-bundle-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.repository = self.work / "repository"
        self.repository.mkdir()
        subprocess.run(
            ["git", "-c", "init.defaultBranch=main", "init", "-q", str(self.repository)],
            check=True,
            capture_output=True,
        )
        commit_version(self.repository, "0.1.0")
        git(self.repository, "tag", "-a", "v0.1.0", "-m", "v0.1.0")
        self.prior_sha = git(self.repository, "rev-parse", "refs/tags/v0.1.0^{commit}")
        self.head_sha = commit_version(self.repository, "0.2.0")
        self.baseline = resolve_baseline(
            self.repository,
            current=parse("0.2.0", allow_v_prefix=False),
            abi_major=1,
            index={"v0.1.0": published("v0.1.0")},
        )
        self.assertTrue(self.baseline["available"])

    def bundle(self, version: str = "0.1.0", git_sha: str | None = None) -> Path:
        directory = self.work / f"download-{version}-{git_sha or self.prior_sha}"
        return write_bundle(directory, version, git_sha or self.prior_sha)

    def test_the_downloaded_bundle_is_verified_by_the_digests_it_records(self) -> None:
        digests = verify_downloaded_bundle(
            self.baseline, self.bundle(), current_git_sha=self.head_sha
        )
        self.assertEqual(
            sorted(digests),
            ["abi_consumer_archive", "install_archive", "source_archive", "wire_client_archive"],
        )
        self.assertTrue(all(len(value) == 64 for value in digests.values()))

    def test_a_tampered_artifact_is_refused(self) -> None:
        directory = self.bundle()
        (directory / "glifistore-0.1.0-linux-amd64.tar.xz").write_bytes(b"rebuilt bytes")
        with self.assertRaisesRegex(BundleError, "mismatch"):
            verify_downloaded_bundle(self.baseline, directory)

    def test_a_bundle_for_another_release_is_refused(self) -> None:
        other = write_bundle(self.work / "other", "0.0.9", self.prior_sha)
        with self.assertRaisesRegex(UpgradeBaselineError, "not the selected baseline"):
            verify_downloaded_bundle(self.baseline, other)

    def test_a_bundle_whose_commit_disagrees_with_the_tag_is_refused(self) -> None:
        directory = self.bundle(git_sha="b" * 40)
        with self.assertRaisesRegex(UpgradeBaselineError, "annotated tag commit"):
            verify_downloaded_bundle(self.baseline, directory)

    def test_a_previous_release_rebuilt_from_head_is_refused(self) -> None:
        directory = self.bundle(git_sha=self.head_sha)
        with self.assertRaisesRegex(UpgradeBaselineError, "rebuilt from HEAD"):
            verify_downloaded_bundle(self.baseline, directory, current_git_sha=self.head_sha)

    def test_an_unavailable_baseline_can_never_be_verified(self) -> None:
        unavailable = resolve_baseline(
            self.repository, current=parse("0.2.0", allow_v_prefix=False), abi_major=1, index={}
        )
        with self.assertRaisesRegex(UpgradeBaselineError, "no admissible previous release"):
            verify_downloaded_bundle(unavailable, self.bundle())

    def test_admission_additionally_demands_the_full_release_policy(self) -> None:
        baseline_path = self.work / "upgrade-baseline.json"
        baseline_path.write_text(json.dumps(self.baseline), encoding="utf-8")
        context_path = self.work / "release-context.json"
        context_path.write_text(
            json.dumps(build_context(self.repository)), encoding="utf-8"
        )
        with self.assertRaises(BundleError):
            admit(
                baseline_path=baseline_path,
                directory=self.bundle(),
                repository=self.repository,
                context_path=context_path,
                output=self.work / "admission.json",
            )
        self.assertFalse((self.work / "admission.json").exists())

    def test_admission_refuses_a_baseline_resolved_for_another_version(self) -> None:
        baseline_path = self.work / "other-baseline.json"
        other = copy.deepcopy(self.baseline)
        other["product_version"] = "0.3.0"
        baseline_path.write_text(json.dumps(other), encoding="utf-8")
        context_path = self.work / "other-context.json"
        context_path.write_text(json.dumps(build_context(self.repository)), encoding="utf-8")
        with self.assertRaisesRegex(UpgradeBaselineError, "resolved for a different version"):
            admit(
                baseline_path=baseline_path,
                directory=self.bundle(),
                repository=self.repository,
                context_path=context_path,
                output=self.work / "other-admission.json",
            )


class CommandLineTests(unittest.TestCase):
    def test_resolve_writes_a_loadable_baseline_for_this_repository(self) -> None:
        with tempfile.TemporaryDirectory(prefix="glifistore-baseline-cli-") as temporary:
            work = Path(temporary)
            context = work / "release-context.json"
            self.assertEqual(
                subprocess.run(
                    [
                        sys.executable,
                        str(TOOLS / "generate_release_context.py"),
                        "--root",
                        str(ROOT),
                        "--output",
                        str(context),
                    ],
                    check=False,
                    capture_output=True,
                    text=True,
                ).returncode,
                0,
            )
            output = work / "upgrade-baseline.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(TOOLS / "upgrade_baseline.py"),
                    "resolve",
                    "--repository",
                    str(ROOT),
                    "--release-context",
                    str(context),
                    "--output",
                    str(output),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            baseline = validate_baseline(json.loads(output.read_text(encoding="utf-8")))
            self.assertEqual(baseline["product_version"], (ROOT / "VERSION").read_text().strip())


if __name__ == "__main__":
    unittest.main()
