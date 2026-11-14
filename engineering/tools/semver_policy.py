#!/usr/bin/env python3
"""SemVer authority and package-version normalization for GlifiStore backends.

VERSION is the single source of truth for the product version. This module parses
it, orders releases by SemVer 2.0.0 precedence, classifies the change kind, and
maps a (version, package_revision) pair onto the version grammar of every
packaging backend in engineering/distribution/package-matrix.yaml. Mappings that
are deterministic but not yet proven against the native tooling carry explicit
limitations instead of an implied guarantee.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from typing import Any, Iterable


SEMVER = re.compile(
    r"^(?P<major>0|[1-9][0-9]*)\.(?P<minor>0|[1-9][0-9]*)\.(?P<patch>0|[1-9][0-9]*)"
    r"(?:-(?P<prerelease>(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
    r"(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+(?P<build>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)

BACKENDS = ("deb", "freebsd", "homebrew", "macports", "openbsd", "rpm")

DEBIAN_UPSTREAM = re.compile(r"^[0-9][A-Za-z0-9.+~]*$")
RPM_FIELD = re.compile(r"^[A-Za-z0-9.~^_+]+$")
MACPORTS_VERSION = re.compile(r"^[A-Za-z0-9._]+$")
FREEBSD_PORTVERSION = re.compile(r"^[A-Za-z0-9._+]+$")
OPENBSD_VERSION = re.compile(r"^[0-9][0-9A-Za-z.]*$")

BUILD_METADATA_LIMITATION = (
    "SemVer build metadata is not representable in any supported package version grammar "
    "and is dropped from the package version"
)


class SemverError(RuntimeError):
    pass


@dataclass(frozen=True)
class Version:
    """A strict SemVer 2.0.0 version.

    Equality is structural (build metadata included); ordering follows SemVer
    precedence, which ignores build metadata.
    """

    major: int
    minor: int
    patch: int
    prerelease: tuple[str, ...] = ()
    build: tuple[str, ...] = ()

    @property
    def core(self) -> str:
        return f"{self.major}.{self.minor}.{self.patch}"

    @property
    def is_prerelease(self) -> bool:
        return bool(self.prerelease)

    def __str__(self) -> str:
        text = self.core
        if self.prerelease:
            text += "-" + ".".join(self.prerelease)
        if self.build:
            text += "+" + ".".join(self.build)
        return text

    def precedence(self) -> tuple[Any, ...]:
        identifiers = [
            (0, int(part), "") if part.isdigit() else (1, 0, part) for part in self.prerelease
        ]
        # An absent prerelease outranks any prerelease of the same core version.
        return (self.major, self.minor, self.patch, 0 if self.prerelease else 1, identifiers)

    def __lt__(self, other: object) -> bool:
        if not isinstance(other, Version):
            return NotImplemented
        return self.precedence() < other.precedence()

    def __le__(self, other: object) -> bool:
        if not isinstance(other, Version):
            return NotImplemented
        return self.precedence() <= other.precedence()

    def __gt__(self, other: object) -> bool:
        if not isinstance(other, Version):
            return NotImplemented
        return self.precedence() > other.precedence()

    def __ge__(self, other: object) -> bool:
        if not isinstance(other, Version):
            return NotImplemented
        return self.precedence() >= other.precedence()

    def as_dict(self) -> dict[str, Any]:
        return {
            "text": str(self),
            "major": self.major,
            "minor": self.minor,
            "patch": self.patch,
            "prerelease": list(self.prerelease),
            "build": list(self.build),
            "is_prerelease": self.is_prerelease,
        }


@dataclass(frozen=True)
class PackageVersion:
    backend: str
    upstream_version: str
    package_version: str
    fields: dict[str, str] = field(default_factory=dict)
    limitations: tuple[str, ...] = ()

    def as_dict(self) -> dict[str, Any]:
        return {
            "backend": self.backend,
            "upstream_version": self.upstream_version,
            "package_version": self.package_version,
            "fields": dict(self.fields),
            "limitations": list(self.limitations),
        }


def parse(text: str, *, allow_v_prefix: bool = True) -> Version:
    if not isinstance(text, str):
        raise SemverError("version must be a string")
    candidate = text.strip()
    if allow_v_prefix and candidate.startswith("v"):
        candidate = candidate[1:]
    match = SEMVER.fullmatch(candidate)
    if match is None:
        raise SemverError(f"not a strict SemVer 2.0.0 version: {text!r}")
    prerelease = match.group("prerelease")
    build = match.group("build")
    return Version(
        major=int(match.group("major")),
        minor=int(match.group("minor")),
        patch=int(match.group("patch")),
        prerelease=tuple(prerelease.split(".")) if prerelease else (),
        build=tuple(build.split(".")) if build else (),
    )


def coerce(value: Version | str) -> Version:
    return value if isinstance(value, Version) else parse(value)


def compare(left: Version | str, right: Version | str) -> int:
    first, second = coerce(left), coerce(right)
    if first < second:
        return -1
    return 1 if first > second else 0


def sort_versions(values: Iterable[Version | str]) -> list[Version]:
    return sorted((coerce(value) for value in values), key=Version.precedence)


def release_kind(current: Version | str, previous: Version | str | None = None) -> str:
    version = coerce(current)
    if previous is None:
        return "initial"
    baseline = coerce(previous)
    if version <= baseline:
        raise SemverError(f"current version {version} does not follow {baseline}")
    if version.is_prerelease:
        return "prerelease"
    if version.major != baseline.major:
        return "major"
    if version.minor != baseline.minor:
        return "minor"
    return "patch"


def select_previous(
    candidates: Iterable[Version | str],
    current: Version | str,
    *,
    include_prereleases: bool = False,
) -> Version | None:
    """Greatest release strictly older than current; never `current minus one`."""
    version = coerce(current)
    older = [
        candidate
        for candidate in sort_versions(candidates)
        if candidate < version and (include_prereleases or not candidate.is_prerelease)
    ]
    return older[-1] if older else None


def _revision(package_revision: int) -> int:
    if type(package_revision) is not int or package_revision < 0:
        raise SemverError("package revision must be a non-negative integer")
    return package_revision


def _tilde_upstream(version: Version) -> str:
    """Debian and RPM order '~' before the empty string, so prereleases precede the release."""
    if not version.prerelease:
        return version.core
    return version.core + "~" + ".".join(version.prerelease)


def _base_limitations(version: Version) -> list[str]:
    return [BUILD_METADATA_LIMITATION] if version.build else []


def _deb(version: Version, package_revision: int) -> PackageVersion:
    revision = _revision(package_revision) + 1
    upstream = _tilde_upstream(version)
    if DEBIAN_UPSTREAM.fullmatch(upstream) is None:
        raise SemverError(f"not a valid Debian upstream version: {upstream}")
    limitations = _base_limitations(version)
    return PackageVersion(
        backend="deb",
        upstream_version=upstream,
        package_version=f"{upstream}-{revision}",
        fields={
            "epoch": "0",
            "upstream_version": upstream,
            "debian_revision": str(revision),
        },
        limitations=tuple(limitations),
    )


def _rpm(version: Version, package_revision: int) -> PackageVersion:
    release = str(_revision(package_revision) + 1)
    upstream = _tilde_upstream(version)
    if RPM_FIELD.fullmatch(upstream) is None or RPM_FIELD.fullmatch(release) is None:
        raise SemverError(f"not a valid RPM version/release pair: {upstream}-{release}")
    limitations = _base_limitations(version)
    if version.is_prerelease:
        limitations.append("RPM '~' prerelease ordering requires rpm >= 4.10 on every target")
    return PackageVersion(
        backend="rpm",
        upstream_version=upstream,
        package_version=f"{upstream}-{release}",
        fields={"epoch": "0", "version": upstream, "release": release},
        limitations=tuple(limitations),
    )


def _macports(version: Version, package_revision: int) -> PackageVersion:
    revision = _revision(package_revision)
    # MacPorts version strings cannot contain '-', which separates name from version.
    upstream = ".".join((version.core, *version.prerelease))
    if MACPORTS_VERSION.fullmatch(upstream) is None:
        raise SemverError(f"not a valid MacPorts version: {upstream}")
    limitations = _base_limitations(version)
    if version.is_prerelease:
        limitations.append(
            "MacPorts vercmp orders the dotted prerelease form above the final release; "
            "publishing a prerelease needs an epoch decision that does not exist yet"
        )
    return PackageVersion(
        backend="macports",
        upstream_version=upstream,
        package_version=f"{upstream}_{revision}",
        fields={"epoch": "0", "version": upstream, "revision": str(revision)},
        limitations=tuple(limitations),
    )


def _homebrew(version: Version, package_revision: int) -> PackageVersion:
    revision = _revision(package_revision)
    upstream = version.core + ("-" + ".".join(version.prerelease) if version.prerelease else "")
    limitations = _base_limitations(version)
    if version.is_prerelease:
        limitations.append(
            "Homebrew tokenization of the prerelease identifiers is not proven in CI"
        )
    return PackageVersion(
        backend="homebrew",
        upstream_version=upstream,
        package_version=upstream + (f"_{revision}" if revision else ""),
        fields={"version": upstream, "revision": str(revision)},
        limitations=tuple(limitations),
    )


def _freebsd(version: Version, package_revision: int) -> PackageVersion:
    revision = _revision(package_revision)
    distversion = version.core + ("-" + ".".join(version.prerelease) if version.prerelease else "")
    portversion = distversion.replace("-", ".")
    if FREEBSD_PORTVERSION.fullmatch(portversion) is None:
        raise SemverError(f"not a valid FreeBSD PORTVERSION: {portversion}")
    limitations = _base_limitations(version)
    if version.is_prerelease:
        limitations.append(
            "FreeBSD DISTVERSION to PORTVERSION mangling is reproduced here, not read back "
            "from 'make -V PORTVERSION' on a native ports tree"
        )
    return PackageVersion(
        backend="freebsd",
        upstream_version=distversion,
        package_version=portversion + (f"_{revision}" if revision else ""),
        fields={
            "distversion": distversion,
            "portversion": portversion,
            "portrevision": str(revision),
            "portepoch": "0",
        },
        limitations=tuple(limitations),
    )


def _openbsd(version: Version, package_revision: int) -> PackageVersion:
    revision = _revision(package_revision)
    # OpenBSD pkgnames keep the version dotted-numeric with an optional alphanumeric suffix.
    upstream = version.core + ("".join(version.prerelease) if version.prerelease else "")
    if OPENBSD_VERSION.fullmatch(upstream) is None:
        raise SemverError(f"not a valid OpenBSD package version: {upstream}")
    limitations = _base_limitations(version)
    if version.is_prerelease:
        limitations.append(
            "OpenBSD pkg_add(1) ordering of the concatenated prerelease suffix is unverified"
        )
    return PackageVersion(
        backend="openbsd",
        upstream_version=upstream,
        package_version=upstream + (f"p{revision}" if revision else ""),
        fields={"v": upstream, "revision": str(revision)},
        limitations=tuple(limitations),
    )


NORMALIZERS = {
    "deb": _deb,
    "freebsd": _freebsd,
    "homebrew": _homebrew,
    "macports": _macports,
    "openbsd": _openbsd,
    "rpm": _rpm,
}


def normalize(version: Version | str, backend: str, package_revision: int = 0) -> PackageVersion:
    if backend not in NORMALIZERS:
        raise SemverError(f"unsupported packaging backend: {backend}")
    return NORMALIZERS[backend](coerce(version), package_revision)


def normalize_all(version: Version | str, package_revision: int = 0) -> dict[str, PackageVersion]:
    parsed = coerce(version)
    return {backend: normalize(parsed, backend, package_revision) for backend in BACKENDS}


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    commands = result.add_subparsers(dest="command", required=True)

    parse_command = commands.add_parser("parse")
    parse_command.add_argument("version")

    compare_command = commands.add_parser("compare")
    compare_command.add_argument("left")
    compare_command.add_argument("right")

    kind_command = commands.add_parser("kind")
    kind_command.add_argument("--current", required=True)
    kind_command.add_argument("--previous")

    previous_command = commands.add_parser("select-previous")
    previous_command.add_argument("--current", required=True)
    previous_command.add_argument("--candidate", action="append", default=[])
    previous_command.add_argument("--include-prereleases", action="store_true")

    normalize_command = commands.add_parser("normalize")
    normalize_command.add_argument("--version", required=True)
    normalize_command.add_argument("--backend", choices=("all", *BACKENDS), default="all")
    normalize_command.add_argument("--package-revision", type=int, default=0)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        if arguments.command == "parse":
            print(json.dumps(parse(arguments.version).as_dict(), indent=2, sort_keys=True))
        elif arguments.command == "compare":
            print(compare(arguments.left, arguments.right))
        elif arguments.command == "kind":
            print(release_kind(arguments.current, arguments.previous))
        elif arguments.command == "select-previous":
            selected = select_previous(
                arguments.candidate,
                arguments.current,
                include_prereleases=arguments.include_prereleases,
            )
            print("" if selected is None else str(selected))
        else:
            if arguments.backend == "all":
                value = {
                    backend: package.as_dict()
                    for backend, package in normalize_all(
                        arguments.version, arguments.package_revision
                    ).items()
                }
            else:
                value = normalize(
                    arguments.version, arguments.backend, arguments.package_revision
                ).as_dict()
            print(json.dumps(value, indent=2, sort_keys=True))
    except SemverError as error:
        print(f"semver policy FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
