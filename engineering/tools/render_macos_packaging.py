#!/usr/bin/env python3
"""Render the GlifiStore MacPorts Portfile and Homebrew formula, fail-closed.

The templates under packaging/macports/ and packaging/homebrew/ carry no version,
URL or checksum. This tool injects them from the release context, which derives
every package version from the VERSION authority. The release profile additionally
refuses anything that is not a sealed source archive: a checkout, a git ref tarball
or a local working copy can never become the source of a released package.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.generate_release_context import ReleaseContextError, load_release_context
from engineering.tools.package_framework import (
    HEX64,
    PROFILES,
    REPO_ROOT,
    PackageFrameworkError,
    digest,
)

MACOS_BACKENDS = ("homebrew", "macports")
TEMPLATES = {
    "macports": Path("packaging/macports/Portfile.in"),
    "homebrew": Path("packaging/homebrew/glifistore.rb.in"),
}
RENDERED_NAMES = {"macports": "Portfile", "homebrew": "glifistore.rb"}
PLACEHOLDER = re.compile(r"\{\{[A-Z][A-Z0-9_]*\}\}")

# Markers of a source that is a git ref or a checkout rather than a sealed
# release archive. GitHub serves these under /archive/ and codeload.
UNSEALED_MARKERS = (
    "/archive/",
    "refs/heads",
    "refs/tags",
    "codeload.",
    "/head/",
    ".git",
)


class MacPackagingError(RuntimeError):
    pass


@dataclass(frozen=True)
class SourceArchive:
    """The exact bytes a rendered port or formula is allowed to build from."""

    url: str
    sha256: str
    size: int | None
    rmd160: str | None = None

    @property
    def basename(self) -> str:
        return archive_basename(self.url)

    @property
    def master_sites(self) -> str:
        return self.url[: -len(self.basename)]

    @property
    def distname(self) -> str:
        return self.basename[: -len(".tar.xz")]


def archive_basename(url: str) -> str:
    return unquote(urlsplit(url).path.rsplit("/", 1)[-1])


def source_basename(product_version: str) -> str:
    return f"GlifiStore-{product_version}.tar.xz"


def ripemd160(path: Path) -> str | None:
    """MacPorts recommends rmd160; OpenSSL 3 often disables it, so it stays optional."""
    try:
        value = hashlib.new("ripemd160")
    except ValueError:
        return None
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def resolve_source(
    *,
    url: str,
    sha256: str,
    size: int | None,
    product_version: str,
    profile: str,
    rmd160: str | None = None,
) -> SourceArchive:
    """Accept a source only when its identity is complete and matches the profile."""
    if profile not in PROFILES:
        raise MacPackagingError(f"unsupported CI profile: {profile}")
    if not isinstance(url, str) or not url.strip():
        raise MacPackagingError("a source archive URL is required")
    url = url.strip()
    if not isinstance(sha256, str) or HEX64.fullmatch(sha256.strip()) is None:
        raise MacPackagingError("a lowercase hexadecimal SHA-256 of the source archive is required")
    sha256 = sha256.strip()

    split = urlsplit(url)
    if split.scheme not in ("https", "file"):
        raise MacPackagingError(f"unsupported source scheme: {split.scheme or '<none>'}")
    expected = source_basename(product_version)
    if archive_basename(url) != expected:
        raise MacPackagingError(
            f"the source archive must be the sealed {expected}, got {archive_basename(url)}"
        )
    lowered = url.lower()
    for marker in UNSEALED_MARKERS:
        if marker in lowered:
            raise MacPackagingError(
                f"{url} looks like a git ref or checkout ('{marker}'), not a sealed source archive"
            )

    if profile == "release":
        if split.scheme != "https":
            raise MacPackagingError(
                "the release profile requires an https source archive URL; "
                f"refusing scheme '{split.scheme}'"
            )
        if not split.netloc:
            raise MacPackagingError("the release source archive URL has no host")
    elif split.scheme == "file":
        # A build-from-tree source is only honest if the bytes really are the
        # ones the rendered metadata pins.
        local = Path(unquote(split.path))
        if local.is_symlink() or not local.is_file():
            raise MacPackagingError(f"local source archive is missing: {local}")
        actual = digest(local)
        if actual != sha256:
            raise MacPackagingError(
                f"local source archive digest mismatch: expected {sha256}, measured {actual}"
            )
        measured = local.stat().st_size
        if size is not None and size != measured:
            raise MacPackagingError(f"local source archive size mismatch: {size} != {measured}")
        size = measured
        rmd160 = rmd160 or ripemd160(local)

    if size is not None and (type(size) is not int or size <= 0):
        raise MacPackagingError("the source archive size must be a positive integer")
    if rmd160 is not None and re.fullmatch(r"[0-9a-f]{40}", rmd160) is None:
        raise MacPackagingError("the source archive RIPEMD-160 must be 40 lowercase hex digits")
    return SourceArchive(url=url, sha256=sha256, size=size, rmd160=rmd160)


def _require_sealed_context(context: dict[str, Any]) -> None:
    git = context["git"]
    if not git["tree_clean"]:
        raise MacPackagingError(
            "the release profile refuses a modified working tree as a packaging source"
        )
    if not git["tag_is_annotated"] or not git["tag_matches_commit"]:
        raise MacPackagingError(
            "the release profile requires the annotated release tag to point at this commit"
        )


def _provenance(backend: str, context: dict[str, Any], profile: str) -> str:
    return (
        f"# rendered for the {profile} packaging profile from GlifiStore "
        f"{context['product_version']} at commit {context['git']['commit']}\n"
        f"# backend: {backend}; edits belong in {TEMPLATES[backend].as_posix()}"
    )


def _macports_checksums(source: SourceArchive) -> str:
    indent = " " * 24
    lines = []
    if source.rmd160 is not None:
        lines.append(f"rmd160  {source.rmd160}")
    lines.append(f"sha256  {source.sha256}")
    if source.size is not None:
        lines.append(f"size    {source.size}")
    return f" \\\n{indent}".join(lines)


def render(
    backend: str,
    context: dict[str, Any],
    source: SourceArchive,
    *,
    profile: str,
    root: Path = REPO_ROOT,
) -> str:
    if backend not in MACOS_BACKENDS:
        raise MacPackagingError(f"unsupported macOS packaging backend: {backend}")
    if profile not in PROFILES:
        raise MacPackagingError(f"unsupported CI profile: {profile}")
    if profile == "release":
        _require_sealed_context(context)
    if source.basename != source_basename(context["product_version"]):
        raise MacPackagingError("the source archive does not belong to this release context")

    package = context["package_versions"][backend]
    template_path = root / TEMPLATES[backend]
    if template_path.is_symlink() or not template_path.is_file():
        raise MacPackagingError(f"missing packaging template: {template_path}")
    template = template_path.read_text(encoding="utf-8")

    values = {"PROVENANCE": _provenance(backend, context, profile)}
    if backend == "macports":
        port_version = package["fields"]["version"]
        # Keep the MacPorts idiom when the distfile follows the port version; a
        # prerelease, whose grammars differ, falls back to the literal name.
        distname = (
            "GlifiStore-${version}"
            if source.distname == f"GlifiStore-{port_version}"
            else source.distname
        )
        values.update(
            {
                "VERSION": port_version,
                "REVISION": package["fields"]["revision"],
                "MASTER_SITES": source.master_sites,
                "DISTNAME": distname,
                "CHECKSUMS": _macports_checksums(source),
            }
        )
    else:
        revision = int(package["fields"]["revision"])
        values.update(
            {
                "URL": source.url,
                "VERSION": package["fields"]["version"],
                "SHA256": source.sha256,
                "REVISION_BLOCK": f"  revision {revision}\n" if revision else "",
            }
        )

    rendered = template
    for name, value in values.items():
        rendered = rendered.replace("{{" + name + "}}", value)

    leftover = sorted({match.group(0) for match in PLACEHOLDER.finditer(rendered)})
    if leftover:
        raise MacPackagingError(f"the rendered {backend} metadata keeps placeholders: {leftover}")
    return rendered


def render_to_file(
    backend: str,
    context: dict[str, Any],
    source: SourceArchive,
    *,
    profile: str,
    directory: Path,
    root: Path = REPO_ROOT,
) -> Path:
    rendered = render(backend, context, source, profile=profile, root=root)
    directory.mkdir(parents=True, exist_ok=True)
    output = directory / RENDERED_NAMES[backend]
    if output.is_symlink():
        raise MacPackagingError(f"refusing to write through a symlink: {output}")
    output.write_text(rendered, encoding="utf-8")
    return output


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--backend", choices=MACOS_BACKENDS, required=True)
    result.add_argument("--profile", choices=PROFILES, required=True)
    result.add_argument("--release-context", type=Path, required=True)
    result.add_argument("--source-url", required=True)
    result.add_argument("--source-sha256", required=True)
    result.add_argument("--source-size", type=int)
    result.add_argument("--source-rmd160")
    result.add_argument("--root", type=Path, default=REPO_ROOT)
    result.add_argument("--output-dir", type=Path)
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        context = load_release_context(arguments.release_context)
        source = resolve_source(
            url=arguments.source_url,
            sha256=arguments.source_sha256,
            size=arguments.source_size,
            rmd160=arguments.source_rmd160,
            product_version=context["product_version"],
            profile=arguments.profile,
        )
        if arguments.output_dir is None:
            sys.stdout.write(
                render(
                    arguments.backend,
                    context,
                    source,
                    profile=arguments.profile,
                    root=arguments.root,
                )
            )
        else:
            print(
                render_to_file(
                    arguments.backend,
                    context,
                    source,
                    profile=arguments.profile,
                    directory=arguments.output_dir,
                    root=arguments.root,
                )
            )
    except (MacPackagingError, PackageFrameworkError, ReleaseContextError, OSError) as error:
        print(f"macOS packaging render FAILED: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
