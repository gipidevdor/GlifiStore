#!/usr/bin/env python3
"""Resolve and admit the sealed N-1 upgrade baseline for GlifiStore packaging.

A package upgrade may only be proven against the bytes of a release that was
actually published and sealed, so this module never assumes that the baseline is
"the current version minus one". It orders every release tag by SemVer
precedence, records why each candidate was accepted or rejected, and keeps the
greatest candidate that is annotated, published, complete, sealed and
ABI-compatible.

The distinction between the three honest pre-upgrade states is the point of the
tool: nothing precedes the current version is an initial baseline, something
precedes it but no sealed release is admissible is BLOCKED, and an admissible
baseline that this run did not exercise is NOT_RUN. A first release therefore
cannot be faked by hiding a draft or incomplete predecessor.

Admission re-verifies the downloaded bundle against the digests recorded in the
prior release's own manifest and refuses a bundle that carries the current
commit, so N-1 can never be rebuilt from HEAD.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:  # release_bundle and prior_release are flat siblings
    sys.path.insert(0, str(_TOOLS))
if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(_TOOLS.parents[1]))

from prior_release import PriorReleaseError  # noqa: E402
from prior_release import validate as validate_prior_release  # noqa: E402
from release_bundle import (  # noqa: E402
    BundleError,
    validate_release_manifest,
    verify_checksums,
    verify_seal,
)

from engineering.tools.generate_release_context import (  # noqa: E402
    ReleaseContextError,
    load_release_context,
)
from engineering.tools.package_framework import (  # noqa: E402
    SAFE_NAME,
    PackageFrameworkError,
    is_utc_timestamp,
    read_json,
    utc_now,
    validate_against_schema,
    write_json,
)
from engineering.tools.semver_policy import SemverError, Version, parse  # noqa: E402


SCHEMA_NAME = "upgrade-baseline.schema.json"
ADMISSION_SCHEMA_NAME = "upgrade-baseline-admission.schema.json"
DEFAULT_WIRE_VERSION = 2
MANIFEST_ASSET = "release-manifest.json"
SEAL_ASSET = "verified-seal.json"
CHECKSUMS_ASSET = "SHA256SUMS"
PROVENANCE_ASSET = "verified-seal.sigstore.json"
BUILD_METADATA_ASSET = "build-metadata.json"
CONTROL_ASSETS = (MANIFEST_ASSET, SEAL_ASSET, CHECKSUMS_ASSET, BUILD_METADATA_ASSET)
RELEASE_FIELDS = {"tag", "draft", "prerelease", "assets"}
INSTALL_ARCHIVE = re.compile(r"^glifistore-(?P<version>.+)-linux-(?P<arch>[A-Za-z0-9_-]+)\.tar\.xz$")


class UpgradeBaselineError(RuntimeError):
    pass


class _IncompleteAssets(RuntimeError):
    """A published release whose asset set cannot support an upgrade proof."""


@dataclass(frozen=True)
class PublishedRelease:
    """What the forge says about a published release; assets are names only."""

    tag: str
    draft: bool
    prerelease: bool
    assets: tuple[str, ...]


def _git(root: Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(root), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise UpgradeBaselineError(f"cannot run git: {error}") from error
    if completed.returncode != 0:
        raise UpgradeBaselineError(completed.stderr.strip() or "git command failed")
    return completed.stdout.strip()


def _tag_objects(root: Path) -> dict[str, str]:
    """Every v* tag with its object type, so a lightweight tag is refused by name."""
    listing = _git(root, "for-each-ref", "--format=%(refname:short) %(objecttype)", "refs/tags/v*")
    objects: dict[str, str] = {}
    for line in listing.splitlines():
        name, _, object_type = line.partition(" ")
        if name:
            objects[name] = object_type
    return objects


def load_release_index(path: Path) -> dict[str, PublishedRelease]:
    """Published-release facts, as reported by the forge and retained as an input."""
    if path.is_symlink() or not path.is_file():
        raise UpgradeBaselineError(f"release index is missing or not regular: {path}")
    value = read_json(path)
    if value.get("schema_version") != 1:
        raise UpgradeBaselineError("unsupported release index schema version")
    entries = value.get("releases")
    if not isinstance(entries, list):
        raise UpgradeBaselineError("release index must carry a releases array")
    index: dict[str, PublishedRelease] = {}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != RELEASE_FIELDS:
            raise UpgradeBaselineError(
                f"release index entries require exactly {sorted(RELEASE_FIELDS)}"
            )
        tag = entry["tag"]
        if not isinstance(tag, str) or SAFE_NAME.fullmatch(tag) is None:
            raise UpgradeBaselineError(f"unsafe release tag in the index: {tag!r}")
        if tag in index:
            raise UpgradeBaselineError(f"release index repeats tag {tag}")
        if not isinstance(entry["draft"], bool) or not isinstance(entry["prerelease"], bool):
            raise UpgradeBaselineError(f"release {tag} has non-boolean draft/prerelease flags")
        assets = entry["assets"]
        if not isinstance(assets, list) or any(
            not isinstance(name, str) or SAFE_NAME.fullmatch(name) is None for name in assets
        ):
            raise UpgradeBaselineError(f"release {tag} carries an unsafe asset name")
        if len(set(assets)) != len(assets):
            raise UpgradeBaselineError(f"release {tag} repeats an asset name")
        index[tag] = PublishedRelease(
            tag=tag,
            draft=entry["draft"],
            prerelease=entry["prerelease"],
            assets=tuple(sorted(assets)),
        )
    return index


def _unique(pattern: re.Pattern[str], assets: tuple[str, ...], role: str) -> str:
    matches = sorted(name for name in assets if pattern.fullmatch(name))
    if len(matches) != 1:
        raise _IncompleteAssets(f"expected exactly one {role} asset, found {matches}")
    return matches[0]


def resolve_assets(
    version: str, abi_major: int, wire_version: int, assets: tuple[str, ...]
) -> dict[str, str | None]:
    """The exact asset names an upgrade needs, or a refusal naming what is absent."""
    missing = [name for name in CONTROL_ASSETS if name not in assets]
    source = f"GlifiStore-{version}.tar.xz"
    if source not in assets:
        missing.append(source)
    if missing:
        raise _IncompleteAssets(f"missing required assets: {sorted(missing)}")
    install = _unique(
        re.compile(rf"^glifistore-{re.escape(version)}-linux-[A-Za-z0-9_-]+\.tar\.xz$"),
        assets,
        "Linux install prefix",
    )
    consumer = _unique(
        re.compile(
            rf"^glifistore-abi-v{abi_major}-consumer-{re.escape(version)}-linux-"
            r"[A-Za-z0-9_-]+\.tar\.xz$"
        ),
        assets,
        "ABI consumer",
    )
    client = _unique(
        re.compile(
            rf"^glifistore-wire-v{wire_version}-client-{re.escape(version)}-linux-"
            r"[A-Za-z0-9_-]+\.tar\.xz$"
        ),
        assets,
        "wire client",
    )
    unbound = [name for name in (consumer, client) if f"{name}.spdx.json" not in assets]
    if unbound:
        raise _IncompleteAssets(f"fixture archives without a bound SPDX SBOM: {sorted(unbound)}")
    return {
        "manifest": MANIFEST_ASSET,
        "seal": SEAL_ASSET,
        "checksums": CHECKSUMS_ASSET,
        "provenance": PROVENANCE_ASSET if PROVENANCE_ASSET in assets else None,
        "source_archive": source,
        "install_archive": install,
        "abi_consumer_archive": consumer,
        "wire_client_archive": client,
    }


def _tag_identity(root: Path, tag: str) -> tuple[str, str, str]:
    reference = f"refs/tags/{tag}"
    commit = _git(root, "rev-parse", f"{reference}^{{commit}}")
    try:
        product = _git(root, "show", f"{reference}^{{commit}}:VERSION").strip()
        abi = _git(root, "show", f"{reference}^{{commit}}:ABI_VERSION").strip()
    except UpgradeBaselineError as error:
        raise _IncompleteAssets(f"the tagged tree has no version authority: {error}") from error
    return commit, product, abi


def resolve_baseline(
    root: Path,
    *,
    current: Version,
    abi_major: int,
    wire_version: int = DEFAULT_WIRE_VERSION,
    index: dict[str, PublishedRelease] | None = None,
    include_prereleases: bool = False,
) -> dict[str, Any]:
    if root.is_symlink() or not root.is_dir():
        raise UpgradeBaselineError("repository must be a regular directory")
    root = root.resolve()
    if type(abi_major) is not int or abi_major < 0:
        raise UpgradeBaselineError("ABI major must be a non-negative integer")
    if type(wire_version) is not int or wire_version < 1:
        raise UpgradeBaselineError("wire version must be a positive integer")

    tags = _tag_objects(root)
    published = index or {}
    rejected: list[dict[str, Any]] = []
    admissible: list[tuple[Version, str, str, int, dict[str, str | None]]] = []
    precedes = False

    def reject(tag: str, version: Version | None, reason: str, detail: str) -> None:
        rejected.append(
            {
                "tag": tag,
                "version": None if version is None else str(version),
                "reason": reason,
                "detail": detail,
            }
        )

    for tag in sorted(set(tags) | set(published)):
        try:
            version = parse(tag)
        except SemverError as error:
            reject(tag, None, "not_semver", str(error))
            continue
        if version >= current:
            reject(tag, version, "not_older", f"{version} does not precede {current}")
            continue
        # Anything older than the current version, admissible or not, forbids the
        # initial-baseline claim later on.
        precedes = True
        if version.is_prerelease and not include_prereleases:
            reject(
                tag,
                version,
                "prerelease_excluded",
                "a SemVer prerelease is not an upgrade baseline unless prereleases are requested",
            )
            continue
        if tags.get(tag) != "tag":
            reject(
                tag,
                version,
                "tag_not_annotated",
                "no annotated tag of this name exists in the repository, so its identity is unproven",
            )
            continue
        try:
            commit, product, abi = _tag_identity(root, tag)
        except _IncompleteAssets as error:
            reject(tag, version, "tag_version_mismatch", str(error))
            continue
        if tag != f"v{product}":
            reject(
                tag,
                version,
                "tag_version_mismatch",
                f"the tagged VERSION authority says {product}",
            )
            continue
        if re.fullmatch(r"[0-9]+\.[0-9]+", abi) is None:
            reject(tag, version, "tag_abi_invalid", f"the tagged ABI_VERSION says {abi!r}")
            continue
        tagged_abi_major = int(abi.split(".", 1)[0])
        if tagged_abi_major != abi_major:
            reject(
                tag,
                version,
                "incompatible_abi",
                f"ABI major {tagged_abi_major} cannot be an upgrade baseline for ABI major {abi_major}",
            )
            continue
        release = published.get(tag)
        if release is None:
            reject(
                tag,
                version,
                "no_published_release",
                "no published release carries this tag, so no sealed bytes can be downloaded",
            )
            continue
        if release.draft:
            reject(tag, version, "draft_release", "the release is still a draft")
            continue
        if version.is_prerelease and not release.prerelease:
            reject(
                tag,
                version,
                "prerelease_published_as_stable",
                "the tag is a SemVer prerelease but the release is published as stable",
            )
            continue
        if not version.is_prerelease and release.prerelease:
            reject(
                tag,
                version,
                "stable_published_as_prerelease",
                "the tag is a stable SemVer version but the release is published as a prerelease",
            )
            continue
        try:
            assets = resolve_assets(product, abi_major, wire_version, release.assets)
        except _IncompleteAssets as error:
            reject(tag, version, "incomplete_assets", str(error))
            continue
        admissible.append((version, tag, commit, tagged_abi_major, assets))

    limitations: list[str] = []
    if index is None and precedes:
        limitations.append(
            "no published-release index was supplied, so no predecessor could be proven to have "
            "sealed assets and every one of them was refused"
        )
    if include_prereleases:
        limitations.append(
            "prereleases were admitted as upgrade baselines on request; a prerelease is not a "
            "supported upgrade source for operators"
        )

    admissible.sort(key=lambda entry: entry[0].precedence())
    if admissible:
        version, tag, commit, tagged_abi_major, assets = admissible[-1]
        for superseded in admissible[:-1]:
            reject(
                superseded[1],
                superseded[0],
                "superseded",
                f"admissible but older than the selected baseline {tag}",
            )
        selected: dict[str, Any] | None = {
            "version": str(version),
            "tag": tag,
            "git_sha": commit,
            "abi_major": tagged_abi_major,
            "assets": assets,
        }
        status = "NOT_RUN"
        reason = (
            f"greatest published, complete and ABI-v{abi_major} compatible release strictly "
            f"older than {current}"
        )
        if assets["provenance"] is None:
            limitations.append(
                f"{tag} publishes no {PROVENANCE_ASSET} asset, so its provenance bundle cannot be "
                "re-verified during admission"
            )
    else:
        selected = None
        if precedes:
            refused = [
                item
                for item in rejected
                if item["reason"] not in ("not_semver", "not_older")
            ]
            status = "BLOCKED"
            reason = (
                f"{len(refused)} tag(s) precede {current} but none is an admissible sealed release"
            )
        else:
            status = "NOT_APPLICABLE_INITIAL_BASELINE"
            reason = f"no release precedes {current}, so this version is the initial baseline"
            limitations.append(
                "this is the initial baseline: no N-1 upgrade proof exists or can exist for it"
            )

    baseline = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "product_version": str(current),
        "abi_major": abi_major,
        "wire_version": wire_version,
        "include_prereleases": include_prereleases,
        "available": selected is not None,
        "upgrade_check_status": status,
        "reason": reason,
        "selected": selected,
        "rejected": sorted(rejected, key=lambda item: (item["tag"], item["reason"])),
        "limitations": limitations,
    }
    return validate_baseline(baseline)


def validate_baseline(baseline: dict[str, Any]) -> dict[str, Any]:
    validate_against_schema(baseline, SCHEMA_NAME, "upgrade baseline")
    if not is_utc_timestamp(baseline["generated_at"]):
        raise UpgradeBaselineError("upgrade baseline timestamp is not an ISO-8601 UTC instant")
    selected = baseline["selected"]
    if baseline["available"] != (selected is not None):
        raise UpgradeBaselineError("upgrade baseline availability disagrees with its selection")
    # A predecessor that was considered and refused is recorded with a reason that
    # is neither not_semver nor not_older, and its presence forbids the
    # initial-baseline claim. This is what makes the first-release state honest.
    refused = [
        entry
        for entry in baseline["rejected"]
        if entry["reason"] not in ("not_semver", "not_older")
    ]
    if selected is None:
        if baseline["upgrade_check_status"] == "NOT_RUN":
            raise UpgradeBaselineError("NOT_RUN requires an admissible baseline")
        if baseline["upgrade_check_status"] == "NOT_APPLICABLE_INITIAL_BASELINE" and refused:
            raise UpgradeBaselineError(
                "an initial baseline cannot coexist with refused predecessors: "
                + ", ".join(sorted(entry["tag"] for entry in refused))
            )
        if baseline["upgrade_check_status"] == "BLOCKED" and not refused:
            raise UpgradeBaselineError("BLOCKED requires at least one refused predecessor")
        return baseline
    if baseline["upgrade_check_status"] != "NOT_RUN":
        raise UpgradeBaselineError(
            "an admissible baseline may only report NOT_RUN before the upgrade runs"
        )
    if selected["tag"] != f"v{selected['version']}":
        raise UpgradeBaselineError("selected baseline tag disagrees with its version")
    if parse(selected["version"]) >= parse(baseline["product_version"], allow_v_prefix=False):
        raise UpgradeBaselineError("selected baseline does not precede the current version")
    if selected["abi_major"] != baseline["abi_major"]:
        raise UpgradeBaselineError("selected baseline ABI major disagrees with the requested one")
    return baseline


def load_baseline(path: Path) -> dict[str, Any]:
    return validate_baseline(read_json(path))


def verify_downloaded_bundle(
    baseline: dict[str, Any], directory: Path, *, current_git_sha: str | None = None
) -> dict[str, str]:
    """Bind downloaded bytes to the digests the prior release recorded for itself."""
    validate_baseline(baseline)
    selected = baseline["selected"]
    if selected is None:
        raise UpgradeBaselineError(
            f"no admissible previous release to verify: {baseline['reason']}"
        )
    if directory.is_symlink() or not directory.is_dir():
        raise UpgradeBaselineError("previous release directory must be regular")
    directory = directory.resolve()
    verify_seal(directory, SEAL_ASSET)
    verify_checksums(directory)
    validate_release_manifest(directory)
    manifest = read_json(directory / MANIFEST_ASSET)
    if manifest["tag"] != selected["tag"] or manifest["product_version"] != selected["version"]:
        raise UpgradeBaselineError(
            f"the downloaded bundle is {manifest['tag']}, not the selected baseline "
            f"{selected['tag']}"
        )
    # Diagnosed before the tag comparison: a bundle built at the current commit is
    # a local rebuild of N-1 whatever else it agrees with.
    if current_git_sha is not None and manifest["git_sha"] == current_git_sha:
        raise UpgradeBaselineError(
            "the previous release bundle carries the current commit: N-1 was rebuilt from HEAD "
            "instead of downloaded"
        )
    if manifest["git_sha"] != selected["git_sha"]:
        raise UpgradeBaselineError(
            "the downloaded bundle disagrees with the annotated tag commit of the baseline"
        )
    if manifest["abi_major"] != selected["abi_major"]:
        raise UpgradeBaselineError("the downloaded bundle has a different ABI major")
    recorded = {entry["name"]: entry["sha256"] for entry in manifest["artifacts"]}
    digests: dict[str, str] = {}
    for role in ("source_archive", "install_archive", "abi_consumer_archive", "wire_client_archive"):
        name = selected["assets"][role]
        if name not in recorded:
            raise UpgradeBaselineError(
                f"the previous release manifest does not record the {role} {name}"
            )
        digests[role] = recorded[name]
    return digests


def admit(
    *,
    baseline_path: Path,
    directory: Path,
    repository: Path,
    context_path: Path,
    output: Path,
    replace: bool = False,
) -> dict[str, Any]:
    """Full admission: byte binding, release policy and annotated-tag identity."""
    baseline = load_baseline(baseline_path)
    context = load_release_context(context_path)
    selected = baseline["selected"]
    if selected is None:
        raise UpgradeBaselineError(
            f"refusing to admit an unavailable previous release: {baseline['reason']}"
        )
    if context["product_version"] != baseline["product_version"]:
        raise UpgradeBaselineError("the upgrade baseline was resolved for a different version")
    digests = verify_downloaded_bundle(
        baseline, directory, current_git_sha=context["git"]["commit"]
    )
    install = INSTALL_ARCHIVE.fullmatch(selected["assets"]["install_archive"])
    if install is None or install.group("version") != selected["version"]:
        raise UpgradeBaselineError(
            f"cannot derive the architecture from {selected['assets']['install_archive']}"
        )
    architecture = install.group("arch")
    with tempfile.TemporaryDirectory(prefix="glifistore-prior-release-") as temporary:
        prior = Path(temporary) / "prior-release.json"
        validate_prior_release(
            directory,
            repository,
            selected["tag"],
            baseline["product_version"],
            baseline["abi_major"],
            architecture,
            prior,
            baseline["wire_version"],
        )
        admitted = read_json(prior)
    receipt = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "current_version": baseline["product_version"],
        "tag": selected["tag"],
        "product_version": selected["version"],
        "git_sha": selected["git_sha"],
        "abi_version": admitted["abi_version"],
        "architecture": architecture,
        "artifacts": {
            role: {"name": selected["assets"][role], "sha256": digest}
            for role, digest in sorted(digests.items())
        },
        "verified_by": [
            "verified-seal.json",
            "SHA256SUMS",
            "release-manifest.json",
            "release-policy",
            "annotated-tag-identity",
        ],
        "limitations": list(baseline["limitations"]),
    }
    validate_against_schema(receipt, ADMISSION_SCHEMA_NAME, "upgrade baseline admission")
    write_json(output, receipt, replace=replace)
    return receipt


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    resolve = commands.add_parser("resolve")
    resolve.add_argument("--repository", type=Path, required=True)
    resolve.add_argument("--release-context", type=Path, required=True)
    resolve.add_argument("--release-index", type=Path)
    resolve.add_argument("--wire-version", type=int, default=DEFAULT_WIRE_VERSION)
    resolve.add_argument("--include-prereleases", action="store_true")
    resolve.add_argument("--output", type=Path)
    resolve.add_argument("--replace", action="store_true")

    validate = commands.add_parser("validate")
    validate.add_argument("--baseline", type=Path, required=True)

    admission = commands.add_parser("admit")
    admission.add_argument("--baseline", type=Path, required=True)
    admission.add_argument("--directory", type=Path, required=True)
    admission.add_argument("--repository", type=Path, required=True)
    admission.add_argument("--release-context", type=Path, required=True)
    admission.add_argument("--output", type=Path, required=True)
    admission.add_argument("--replace", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "resolve":
            context = load_release_context(arguments.release_context)
            baseline = resolve_baseline(
                arguments.repository,
                current=parse(context["product_version"], allow_v_prefix=False),
                abi_major=context["abi"]["major"],
                wire_version=arguments.wire_version,
                index=(
                    None
                    if arguments.release_index is None
                    else load_release_index(arguments.release_index)
                ),
                include_prereleases=arguments.include_prereleases,
            )
            if arguments.output is None:
                print(json.dumps(baseline, indent=2, sort_keys=True))
            else:
                write_json(arguments.output, baseline, replace=arguments.replace)
                print(arguments.output)
        elif arguments.command == "validate":
            baseline = load_baseline(arguments.baseline)
            print(
                f"upgrade baseline OK (available={str(baseline['available']).lower()}, "
                f"status={baseline['upgrade_check_status']})"
            )
        else:
            admit(
                baseline_path=arguments.baseline,
                directory=arguments.directory,
                repository=arguments.repository,
                context_path=arguments.release_context,
                output=arguments.output,
                replace=arguments.replace,
            )
            print(arguments.output)
    except (
        BundleError,
        KeyError,
        OSError,
        PackageFrameworkError,
        PriorReleaseError,
        ReleaseContextError,
        SemverError,
        TypeError,
        UpgradeBaselineError,
        ValueError,
    ) as error:
        print(f"upgrade baseline FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
