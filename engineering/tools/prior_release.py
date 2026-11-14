#!/usr/bin/env python3
"""Select and validate an official prior GlifiStore release baseline."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

from release_bundle import (
    BundleError,
    read_json,
    validate_release_manifest,
    validate_release_policy,
    verify_checksums,
    verify_seal,
)
from semver_policy import SemverError, Version, select_previous
from semver_policy import parse as parse_semver


class PriorReleaseError(RuntimeError):
    pass


def version(value: str) -> Version:
    """SemVer ordering comes from the single policy module, never from a local regex."""
    try:
        return parse_semver(value)
    except SemverError as error:
        raise PriorReleaseError(f"invalid semantic version: {value}") from error


def git(repository: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise PriorReleaseError(f"cannot inspect release tags: {error}") from error
    if result.returncode != 0:
        raise PriorReleaseError(result.stderr.strip() or "git command failed")
    return result.stdout.strip()


def tag_identity(repository: Path, tag: str) -> tuple[str, str, str]:
    reference = f"refs/tags/{tag}"
    if git(repository, "cat-file", "-t", reference) != "tag":
        raise PriorReleaseError(f"prior release tag is not annotated: {tag}")
    commit = git(repository, "rev-parse", f"{reference}^{{commit}}")
    product_version = git(repository, "show", f"{reference}^{{commit}}:VERSION")
    abi_version = git(repository, "show", f"{reference}^{{commit}}:ABI_VERSION")
    if tag != f"v{product_version}":
        raise PriorReleaseError(f"tag/VERSION mismatch: {tag}")
    if re.fullmatch(r"[0-9]+\.[0-9]+", abi_version) is None:
        raise PriorReleaseError(f"invalid ABI_VERSION at {tag}")
    return commit, product_version, abi_version


def select(
    repository: Path,
    before_version: str,
    abi_major: int,
    *,
    include_prereleases: bool = False,
) -> str:
    """Greatest annotated ABI-compatible predecessor; never `before_version minus one`."""
    if repository.is_symlink() or not repository.is_dir():
        raise PriorReleaseError("repository must be a regular directory")
    repository = repository.resolve()
    before = version(before_version)
    listing = git(
        repository,
        "for-each-ref",
        "--format=%(refname:short) %(objecttype)",
        "refs/tags/v*",
    )
    candidates: dict[Version, str] = {}
    for line in listing.splitlines():
        tag, separator, object_type = line.partition(" ")
        if object_type != "tag":
            continue
        try:
            parsed = parse_semver(tag)
        except SemverError:
            continue
        if parsed >= before:
            continue
        if parsed.is_prerelease and not include_prereleases:
            continue
        _, _, abi = tag_identity(repository, tag)
        if int(abi.split(".", 1)[0]) == abi_major:
            candidates[parsed] = tag
    chosen = select_previous(candidates, before, include_prereleases=include_prereleases)
    if chosen is None:
        raise PriorReleaseError(
            f"no annotated prior release with ABI major {abi_major} exists before {before_version}"
        )
    selected = candidates[chosen]
    print(selected)
    return selected


def validate(
    directory: Path,
    repository: Path,
    tag: str,
    before_version: str,
    abi_major: int,
    architecture: str,
    output: Path,
    wire_version: int = 2,
) -> None:
    if directory.is_symlink() or not directory.is_dir():
        raise PriorReleaseError("prior release directory must be regular")
    directory = directory.resolve()
    verify_seal(directory, "verified-seal.json")
    verify_checksums(directory)
    validate_release_manifest(directory)
    validate_release_policy(directory)
    manifest = read_json(directory / "release-manifest.json")
    commit, product_version, abi_version = tag_identity(repository.resolve(), tag)
    if version(product_version) >= version(before_version):
        raise PriorReleaseError("prior release is not strictly older than candidate")
    if (
        manifest["tag"] != tag
        or manifest["product_version"] != product_version
        or manifest["git_sha"] != commit
        or manifest["abi_major"] != abi_major
        or int(abi_version.split(".", 1)[0]) != abi_major
    ):
        raise PriorReleaseError("prior release manifest disagrees with annotated tag or ABI")
    install_pattern = re.compile(
        rf"^glifistore-{re.escape(product_version)}-linux-{re.escape(architecture)}\.tar\.xz$"
    )
    consumer_pattern = re.compile(
        rf"^glifistore-abi-v{abi_major}-consumer-{re.escape(product_version)}-linux-"
        rf"{re.escape(architecture)}\.tar\.xz$"
    )
    wire_client_pattern = re.compile(
        rf"^glifistore-wire-v{wire_version}-client-{re.escape(product_version)}-linux-"
        rf"{re.escape(architecture)}\.tar\.xz$"
    )
    names = [entry["name"] for entry in manifest["artifacts"]]
    installs = [name for name in names if install_pattern.fullmatch(name)]
    consumers = [name for name in names if consumer_pattern.fullmatch(name)]
    wire_clients = [name for name in names if wire_client_pattern.fullmatch(name)]
    if len(installs) != 1 or len(consumers) != 1 or len(wire_clients) != 1:
        raise PriorReleaseError(
            "prior release needs one Linux prefix, ABI consumer, and wire client: "
            f"{installs}, {consumers}, {wire_clients}"
        )
    if f"{consumers[0]}.spdx.json" not in names:
        raise PriorReleaseError("prior ABI consumer archive has no bound SPDX SBOM")
    if f"{wire_clients[0]}.spdx.json" not in names:
        raise PriorReleaseError("prior wire client archive has no bound SPDX SBOM")
    value = {
        "schema_version": 1,
        "tag": tag,
        "product_version": product_version,
        "git_sha": commit,
        "abi_version": abi_version,
        "install_archive": installs[0],
        "abi_consumer_archive": consumers[0],
        "wire_client_archive": wire_clients[0],
    }
    if output.exists() or output.is_symlink():
        raise PriorReleaseError(f"refusing to replace output: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)
    selection = commands.add_parser("select")
    selection.add_argument("--repository", type=Path, required=True)
    selection.add_argument("--before-version", required=True)
    selection.add_argument("--abi-major", type=int, required=True)
    selection.add_argument("--include-prereleases", action="store_true")
    validation = commands.add_parser("validate")
    validation.add_argument("--directory", type=Path, required=True)
    validation.add_argument("--repository", type=Path, required=True)
    validation.add_argument("--tag", required=True)
    validation.add_argument("--before-version", required=True)
    validation.add_argument("--abi-major", type=int, required=True)
    validation.add_argument("--architecture", required=True)
    validation.add_argument("--wire-version", type=int, default=2)
    validation.add_argument("--output", type=Path, required=True)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        if args.command == "select":
            select(
                args.repository,
                args.before_version,
                args.abi_major,
                include_prereleases=args.include_prereleases,
            )
        else:
            validate(
                args.directory,
                args.repository,
                args.tag,
                args.before_version,
                args.abi_major,
                args.architecture,
                args.output,
                args.wire_version,
            )
    except (
        BundleError,
        KeyError,
        OSError,
        PriorReleaseError,
        SemverError,
        TypeError,
        ValueError,
    ) as error:
        print(f"prior release FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
