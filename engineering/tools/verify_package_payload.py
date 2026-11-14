#!/usr/bin/env python3
"""Check an installed or staged package payload against the shared inventory.

packaging/common/file-lists/payload.yaml is the single description of what a
GlifiStore package delivers. This tool resolves its @token@ placeholders from
the layout recorded by engineering/tools/render_package_metadata.py and proves,
component by component, that the declared entries are present with the declared
kind.

The same inventory drives the removal check: components named with
--absent-component must have disappeared, which is how the configuration and
data retention policy in packaging/common/config-data-policy.md is verified
instead of asserted.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.package_framework import (
    REPO_ROOT,
    PackageFrameworkError,
    load_yaml,
    read_json,
    require_exact_keys,
)

PAYLOAD_PATH = REPO_ROOT / "packaging" / "common" / "file-lists" / "payload.yaml"
TOKEN = re.compile(r"@([a-z0-9_]+)@")
KINDS = ("file", "executable", "shared_library", "directory", "glob")


class PayloadError(RuntimeError):
    pass


def load_payload(path: Path = PAYLOAD_PATH) -> dict[str, Any]:
    value = load_yaml(path)
    require_exact_keys(value, {"version", "tokens", "components"}, "payload inventory")
    if value["version"] != 1:
        raise PayloadError("unsupported payload inventory version")
    declared = value["tokens"]
    if not isinstance(declared, list) or not declared:
        raise PayloadError("payload inventory declares no tokens")
    components = value["components"]
    if not isinstance(components, list) or not components:
        raise PayloadError("payload inventory declares no components")
    seen: set[str] = set()
    for component in components:
        require_exact_keys(component, {"id", "description", "entries"}, "payload component")
        identifier = component["id"]
        if identifier in seen:
            raise PayloadError(f"payload inventory duplicates component {identifier}")
        seen.add(identifier)
        entries = component["entries"]
        if not isinstance(entries, list) or not entries:
            raise PayloadError(f"payload component {identifier} declares no entries")
        for entry in entries:
            require_exact_keys(entry, {"path", "kind"}, f"payload entry in {identifier}")
            if entry["kind"] not in KINDS:
                raise PayloadError(f"unsupported payload kind: {entry['kind']}")
            if not entry["path"].startswith("@") and not entry["path"].startswith("/"):
                raise PayloadError(f"payload paths must be absolute: {entry['path']}")
            unknown = sorted(set(TOKEN.findall(entry["path"])) - set(declared))
            if unknown:
                raise PayloadError(f"payload entry uses undeclared tokens: {unknown}")
            if ".." in entry["path"]:
                raise PayloadError(f"payload paths must be normalised: {entry['path']}")
    return value


def tokens_from_layout(path: Path) -> dict[str, str]:
    """Read the layout the metadata renderer actually used, not a second guess."""
    manifest = read_json(path)
    layout = manifest.get("layout")
    rendered = manifest.get("tokens")
    if not isinstance(layout, dict) or not isinstance(rendered, dict):
        raise PayloadError(f"not a rendered package metadata manifest: {path}")
    tokens = {key: str(value) for key, value in layout.items()}
    for name in ("ABI_MAJOR", "ABI_VERSION"):
        if name not in rendered:
            raise PayloadError(f"rendered metadata misses {name}")
        tokens[name.lower()] = str(rendered[name])
    return tokens


def resolve(path: str, tokens: dict[str, str]) -> str:
    def replace(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in tokens:
            raise PayloadError(f"no value for payload token @{name}@")
        return tokens[name]

    resolved = TOKEN.sub(replace, path)
    if not resolved.startswith("/"):
        raise PayloadError(f"resolved payload path is not absolute: {resolved}")
    return resolved


def component(payload: dict[str, Any], identifier: str) -> dict[str, Any]:
    for entry in payload["components"]:
        if entry["id"] == identifier:
            return entry
    raise PayloadError(f"unknown payload component: {identifier}")


def _matches(root: Path, resolved: str, kind: str) -> list[Path]:
    relative = resolved.lstrip("/")
    if kind == "glob":
        return sorted(root.glob(relative))
    candidate = root / relative
    return [candidate] if (candidate.exists() or candidate.is_symlink()) else []


def _kind_failure(path: Path, kind: str) -> str | None:
    if kind == "directory":
        return None if path.is_dir() else "not a directory"
    if path.is_dir():
        return "is a directory"
    if not path.is_file():
        return "not a regular file"
    if kind == "executable" and not path.stat().st_mode & 0o111:
        return "not executable"
    return None


def verify(
    root: Path,
    tokens: dict[str, str],
    *,
    present: list[str],
    absent: list[str],
    payload: dict[str, Any] | None = None,
) -> list[str]:
    inventory = payload if payload is not None else load_payload()
    if not present and not absent:
        raise PayloadError("choose at least one --component or --absent-component")
    overlap = sorted(set(present) & set(absent))
    if overlap:
        raise PayloadError(f"components cannot be required present and absent: {overlap}")

    failures: list[str] = []
    for identifier in present:
        for entry in component(inventory, identifier)["entries"]:
            resolved = resolve(entry["path"], tokens)
            found = _matches(root, resolved, entry["kind"])
            if not found:
                failures.append(f"{identifier}: missing {resolved}")
                print(f"MISSING  {identifier:<13} {resolved}")
                continue
            problem = _kind_failure(found[0], entry["kind"])
            if problem is not None:
                failures.append(f"{identifier}: {resolved} -> {problem}")
                print(f"WRONGKIND {identifier:<12} {resolved} ({problem})")
                continue
            suffix = f" ({len(found)} match(es))" if entry["kind"] == "glob" else ""
            print(f"OK       {identifier:<13} {resolved}{suffix}")

    for identifier in absent:
        for entry in component(inventory, identifier)["entries"]:
            resolved = resolve(entry["path"], tokens)
            found = _matches(root, resolved, entry["kind"])
            if found:
                failures.append(f"{identifier}: still present {resolved}")
                print(f"PRESENT  {identifier:<13} {resolved}")
            else:
                print(f"REMOVED  {identifier:<13} {resolved}")
    return failures


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--payload", type=Path, default=PAYLOAD_PATH)
    result.add_argument("--root", type=Path, default=Path("/"))
    result.add_argument("--layout", type=Path, help="rendered-metadata.json from the renderer")
    result.add_argument("--token", action="append", default=[], metavar="NAME=VALUE")
    result.add_argument("--component", action="append", default=[])
    result.add_argument("--absent-component", action="append", default=[])
    return result


def main() -> int:
    arguments = parser().parse_args()
    try:
        tokens = tokens_from_layout(arguments.layout) if arguments.layout else {}
        for item in arguments.token:
            name, separator, value = item.partition("=")
            if not separator or not name.strip() or not value.strip():
                raise PayloadError(f"tokens must be given as NAME=VALUE: {item!r}")
            tokens[name.strip()] = value.strip()
        payload = load_payload(arguments.payload)
        failures = verify(
            arguments.root.resolve(),
            tokens,
            present=arguments.component,
            absent=arguments.absent_component,
            payload=payload,
        )
    except (PackageFrameworkError, PayloadError, OSError) as error:
        print(f"package payload FAILED: {error}", file=sys.stderr)
        return 1
    if failures:
        print(f"package payload FAILED: {len(failures)} problem(s)", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("package payload OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
