#!/usr/bin/env python3
"""Validate the immutable source identity used by a GlifiStore release."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


PRODUCT_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")
ABI_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$")


class ReleaseIdentityError(RuntimeError):
    pass


@dataclass(frozen=True)
class ReleaseIdentity:
    product_version: str
    abi_major: int
    abi_minor: int
    tag: str
    git_sha: str
    source_date_epoch: int


def _read_authority(root: Path, name: str) -> str:
    path = root / name
    try:
        value = path.read_text(encoding="utf-8").strip()
    except OSError as error:
        raise ReleaseIdentityError(f"cannot read {name}: {error}") from error
    if not value or "\n" in value or "\r" in value:
        raise ReleaseIdentityError(f"{name} must contain exactly one non-empty line")
    return value


def _git(root: Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or "git command failed"
        raise ReleaseIdentityError(detail)
    return completed.stdout.strip()


def validate_release_identity(
    root: Path,
    tag: str,
    *,
    expected_sha: str | None = None,
    require_clean: bool = True,
) -> ReleaseIdentity:
    root = root.resolve()
    product = _read_authority(root, "VERSION")
    product_match = PRODUCT_RE.fullmatch(product)
    if product_match is None:
        raise ReleaseIdentityError("VERSION is not strict X.Y.Z SemVer")
    abi = _read_authority(root, "ABI_VERSION")
    abi_match = ABI_RE.fullmatch(abi)
    if abi_match is None:
        raise ReleaseIdentityError("ABI_VERSION is not strict major.minor")
    expected_tag = f"v{product}"
    if tag != expected_tag:
        raise ReleaseIdentityError(f"tag/version mismatch: tag={tag!r}, expected={expected_tag!r}")

    tag_ref = f"refs/tags/{tag}"
    if _git(root, "cat-file", "-t", tag_ref) != "tag":
        raise ReleaseIdentityError(f"release tag {tag!r} must be an annotated tag object")
    tag_sha = _git(root, "rev-list", "-n", "1", tag_ref)
    head_sha = _git(root, "rev-parse", "HEAD")
    if head_sha != tag_sha:
        raise ReleaseIdentityError(
            f"checkout is not the release tag commit: HEAD={head_sha}, tag={tag_sha}"
        )
    if expected_sha is not None and expected_sha != tag_sha:
        raise ReleaseIdentityError(
            f"requested git SHA does not match tag target: requested={expected_sha}, tag={tag_sha}"
        )
    if require_clean:
        if _git(root, "status", "--porcelain", "--untracked-files=no"):
            raise ReleaseIdentityError("tracked working tree changes are forbidden in a release build")

    epoch_text = _git(root, "show", "-s", "--format=%ct", tag_sha)
    try:
        epoch = int(epoch_text)
    except ValueError as error:
        raise ReleaseIdentityError("tag target has an invalid commit timestamp") from error
    if epoch <= 0:
        raise ReleaseIdentityError("tag target commit timestamp must be positive")

    return ReleaseIdentity(
        product_version=product,
        abi_major=int(abi_match.group(1)),
        abi_minor=int(abi_match.group(2)),
        tag=tag,
        git_sha=tag_sha,
        source_date_epoch=epoch,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--tag", required=True)
    parser.add_argument("--git-sha")
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    try:
        identity = validate_release_identity(
            arguments.root,
            arguments.tag,
            expected_sha=arguments.git_sha,
            require_clean=not arguments.allow_dirty,
        )
    except ReleaseIdentityError as error:
        print(f"release identity FAILED: {error}", file=sys.stderr)
        return 1
    encoded = json.dumps(asdict(identity), indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
