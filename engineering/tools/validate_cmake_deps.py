#!/usr/bin/env python3
"""Validate Phase C CMake layout, include dependency matrix and installed header closure."""

from __future__ import annotations

import argparse
import fnmatch
import posixpath
import re
import shlex
import sys
from collections import deque
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("PyYAML required", file=sys.stderr)
    sys.exit(2)

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')
ANY_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*(?:"([^"]+)"|<([^>]+)>)')
BRACKET_COMMENT_RE = re.compile(r"#\[(=*)\[.*?\]\1\]", re.DOTALL)
# CMake command names are case-insensitive; the FILES keyword is not.
INSTALL_FILES_RE = re.compile(r"\b(?i:install)\s*\(\s*FILES\b(.*?)\)", re.DOTALL)
INSTALL_KEYWORDS = frozenset(
    {
        "CONFIGURATIONS",
        "COMPONENT",
        "DESTINATION",
        "EXCLUDE_FROM_ALL",
        "OPTIONAL",
        "PERMISSIONS",
        "RENAME",
        "TYPE",
    }
)
INSTALL_INCLUDE_DIR = "${CMAKE_INSTALL_INCLUDEDIR}"
PUBLIC_INCLUDE_ROOT = "include/"
# Each inventory below lists the installed headers independently of CMakeLists.txt.
PAYLOAD_INVENTORY = "packaging/common/file-lists/payload.yaml"
PAYLOAD_INCLUDE_TOKEN = "@includedir@/"
PLIST_INVENTORIES = ("packaging/freebsd/pkg-plist", "packaging/openbsd/pkg/PLIST")


def iter_sources(root: Path, pattern: str) -> list[Path]:
    if "{" in pattern and "}" in pattern:
        pre, rest = pattern.split("{", 1)
        body, post = rest.split("}", 1)
        paths: list[Path] = []
        for alt in body.split(","):
            paths.extend(iter_sources(root, f"{pre}{alt}{post}"))
        return paths
    return [
        path
        for path in root.rglob("*")
        if path.is_file() and fnmatch.fnmatch(path.relative_to(root).as_posix(), pattern)
    ]


def strip_cmake_comments(text: str) -> str:
    """Drop CMake line and bracket comments; a '#' inside a quoted argument is kept."""
    text = BRACKET_COMMENT_RE.sub("", text)
    kept: list[str] = []
    quoted = False
    in_comment = False
    index = 0
    while index < len(text):
        char = text[index]
        if in_comment:
            if char == "\n":
                in_comment = False
                kept.append(char)
        elif char == "\\":
            kept.append(text[index : index + 2])
            index += 1
        elif char == '"':
            quoted = not quoted
            kept.append(char)
        elif char == "#" and not quoted:
            in_comment = True
        else:
            kept.append(char)
        index += 1
    return "".join(kept)


def installed_headers(cmake_text: str) -> tuple[list[str], list[str]]:
    """Return the headers CMake installs below the include directory, plus layout errors.

    A header keeps its path below include/ at install time, because installed code includes
    it as "glifistore/<dir>/<name>". Only literal install(FILES ...) rules are understood;
    a different mechanism must teach this validator before it can replace them.
    """
    headers: list[str] = []
    errors: list[str] = []
    for match in INSTALL_FILES_RE.finditer(strip_cmake_comments(cmake_text)):
        tokens = shlex.split(match.group(1))
        keyword_positions = [index for index, token in enumerate(tokens) if token in INSTALL_KEYWORDS]
        files = tokens[: keyword_positions[0]] if keyword_positions else tokens
        destination = ""
        if "DESTINATION" in tokens:
            position = tokens.index("DESTINATION") + 1
            destination = tokens[position] if position < len(tokens) else ""
        if not destination.startswith(INSTALL_INCLUDE_DIR):
            continue
        for source in files:
            if not source.startswith(PUBLIC_INCLUDE_ROOT):
                errors.append(
                    f"install(FILES) puts {source} below {INSTALL_INCLUDE_DIR} "
                    "but it is not under include/"
                )
                continue
            directory = posixpath.dirname(source[len(PUBLIC_INCLUDE_ROOT) :])
            expected = f"{INSTALL_INCLUDE_DIR}/{directory}" if directory else INSTALL_INCLUDE_DIR
            if destination != expected:
                errors.append(
                    f"install(FILES) installs {source} to {destination}, "
                    f"but its include path requires {expected}"
                )
            headers.append(source)
    return headers, errors


def header_closure_errors(root: Path, headers: list[str]) -> list[str]:
    """Report headers that an installed header includes but CMake does not install.

    An installed header is compiled against the install prefix alone, so every project
    header it reaches transitively must be installed too. System headers (angle brackets
    that do not name a file below include/) are outside this contract.
    """
    if not headers:
        message = (
            f"root CMakeLists.txt installs no header below {INSTALL_INCLUDE_DIR} "
            "with install(FILES ...); the installed header closure cannot be checked"
        )
        return [message]
    errors: list[str] = []
    installed = set(headers)
    seen = set(headers)
    pending = deque(sorted(installed))
    while pending:
        header = pending.popleft()
        path = root / header
        if not path.is_file():
            errors.append(f"install(FILES) lists {header}, which does not exist")
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            match = ANY_INCLUDE_RE.match(line)
            if not match:
                continue
            quoted, angled = match.groups()
            name = quoted or angled
            target = root / PUBLIC_INCLUDE_ROOT / name
            if not target.is_file() and quoted:
                target = path.parent / name
            if not target.is_file():
                if quoted:
                    errors.append(
                        f'{header} includes "{name}", which is not a public header below include/ '
                        "and so cannot be part of the installed API"
                    )
                continue
            included = target.relative_to(root).as_posix()
            if included in seen:
                continue
            seen.add(included)
            pending.append(included)
            if included not in installed:
                errors.append(
                    f"{header} includes {name}, but root CMakeLists.txt does not install {included}"
                )
    return errors


def inventory_errors(root: Path, headers: list[str]) -> list[str]:
    """Compare the headers CMake installs with the package inventories that list them.

    The Debian/RPM payload inventory and the FreeBSD/OpenBSD plists each enumerate the
    installed headers by hand; a header missing from a plist breaks the native port build.
    """
    expected = {header[len(PUBLIC_INCLUDE_ROOT) :] for header in headers}
    errors: list[str] = []
    listed: dict[str, set[str]] = {}
    for relative in (PAYLOAD_INVENTORY, *PLIST_INVENTORIES):
        path = root / relative
        if not path.is_file():
            errors.append(f"missing package inventory: {relative}")
            continue
        text = path.read_text(encoding="utf-8")
        if relative == PAYLOAD_INVENTORY:
            inventory = yaml.safe_load(text) or {}
            listed[relative] = {
                str(entry["path"])[len(PAYLOAD_INCLUDE_TOKEN) :]
                for component in inventory.get("components") or []
                for entry in component.get("entries") or []
                if str(entry.get("path", "")).startswith(PAYLOAD_INCLUDE_TOKEN)
            }
        else:
            listed[relative] = {
                line[len(PUBLIC_INCLUDE_ROOT) :]
                for line in text.splitlines()
                if line.startswith(PUBLIC_INCLUDE_ROOT)
            }
    for relative, names in listed.items():
        errors.extend(
            f"{relative} does not list installed header {name}" for name in sorted(expected - names)
        )
        errors.extend(
            f"{relative} lists header {name}, which root CMakeLists.txt does not install"
            for name in sorted(names - expected)
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=None)
    args = parser.parse_args()
    root = (args.root or Path(__file__).resolve().parents[2]).resolve()
    matrix_path = root / "engineering" / "build" / "dependency-matrix.yaml"
    matrix = yaml.safe_load(matrix_path.read_text(encoding="utf-8"))

    errors: list[str] = []
    for rel in matrix.get("required_cmake_files", []):
        if not (root / rel).is_file():
            errors.append(f"missing required CMake file: {rel}")
    cmake_root = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    for sub in ("src", "tools", "tests", "benchmarks", "fuzz"):
        if f"add_subdirectory({sub})" not in cmake_root:
            errors.append(f"root CMakeLists.txt missing add_subdirectory({sub})")

    for rule in matrix.get("include_rules", []):
        rid = rule["id"]
        allow = set(rule.get("allow_include_prefix", []))
        forbid = rule["forbid_include_prefix"]
        for path in iter_sources(root, rule["from_glob"]):
            if path.suffix not in {".hpp", ".h", ".cpp", ".cc", ".cxx"}:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for line in text.splitlines():
                match = INCLUDE_RE.match(line)
                if not match:
                    continue
                inc = match.group(1)
                if inc in allow or any(inc.startswith(a) for a in allow if a.endswith("/")):
                    continue
                if inc == forbid or inc.startswith(forbid):
                    errors.append(
                        f"{rid}: {path.relative_to(root).as_posix()} includes forbidden '{inc}'"
                    )

    required_aliases = {"GlifiStore::core", "GlifiStore::server", "GlifiStore::client"}
    found: set[str] = set()
    for spec in matrix.get("targets", {}).values():
        found.update(spec.get("aliases", []))
    for alias in sorted(required_aliases - found):
        errors.append(f"dependency matrix missing required alias {alias}")

    headers, install_errors = installed_headers(cmake_root)
    errors.extend(install_errors)
    errors.extend(header_closure_errors(root, headers))
    errors.extend(inventory_errors(root, headers))

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"CMake dependency validation FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    print(
        f"CMake dependency validation OK "
        f"({len(matrix.get('targets', {}))} targets, "
        f"{len(matrix.get('include_rules', []))} include rules, "
        f"{len(headers)} installed headers closed under #include)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
