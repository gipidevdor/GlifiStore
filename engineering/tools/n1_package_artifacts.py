#!/usr/bin/env python3
"""Resolve sealed N-1 package bytes supplied for package-upgrade continuity.

GLIFISTORE_N1_PACKAGE_DIR points at a directory of already-published packages.
This module never rebuilds a predecessor from HEAD: it only selects files that
already exist on disk and whose names carry the SemVer selected by the release
context.

Linux selects ``.deb`` / ``.rpm``; FreeBSD ``.pkg``; OpenBSD ``.tgz``; macOS
MacPorts/Homebrew select the sealed ``GlifiStore-<version>.tar.xz`` source
archive that those backends build and install from.

Version matching is token-bound (not a substring), so ``0.0.9`` does not match
``10.0.9`` or ``0.0.90``. Multi-arch directories refuse unless ``arch`` is set.
When a ``SHA256SUMS`` (or ``SHA256SUMS.txt``) file is present beside selected
bytes, digests are verified fail-closed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path

N1_PACKAGE_DIR_ENVIRONMENT = "GLIFISTORE_N1_PACKAGE_DIR"
CONTAINER_N1_MOUNT = "/n1-packages"

_EXCLUDED_NAME_TOKENS = ("debuginfo", "debugsource", "-dbgsym")
_LINUX_NAME = re.compile(
    r"^(?:lib)?glifistore",
    re.IGNORECASE,
)
_BSD_NAME = re.compile(r"^glifistore", re.IGNORECASE)
_MACOS_SOURCE_NAME = re.compile(r"^GlifiStore-(.+)\.tar\.xz$")
# Arch tokens that appear in GlifiStore package basenames across backends.
_ARCH_TOKEN = re.compile(
    r"(?:^|[-_.])(amd64|arm64|aarch64|x86_64|i386|armv7)(?:[-_.]|$)",
    re.IGNORECASE,
)
_SHA256SUMS_NAMES = ("SHA256SUMS", "SHA256SUMS.txt", "sha256sums", "sha256sums.txt")


class N1PackageError(RuntimeError):
    """Sealed N-1 packages were requested but could not be resolved honestly."""


def _require_safe_version(version: str) -> str:
    if not version or "/" in version or ".." in version:
        raise N1PackageError(f"refusing unsafe N-1 product version: {version!r}")
    return version


def _require_directory(directory: Path) -> Path:
    if directory.is_symlink() or not directory.is_dir():
        raise N1PackageError(f"N-1 package directory is missing or not regular: {directory}")
    return directory


def version_token_in_name(name: str, version: str) -> bool:
    """True when ``version`` appears as a SemVer-ish token, not a substring of another."""
    pattern = re.compile(
        rf"(?:^|[^0-9A-Za-z.]){re.escape(version)}(?:[^0-9A-Za-z.]|$)"
    )
    return pattern.search(name) is not None


def arch_token_in_name(name: str) -> str | None:
    match = _ARCH_TOKEN.search(name)
    if match is None:
        return None
    token = match.group(1).lower()
    if token == "x86_64":
        return "amd64"
    if token == "aarch64":
        return "arm64"
    return token


def _normalize_arch_filter(arch: str | None) -> str | None:
    if arch is None or not arch.strip():
        return None
    token = arch.strip().lower()
    if token in {"x86_64", "amd64"}:
        return "amd64"
    if token in {"aarch64", "arm64"}:
        return "arm64"
    return token


def _refuse_ambiguous_arches(selected: list[Path], arch: str | None) -> list[Path]:
    wanted = _normalize_arch_filter(arch)
    if wanted is not None:
        filtered = [
            path
            for path in selected
            if (token := arch_token_in_name(path.name)) is None or token == wanted
        ]
        if not filtered:
            raise N1PackageError(
                f"no sealed packages matched architecture filter {wanted!r} among "
                f"{[path.name for path in selected]}"
            )
        selected = filtered

    declared = {
        token
        for path in selected
        if (token := arch_token_in_name(path.name)) is not None
    }
    if len(declared) > 1:
        raise N1PackageError(
            "multiple architectures among sealed N-1 packages "
            f"({sorted(declared)}); pass an explicit arch filter or split the directory: "
            f"{[path.name for path in selected]}"
        )
    return selected


def _load_sha256sums(directory: Path) -> dict[str, str] | None:
    for name in _SHA256SUMS_NAMES:
        path = directory / name
        if path.is_symlink() or not path.is_file():
            continue
        digests: dict[str, str] = {}
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            parts = stripped.split()
            if len(parts) < 2:
                raise N1PackageError(f"malformed checksum line in {path}: {line!r}")
            digest, filename = parts[0].lower(), parts[-1].lstrip("*")
            if len(digest) != 64 or any(char not in "0123456789abcdef" for char in digest):
                raise N1PackageError(f"malformed SHA-256 in {path}: {digest!r}")
            digests[Path(filename).name] = digest
        return digests
    return None


def verify_selected_digests(directory: Path, selected: list[Path]) -> None:
    """When SHA256SUMS is present under ``directory``, require every selected basename."""
    digests = _load_sha256sums(directory)
    if digests is None:
        return
    for path in selected:
        expected = digests.get(path.name)
        if expected is None:
            raise N1PackageError(
                f"sealed package {path.name} is absent from SHA256SUMS under {directory}"
            )
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected:
            raise N1PackageError(
                f"digest mismatch for sealed package {path.name}: "
                f"got {actual}, want {expected}"
            )


def supplied_n1_package_dir() -> Path | None:
    """Return the operator-supplied directory, or None when the env var is unset."""
    raw = os.environ.get(N1_PACKAGE_DIR_ENVIRONMENT, "").strip()
    if not raw:
        return None
    path = Path(raw)
    if path.is_symlink() or not path.is_dir():
        raise N1PackageError(
            f"{N1_PACKAGE_DIR_ENVIRONMENT}={raw!r} is not a regular directory of sealed packages"
        )
    return path.resolve()


def select_linux_n1_packages(
    directory: Path, backend: str, version: str, *, arch: str | None = None
) -> list[Path]:
    """Primary deb/rpm packages for ``version`` under ``directory`` (never rebuilt)."""
    if backend not in ("deb", "rpm"):
        raise N1PackageError(f"Linux N-1 package selection does not support backend {backend!r}")
    version = _require_safe_version(version)
    directory = _require_directory(directory)

    suffix = ".deb" if backend == "deb" else ".rpm"
    selected: list[Path] = []
    for path in sorted(directory.rglob(f"*{suffix}")):
        if path.is_symlink() or not path.is_file():
            continue
        name = path.name
        if any(token in name for token in _EXCLUDED_NAME_TOKENS):
            continue
        if _LINUX_NAME.match(name) is None:
            continue
        if not version_token_in_name(name, version):
            continue
        selected.append(path.resolve())
    if not selected:
        raise N1PackageError(
            f"no sealed {backend} packages for product version {version} under {directory}"
        )
    selected = _refuse_ambiguous_arches(selected, arch)
    verify_selected_digests(directory, selected)
    return selected


def select_bsd_n1_packages(
    directory: Path, backend: str, version: str, *, arch: str | None = None
) -> list[Path]:
    """Primary FreeBSD ``.pkg`` or OpenBSD ``.tgz`` packages for ``version``."""
    if backend not in ("freebsd", "openbsd"):
        raise N1PackageError(f"BSD N-1 package selection does not support backend {backend!r}")
    version = _require_safe_version(version)
    directory = _require_directory(directory)

    suffix = ".pkg" if backend == "freebsd" else ".tgz"
    selected: list[Path] = []
    for path in sorted(directory.rglob(f"*{suffix}")):
        if path.is_symlink() or not path.is_file():
            continue
        name = path.name
        if _BSD_NAME.match(name) is None:
            continue
        if not version_token_in_name(name, version):
            continue
        selected.append(path.resolve())
    if not selected:
        raise N1PackageError(
            f"no sealed {backend} packages for product version {version} under {directory}"
        )
    selected = _refuse_ambiguous_arches(selected, arch)
    verify_selected_digests(directory, selected)
    return selected


def select_macos_n1_source(directory: Path, version: str) -> Path:
    """Sealed ``GlifiStore-<version>.tar.xz`` under ``directory`` (never rebuilt)."""
    version = _require_safe_version(version)
    directory = _require_directory(directory)
    expected = f"GlifiStore-{version}.tar.xz"
    selected: list[Path] = []
    for path in sorted(directory.rglob("GlifiStore-*.tar.xz")):
        if path.is_symlink() or not path.is_file():
            continue
        match = _MACOS_SOURCE_NAME.match(path.name)
        if match is None or match.group(1) != version:
            continue
        selected.append(path.resolve())
    if not selected:
        raise N1PackageError(
            f"no sealed macOS source archive {expected} under {directory}"
        )
    if len(selected) > 1:
        raise N1PackageError(
            f"multiple sealed macOS source archives for {version} under {directory}: "
            f"{[path.name for path in selected]}"
        )
    verify_selected_digests(directory, selected)
    return selected[0]


def upgrade_exercise_requested(previous: dict) -> bool:
    """True when a SemVer predecessor exists and sealed package bytes were supplied."""
    if not previous.get("available"):
        return False
    return supplied_n1_package_dir() is not None


def _cli(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    linux = sub.add_parser("select-linux", help="Print sealed Linux N-1 package paths")
    linux.add_argument("--backend", choices=("deb", "rpm"), required=True)
    linux.add_argument("--directory", type=Path, required=True)
    linux.add_argument("--version", required=True)
    linux.add_argument("--arch", default=None)

    bsd = sub.add_parser("select-bsd", help="Print sealed FreeBSD/OpenBSD N-1 package paths")
    bsd.add_argument("--backend", choices=("freebsd", "openbsd"), required=True)
    bsd.add_argument("--directory", type=Path, required=True)
    bsd.add_argument("--version", required=True)
    bsd.add_argument("--arch", default=None)

    macos = sub.add_parser("select-macos-source", help="Print sealed macOS N-1 source archive")
    macos.add_argument("--directory", type=Path, required=True)
    macos.add_argument("--version", required=True)

    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "select-linux":
            paths = select_linux_n1_packages(
                arguments.directory,
                arguments.backend,
                arguments.version,
                arch=arguments.arch,
            )
        elif arguments.command == "select-bsd":
            paths = select_bsd_n1_packages(
                arguments.directory,
                arguments.backend,
                arguments.version,
                arch=arguments.arch,
            )
        else:
            paths = [select_macos_n1_source(arguments.directory, arguments.version)]
    except N1PackageError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for path in paths:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli())
