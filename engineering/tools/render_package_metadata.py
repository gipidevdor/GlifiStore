#!/usr/bin/env python3
"""Render Debian and RPM packaging metadata from the release context.

No packaging file in this repository carries a version number: the templates
under packaging/debian/templates and packaging/rpm/templates use @TOKEN@
placeholders, and every token is resolved here from the release context
produced by engineering/tools/generate_release_context.py.

The renderer is fail-closed. It refuses a template that embeds the product
version literally, refuses a placeholder it cannot resolve, refuses to leave an
unresolved placeholder in its output, and records the resolved installation
layout so that engineering/tools/verify_package_payload.py checks the installed
files against the same paths the metadata declared.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.generate_release_context import (
    ReleaseContextError,
    load_release_context,
)
from engineering.tools.package_framework import (
    REPO_ROOT,
    PackageFrameworkError,
    digest,
    encode_json,
    utc_now,
)

PLACEHOLDER = re.compile(r"@[A-Z0-9_]+@")

MAINTAINER = "Giacomo Picchiarelli <gpicchiarelli@gmail.com>"
HOMEPAGE = "https://github.com/gpicchiarelli/GlifiStore"
LICENSE_ID = "BSD-3-Clause"
SERVICE_USER = "glifistore"
SERVICE_GROUP = "glifistore"

RUNTIME_PACKAGE = "glifistore"
DEV_PACKAGE_SUFFIX = "-dev"

# Backend layouts. Only the values a distribution genuinely decides are
# overridable on the command line; the lifecycle scripts pass what the native
# tooling reports (dpkg-architecture, rpm --eval) instead of guessing.
LAYOUTS: dict[str, dict[str, str]] = {
    "deb": {
        "prefix": "/usr",
        "bindir": "/usr/bin",
        "libdir": "/usr/lib",
        "includedir": "/usr/include",
        "datadir": "/usr/share",
        "mandir": "/usr/share/man",
        "sysconfdir": "/etc",
        "unitdir": "/lib/systemd/system",
        "statedir": "/var/lib/glifistore",
    },
    "rpm": {
        "prefix": "/usr",
        "bindir": "/usr/bin",
        "libdir": "/usr/lib64",
        "includedir": "/usr/include",
        "datadir": "/usr/share",
        "mandir": "/usr/share/man",
        "sysconfdir": "/etc",
        "unitdir": "/usr/lib/systemd/system",
        "statedir": "/var/lib/glifistore",
    },
}

OVERRIDABLE = ("libdir", "unitdir")

# template name -> (output path relative to the render root, octal mode)
DEB_OUTPUTS: dict[str, tuple[str, int]] = {
    "control.in": ("debian/control", 0o644),
    "changelog.in": ("debian/changelog", 0o644),
    "rules.in": ("debian/rules", 0o755),
    "copyright": ("debian/copyright", 0o644),
    "source-format": ("debian/source/format", 0o644),
    "runtime.install.in": ("debian/@RUNTIME_PACKAGE@.install", 0o644),
    "runtime.dirs.in": ("debian/@RUNTIME_PACKAGE@.dirs", 0o644),
    "runtime.postinst.in": ("debian/@RUNTIME_PACKAGE@.postinst", 0o755),
    "runtime.postrm.in": ("debian/@RUNTIME_PACKAGE@.postrm", 0o755),
    "library.install.in": ("debian/@LIB_PACKAGE@.install", 0o644),
    "development.install.in": ("debian/@DEV_PACKAGE@.install", 0o644),
}

RPM_OUTPUTS: dict[str, tuple[str, int]] = {
    "glifistore.spec.in": ("@RUNTIME_PACKAGE@.spec", 0o644),
}

# dh_installsystemd installs debian/<package>.<unit>.service as <unit>.service.
DEB_UNIT_OUTPUT = "debian/@RUNTIME_PACKAGE@.glifistored.service"
RPM_UNIT_OUTPUT = "glifistored.service"

SERVICE_TEMPLATE = "packaging/common/service/glifistored.service.in"
ACCOUNT_TEMPLATE = "packaging/common/service/service-account.sh.in"


class RenderError(RuntimeError):
    pass


def _debian_date(generated_at: str) -> str:
    moment = dt.datetime.fromisoformat(generated_at.replace("Z", "+00:00"))
    return moment.strftime("%a, %d %b %Y %H:%M:%S +0000")


def _rpm_date(generated_at: str) -> str:
    moment = dt.datetime.fromisoformat(generated_at.replace("Z", "+00:00"))
    return moment.strftime("%a %b %d %Y")


def _relative(path: str) -> str:
    return path.lstrip("/")


def resolve_layout(backend: str, overrides: dict[str, str] | None = None) -> dict[str, str]:
    if backend not in LAYOUTS:
        raise RenderError(f"unsupported packaging backend: {backend}")
    layout = dict(LAYOUTS[backend])
    for key, value in (overrides or {}).items():
        if key not in OVERRIDABLE:
            raise RenderError(f"{key} is not an overridable layout entry for {backend}")
        if not value.startswith("/") or value.rstrip("/") != value or ".." in value:
            raise RenderError(f"{key} must be a normalised absolute path: {value!r}")
        layout[key] = value
    return layout


def build_tokens(
    context: dict[str, Any],
    backend: str,
    layout: dict[str, str],
    *,
    maintainer: str = MAINTAINER,
) -> dict[str, str]:
    """Every placeholder any template may use, resolved from the release context."""
    package = context["package_versions"][backend]
    product_version = context["product_version"]
    abi = context["abi"]
    commit = context["git"]["commit"]
    config_file = f"{layout['sysconfdir']}/glifistore/glifistored.conf"
    library_package = f"lib{RUNTIME_PACKAGE}{abi['major']}"

    tokens: dict[str, str] = {
        "PRODUCT_VERSION": product_version,
        "PACKAGE_VERSION": package["package_version"],
        "UPSTREAM_VERSION": package["upstream_version"],
        "PACKAGE_REVISION": str(context["package_revision"]),
        "RELEASE_KIND": context["release_kind"],
        "GIT_COMMIT": commit,
        "GIT_COMMIT_SHORT": commit[:12],
        "ABI_MAJOR": str(abi["major"]),
        "ABI_MINOR": str(abi["minor"]),
        "ABI_VERSION": abi["version"],
        "SOURCE_PACKAGE": RUNTIME_PACKAGE,
        "RUNTIME_PACKAGE": RUNTIME_PACKAGE,
        "LIB_PACKAGE": library_package,
        "DEV_PACKAGE": f"lib{RUNTIME_PACKAGE}{DEV_PACKAGE_SUFFIX}",
        "SOURCE_ARCHIVE": f"GlifiStore-{product_version}.tar.xz",
        "SOURCE_DIRECTORY": f"GlifiStore-{product_version}",
        "MAINTAINER": maintainer,
        "HOMEPAGE": HOMEPAGE,
        "LICENSE_ID": LICENSE_ID,
        "SERVICE_USER": SERVICE_USER,
        "SERVICE_GROUP": SERVICE_GROUP,
        "CONFIG_FILE": config_file,
        "CONFIG_FILE_REL": _relative(config_file),
        "DEB_CHANGELOG_DATE": _debian_date(context["generated_at"]),
        "RPM_CHANGELOG_DATE": _rpm_date(context["generated_at"]),
    }
    if backend == "deb":
        tokens["DEB_PACKAGE_VERSION"] = package["package_version"]
        tokens["DEB_UPSTREAM_VERSION"] = package["fields"]["upstream_version"]
        tokens["DEB_REVISION"] = package["fields"]["debian_revision"]
    else:
        tokens["RPM_VERSION"] = package["fields"]["version"]
        tokens["RPM_RELEASE"] = package["fields"]["release"]
    for key, value in layout.items():
        tokens[key.upper()] = value
        tokens[f"{key.upper()}_REL"] = _relative(value)
    return tokens


def substitute(text: str, tokens: dict[str, str], *, origin: str) -> str:
    def replace(match: re.Match[str]) -> str:
        name = match.group(0)[1:-1]
        if name not in tokens:
            raise RenderError(f"{origin} uses an unknown placeholder @{name}@")
        return tokens[name]

    rendered = PLACEHOLDER.sub(replace, text)
    leftover = PLACEHOLDER.search(rendered)
    if leftover is not None:
        raise RenderError(f"{origin} still contains {leftover.group(0)} after rendering")
    return rendered


def _read_template(path: Path, tokens: dict[str, str]) -> str:
    if path.is_symlink() or not path.is_file():
        raise RenderError(f"missing packaging template: {path}")
    text = path.read_text(encoding="utf-8")
    version = tokens["PRODUCT_VERSION"]
    if version in text:
        raise RenderError(
            f"{path.name} embeds the product version literally; use a placeholder instead"
        )
    return text


def _account_snippet(root: Path, tokens: dict[str, str], indent: str) -> str:
    snippet = _read_template(root / ACCOUNT_TEMPLATE, tokens)
    rendered = substitute(snippet, tokens, origin=ACCOUNT_TEMPLATE).rstrip("\n")
    return "\n".join(indent + line if line.strip() else "" for line in rendered.splitlines())


def _expand_snippet(text: str, root: Path, tokens: dict[str, str]) -> str:
    """Inline the shared service-account snippet at the placeholder's indentation."""
    lines: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == "@SERVICE_ACCOUNT_SNIPPET@":
            indent = line[: len(line) - len(line.lstrip())]
            lines.append(_account_snippet(root, tokens, indent))
            continue
        if "@SERVICE_ACCOUNT_SNIPPET@" in line:
            raise RenderError("@SERVICE_ACCOUNT_SNIPPET@ must be alone on its line")
        lines.append(line)
    return "\n".join(lines) + "\n"


def render(
    context_path: Path,
    backend: str,
    output: Path,
    *,
    root: Path = REPO_ROOT,
    overrides: dict[str, str] | None = None,
    maintainer: str = MAINTAINER,
    replace: bool = False,
) -> dict[str, Any]:
    if backend not in ("deb", "rpm"):
        raise RenderError(f"no packaging metadata renderer for backend {backend!r}")
    context = load_release_context(context_path)
    layout = resolve_layout(backend, overrides)
    tokens = build_tokens(context, backend, layout, maintainer=maintainer)

    if output.exists() and not replace:
        raise RenderError(f"refusing to render into an existing directory: {output}")

    template_root = root / "packaging" / ("debian" if backend == "deb" else "rpm") / "templates"
    outputs = DEB_OUTPUTS if backend == "deb" else RPM_OUTPUTS
    unit_output = DEB_UNIT_OUTPUT if backend == "deb" else RPM_UNIT_OUTPUT

    plan: list[tuple[Path, str, int]] = []
    for template_name, (relative_output, mode) in sorted(outputs.items()):
        text = _read_template(template_root / template_name, tokens)
        rendered = substitute(
            _expand_snippet(text, root, tokens), tokens, origin=template_name
        )
        target = substitute(relative_output, tokens, origin=f"{template_name} output name")
        plan.append((output / target, rendered, mode))

    unit_text = _read_template(root / SERVICE_TEMPLATE, tokens)
    plan.append(
        (
            output / substitute(unit_output, tokens, origin="unit output name"),
            substitute(unit_text, tokens, origin=SERVICE_TEMPLATE),
            0o644,
        )
    )

    files: list[dict[str, Any]] = []
    for target, text, mode in plan:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_symlink():
            raise RenderError(f"refusing to write through a symlink: {target}")
        target.write_text(text, encoding="utf-8")
        target.chmod(mode)
        files.append(
            {
                "path": str(target.relative_to(output)),
                "mode": f"{mode:04o}",
                "sha256": digest(target),
            }
        )

    manifest = {
        "schema_version": 1,
        "backend": backend,
        "generated_at": utc_now(),
        "git_sha": context["git"]["commit"],
        "product_version": context["product_version"],
        "package_version": context["package_versions"][backend]["package_version"],
        "package_revision": context["package_revision"],
        "layout": layout,
        "packages": {
            "runtime": tokens["RUNTIME_PACKAGE"],
            "library": tokens["LIB_PACKAGE"],
            "development": tokens["DEV_PACKAGE"],
        },
        "tokens": tokens,
        "files": sorted(files, key=lambda entry: entry["path"]),
    }
    (output / "rendered-metadata.json").write_text(encode_json(manifest), encoding="utf-8")
    # The lifecycle scripts run in containers that have no Python before their
    # bootstrap step, so the same tokens are also exported as shell assignments.
    (output / "layout.env").write_text(_shell_environment(tokens), encoding="utf-8")
    return manifest


def _shell_environment(tokens: dict[str, str]) -> str:
    lines = [
        "# Generated by engineering/tools/render_package_metadata.py. Do not edit.",
        "# Sourced by scripts/packaging/linux-package-lifecycle.sh.",
    ]
    for name, value in sorted(tokens.items()):
        if "\n" in value:
            raise RenderError(f"token {name} spans multiple lines and cannot be exported")
        lines.append(f"GS_PKG_{name}={_shell_quote(value)}")
    return "\n".join(lines) + "\n"


def _shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\\''") + "'"


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--backend", choices=("deb", "rpm"), required=True)
    result.add_argument("--release-context", type=Path, required=True)
    result.add_argument("--output", type=Path, required=True)
    result.add_argument("--root", type=Path, default=REPO_ROOT)
    result.add_argument("--libdir")
    result.add_argument("--unitdir")
    result.add_argument("--maintainer", default=MAINTAINER)
    result.add_argument("--replace", action="store_true")
    return result


def main() -> int:
    arguments = parser().parse_args()
    overrides = {
        key: value
        for key, value in (("libdir", arguments.libdir), ("unitdir", arguments.unitdir))
        if value
    }
    try:
        manifest = render(
            arguments.release_context,
            arguments.backend,
            arguments.output,
            root=arguments.root,
            overrides=overrides,
            maintainer=arguments.maintainer,
            replace=arguments.replace,
        )
    except (PackageFrameworkError, ReleaseContextError, RenderError, OSError) as error:
        print(f"package metadata render FAILED: {error}", file=sys.stderr)
        return 1
    print(
        f"package metadata OK backend={manifest['backend']} "
        f"package_version={manifest['package_version']} files={len(manifest['files'])}"
    )
    print(json.dumps(manifest["layout"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
