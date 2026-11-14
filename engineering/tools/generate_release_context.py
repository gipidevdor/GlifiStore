#!/usr/bin/env python3
"""Derive the release context that drives GlifiStore packaging CI.

The context is the only place where VERSION, ABI_VERSION, the git identity, the
package revision and the SemVer-aware previous release are resolved. Every
backend, workflow and evidence document reads it instead of hard-coding a
version number.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.package_framework import (
    GIT_OBJECT_ID,
    PackageFrameworkError,
    is_utc_timestamp,
    read_json,
    utc_now,
    validate_against_schema,
    write_json,
)
from engineering.tools.semver_policy import (
    SemverError,
    Version,
    normalize_all,
    parse,
    release_kind,
    select_previous,
)


SCHEMA_NAME = "release-context.schema.json"


class ReleaseContextError(RuntimeError):
    pass


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
        raise ReleaseContextError(f"cannot run git: {error}") from error
    if completed.returncode != 0:
        raise ReleaseContextError(completed.stderr.strip() or "git command failed")
    return completed.stdout.strip()


def _git_object_type(root: Path, reference: str) -> str | None:
    try:
        return _git(root, "cat-file", "-t", reference)
    except ReleaseContextError:
        return None


def _authority(root: Path, name: str) -> str:
    path = root / name
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise ReleaseContextError(f"cannot read {name}: {error}") from error
    if not value or "\n" in value or "\r" in value:
        raise ReleaseContextError(f"{name} must contain exactly one non-empty line")
    return value


def annotated_release_tags(root: Path) -> dict[Version, str]:
    """Annotated v<semver> tags only; lightweight tags are not release authorities."""
    listing = _git(root, "for-each-ref", "--format=%(refname:short) %(objecttype)", "refs/tags/v*")
    tags: dict[Version, str] = {}
    for line in listing.splitlines():
        name, _, object_type = line.partition(" ")
        if object_type != "tag":
            continue
        try:
            version = parse(name)
        except SemverError:
            continue
        tags[version] = name
    return tags


def previous_release(
    root: Path, current: Version, *, include_prereleases: bool = False
) -> dict[str, Any]:
    tags = annotated_release_tags(root)
    selected = select_previous(tags, current, include_prereleases=include_prereleases)
    if selected is None:
        return {
            "available": False,
            "version": None,
            "tag": None,
            "git_sha": None,
            "abi_major": None,
            "reason": f"no annotated release tag precedes {current}",
        }
    tag = tags[selected]
    reference = f"refs/tags/{tag}"
    commit = _git(root, "rev-parse", f"{reference}^{{commit}}")
    tagged_version = _git(root, "show", f"{reference}^{{commit}}:VERSION").strip()
    tagged_abi = _git(root, "show", f"{reference}^{{commit}}:ABI_VERSION").strip()
    if tag != f"v{tagged_version}":
        raise ReleaseContextError(f"prior tag disagrees with its VERSION authority: {tag}")
    try:
        abi_major = int(tagged_abi.split(".", 1)[0])
    except ValueError as error:
        raise ReleaseContextError(f"prior tag has an invalid ABI_VERSION: {tag}") from error
    return {
        "available": True,
        "version": str(selected),
        "tag": tag,
        "git_sha": commit,
        "abi_major": abi_major,
        "reason": f"greatest annotated release strictly older than {current}",
    }


def build_context(
    root: Path,
    *,
    package_revision: int = 0,
    commit: str | None = None,
    include_prereleases: bool = False,
    require_clean: bool = False,
) -> dict[str, Any]:
    root = root.resolve()
    if type(package_revision) is not int or package_revision < 0:
        raise ReleaseContextError("package revision must be a non-negative integer")
    product = _authority(root, "VERSION")
    abi = _authority(root, "ABI_VERSION")
    try:
        version = parse(product, allow_v_prefix=False)
    except SemverError as error:
        raise ReleaseContextError(f"VERSION authority is invalid: {error}") from error
    abi_parts = abi.split(".")
    if len(abi_parts) != 2 or not all(part.isdigit() for part in abi_parts):
        raise ReleaseContextError("ABI_VERSION is not strict major.minor")

    head = commit or _git(root, "rev-parse", "HEAD")
    if GIT_OBJECT_ID.fullmatch(head) is None:
        raise ReleaseContextError(f"invalid git commit identity: {head}")
    tree_clean = not _git(root, "status", "--porcelain", "--untracked-files=no")
    if require_clean and not tree_clean:
        raise ReleaseContextError("tracked working tree changes are forbidden in this profile")

    tag = f"v{product}"
    tag_is_annotated = _git_object_type(root, f"refs/tags/{tag}") == "tag"
    tag_matches_commit = (
        tag_is_annotated and _git(root, "rev-parse", f"refs/tags/{tag}^{{commit}}") == head
    )

    previous = previous_release(root, version, include_prereleases=include_prereleases)
    baseline = previous["version"] if previous["available"] else None

    limitations: list[str] = []
    if not tag_is_annotated:
        limitations.append(
            f"annotated tag {tag} does not exist yet; this context describes an untagged commit"
        )
    elif not tag_matches_commit:
        limitations.append(f"annotated tag {tag} does not point at the described commit")
    if not tree_clean:
        limitations.append("the working tree has tracked modifications; artifacts are not sealed")
    if not previous["available"]:
        limitations.append(
            "no previous release exists; upgrade evidence can only be "
            "NOT_APPLICABLE_INITIAL_BASELINE"
        )

    try:
        packages = {
            backend: package.as_dict()
            for backend, package in normalize_all(version, package_revision).items()
        }
        kind = release_kind(version, baseline)
    except SemverError as error:
        raise ReleaseContextError(str(error)) from error

    context = {
        "schema_version": 1,
        "generated_at": utc_now(),
        "product_version": product,
        "semver": version.as_dict(),
        "abi": {
            "major": int(abi_parts[0]),
            "minor": int(abi_parts[1]),
            "version": abi,
        },
        "git": {
            "commit": head,
            "tag": tag if tag_is_annotated else None,
            "tag_is_annotated": tag_is_annotated,
            "tag_matches_commit": tag_matches_commit,
            "tree_clean": tree_clean,
        },
        "package_revision": package_revision,
        "release_kind": kind,
        "previous": previous,
        "package_versions": packages,
        "limitations": limitations,
    }
    validate_release_context(context)
    return context


def validate_release_context(context: dict[str, Any]) -> dict[str, Any]:
    validate_against_schema(context, SCHEMA_NAME, "release context")
    if not is_utc_timestamp(context["generated_at"]):
        raise ReleaseContextError("release context timestamp is not an ISO-8601 UTC instant")
    semver = context["semver"]
    if semver["text"] != context["product_version"]:
        raise ReleaseContextError("release context SemVer text disagrees with the product version")
    previous = context["previous"]
    identity = ("version", "tag", "git_sha", "abi_major")
    if previous["available"]:
        if any(previous[part] is None for part in identity):
            raise ReleaseContextError("available previous release misses part of its identity")
        if previous["tag"] != f"v{previous['version']}":
            raise ReleaseContextError("previous release tag disagrees with its version")
        if context["release_kind"] == "initial":
            raise ReleaseContextError("a previous release exists, so the kind cannot be initial")
    else:
        if any(previous[part] is not None for part in identity):
            raise ReleaseContextError("unavailable previous release must not carry an identity")
        if context["release_kind"] != "initial":
            raise ReleaseContextError("without a previous release the kind must be initial")
    for backend, package in context["package_versions"].items():
        if package["backend"] != backend:
            raise ReleaseContextError(f"package version key disagrees with its backend: {backend}")
    return context


def load_release_context(path: Path) -> dict[str, Any]:
    return validate_release_context(read_json(path))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--package-revision", type=int, default=0)
    parser.add_argument("--commit")
    parser.add_argument("--include-prereleases", action="store_true")
    parser.add_argument("--require-clean", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--replace", action="store_true")
    arguments = parser.parse_args()
    try:
        context = build_context(
            arguments.root,
            package_revision=arguments.package_revision,
            commit=arguments.commit,
            include_prereleases=arguments.include_prereleases,
            require_clean=arguments.require_clean,
        )
        if arguments.output is None:
            print(json.dumps(context, indent=2, sort_keys=True))
        else:
            write_json(arguments.output, context, replace=arguments.replace)
            print(arguments.output)
    except (PackageFrameworkError, ReleaseContextError, OSError) as error:
        print(f"release context FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
