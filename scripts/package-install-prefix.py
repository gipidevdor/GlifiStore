#!/usr/bin/env python3
"""Package one already-installed GlifiStore prefix without rebuilding it."""

from __future__ import annotations

import argparse
import os
import re
import sys
import tarfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engineering" / "tools"))

from release_identity import ReleaseIdentityError, validate_release_identity  # noqa: E402


VERSIONED_ABI_PATTERNS = (
    re.compile(r"^libglifistore\.so\.[0-9]+(?:\.[0-9]+)*$"),
    re.compile(r"^libglifistore\.[0-9]+(?:\.[0-9]+)*\.dylib$"),
    re.compile(r"^glifistore-[0-9]+(?:\.[0-9]+)*\.dll$"),
)


def _is_versioned_abi(path: Path) -> bool:
    return any(pattern.fullmatch(path.name) for pattern in VERSIONED_ABI_PATTERNS)


def _safe_link_target(relative: PurePosixPath, target: str) -> bool:
    candidate = PurePosixPath(target)
    if candidate.is_absolute():
        return False
    depth = len(relative.parent.parts)
    for part in candidate.parts:
        if part == "..":
            depth -= 1
            if depth < 0:
                return False
        elif part not in ("", "."):
            depth += 1
    return True


def _normalized_info(name: str, epoch: int, mode: int, kind: bytes) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.mtime = epoch
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mode = mode
    info.type = kind
    return info


def package_prefix(
    root: Path,
    tag: str,
    prefix: Path,
    output_directory: Path,
    target_os: str,
    architecture: str,
) -> Path:
    identity = validate_release_identity(root, tag)
    prefix = prefix.resolve()
    if not prefix.is_dir():
        raise ReleaseIdentityError(f"install prefix does not exist: {prefix}")
    relative_files = [path.relative_to(prefix) for path in prefix.rglob("*")]
    required = {
        PurePosixPath("bin/glifistored"),
        PurePosixPath("include/glifistore/abi/glifistore.h"),
        PurePosixPath("share/GlifiStore/VERSION"),
        PurePosixPath("share/GlifiStore/ABI_VERSION"),
    }
    normalized = {PurePosixPath(path.as_posix()) for path in relative_files}
    missing = sorted(str(path) for path in required - normalized)
    if missing:
        raise ReleaseIdentityError(f"install prefix misses required files: {', '.join(missing)}")
    if not any(_is_versioned_abi(path) for path in relative_files):
        raise ReleaseIdentityError("install prefix misses the versioned shared C ABI")
    installed_version = (prefix / "share/GlifiStore/VERSION").read_text(encoding="utf-8").strip()
    installed_abi = (prefix / "share/GlifiStore/ABI_VERSION").read_text(encoding="utf-8").strip()
    if installed_version != identity.product_version:
        raise ReleaseIdentityError("installed VERSION disagrees with release identity")
    if installed_abi != f"{identity.abi_major}.{identity.abi_minor}":
        raise ReleaseIdentityError("installed ABI_VERSION disagrees with release identity")

    safe_os = target_os.lower().replace(" ", "-")
    safe_arch = architecture.lower().replace(" ", "-")
    if not safe_os.replace("-", "").replace("_", "").isalnum() or not safe_arch.replace(
        "-", ""
    ).replace("_", "").isalnum():
        raise ReleaseIdentityError("target OS and architecture must be simple identifiers")
    basename = f"glifistore-{identity.product_version}-{safe_os}-{safe_arch}"
    output_directory.mkdir(parents=True, exist_ok=True)
    output = output_directory / f"{basename}.tar.xz"
    if output.exists():
        raise ReleaseIdentityError(f"refusing to replace existing artifact: {output}")

    with tarfile.open(output, "w:xz", format=tarfile.PAX_FORMAT, preset=9) as archive:
        root_info = _normalized_info(f"{basename}/", identity.source_date_epoch, 0o755, tarfile.DIRTYPE)
        archive.addfile(root_info)
        for relative_path in sorted(relative_files, key=lambda path: path.as_posix()):
            source = prefix / relative_path
            relative = PurePosixPath(relative_path.as_posix())
            archive_name = f"{basename}/{relative.as_posix()}"
            stat = source.lstat()
            if source.is_symlink():
                target = os.readlink(source)
                if not _safe_link_target(relative, target):
                    raise ReleaseIdentityError(f"unsafe install-prefix symlink: {relative} -> {target}")
                info = _normalized_info(archive_name, identity.source_date_epoch, 0o777, tarfile.SYMTYPE)
                info.linkname = target
                archive.addfile(info)
            elif source.is_dir():
                info = _normalized_info(archive_name + "/", identity.source_date_epoch, 0o755,
                                        tarfile.DIRTYPE)
                archive.addfile(info)
            elif source.is_file():
                mode = 0o755 if stat.st_mode & 0o111 else 0o644
                info = _normalized_info(archive_name, identity.source_date_epoch, mode, tarfile.REGTYPE)
                info.size = stat.st_size
                with source.open("rb") as stream:
                    archive.addfile(info, stream)
            else:
                raise ReleaseIdentityError(f"special file forbidden in install prefix: {relative}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, default=ROOT / "dist" / "release-candidate")
    parser.add_argument("--target-os", required=True)
    parser.add_argument("--architecture", required=True)
    arguments = parser.parse_args()
    try:
        output = package_prefix(
            arguments.root.resolve(),
            arguments.tag,
            arguments.prefix,
            arguments.output_directory,
            arguments.target_os,
            arguments.architecture,
        )
    except (OSError, ReleaseIdentityError, tarfile.TarError) as error:
        print(f"binary package FAILED: {error}", file=sys.stderr)
        return 1
    print(f"binary_archive={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
