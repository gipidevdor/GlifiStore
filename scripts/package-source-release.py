#!/usr/bin/env python3
"""Create a deterministic source archive from one exact annotated release tag."""

from __future__ import annotations

import argparse
import hashlib
import lzma
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engineering" / "tools"))

from release_identity import ReleaseIdentityError, validate_release_identity  # noqa: E402


REQUIRED_MEMBERS = {
    "ABI_VERSION",
    "CMakeLists.txt",
    "LICENSE",
    "NOTICE",
    "THIRD_PARTY_NOTICES.md",
    "VERSION",
    "include/glifistore/abi/glifistore.h",
}
FORBIDDEN_PARTS = {".git", ".idea", ".vscode", "__pycache__"}
FORBIDDEN_TOP_LEVEL = {"build", "dist", ".tools"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_archive(path: Path, prefix: str) -> None:
    present: set[str] = set()
    with tarfile.open(path, mode="r:xz") as archive:
        members = archive.getmembers()
        if not members:
            raise ReleaseIdentityError("source archive is empty")
        names = [member.name for member in members]
        if names != sorted(names):
            raise ReleaseIdentityError("source archive member order is not deterministic")
        for member in members:
            candidate = PurePosixPath(member.name)
            if candidate.is_absolute() or ".." in candidate.parts:
                raise ReleaseIdentityError(f"unsafe archive member: {member.name}")
            if not candidate.parts or candidate.parts[0] != prefix.rstrip("/"):
                raise ReleaseIdentityError(f"archive member escapes release prefix: {member.name}")
            relative = PurePosixPath(*candidate.parts[1:])
            if any(part in FORBIDDEN_PARTS for part in relative.parts):
                raise ReleaseIdentityError(f"forbidden source member: {member.name}")
            if relative.parts and relative.parts[0] in FORBIDDEN_TOP_LEVEL:
                raise ReleaseIdentityError(f"forbidden generated tree: {member.name}")
            if member.isfile():
                present.add(str(relative))
            if member.uid != 0 or member.gid != 0:
                raise ReleaseIdentityError(f"non-normalized owner for {member.name}")
    missing = sorted(REQUIRED_MEMBERS - present)
    if missing:
        raise ReleaseIdentityError(f"source archive misses required files: {', '.join(missing)}")


def build_archive(root: Path, tag: str, output_directory: Path) -> Path:
    identity = validate_release_identity(root, tag)
    output_directory.mkdir(parents=True, exist_ok=True)
    archive_path = output_directory / f"GlifiStore-{identity.product_version}.tar.xz"
    if archive_path.exists():
        raise ReleaseIdentityError(f"refusing to replace existing artifact: {archive_path}")
    prefix = f"GlifiStore-{identity.product_version}/"
    with tempfile.TemporaryDirectory(prefix="glifistore-source-") as temporary:
        tar_path = Path(temporary) / "source.tar"
        with tar_path.open("wb") as destination:
            archived = subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "archive",
                    "--format=tar",
                    f"--prefix={prefix}",
                    tag,
                ],
                check=False,
                stdout=destination,
                stderr=subprocess.PIPE,
            )
        if archived.returncode != 0:
            raise ReleaseIdentityError(
                "git archive failed: " + archived.stderr.decode("utf-8", errors="replace").strip()
            )
        with tar_path.open("rb") as source, lzma.open(
            archive_path, mode="wb", format=lzma.FORMAT_XZ, preset=9
        ) as destination:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                destination.write(block)
    validate_archive(archive_path, prefix)
    return archive_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output-directory", type=Path, default=ROOT / "dist" / "release-candidate")
    arguments = parser.parse_args()
    try:
        result = build_archive(arguments.root.resolve(), arguments.tag, arguments.output_directory)
    except (OSError, ReleaseIdentityError, tarfile.TarError, lzma.LZMAError) as error:
        print(f"source release FAILED: {error}", file=sys.stderr)
        return 1
    print(f"source_archive={result}")
    print(f"sha256={sha256(result)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
