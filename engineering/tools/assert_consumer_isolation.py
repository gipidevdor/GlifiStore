#!/usr/bin/env python3
"""Refuse an external consumer build that leaked back into the source checkout.

A packaged consumer proves nothing if it compiled against the repository it was
supposed to be independent from. This tool scans the consumer's compile commands,
build logs and installed metadata (pkg-config, CMake package files) and fails
when any include, library, rpath or sysroot path resolves inside a forbidden
root.

GITHUB_WORKSPACE is treated as forbidden automatically, so a CI job cannot pass
by simply forgetting to declare it. Running with no forbidden root at all is an
error rather than a silent success.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections.abc import Callable
from pathlib import Path

# Compiler and linker flags that introduce a search path.
PATH_FLAGS = ("-I", "-isystem", "-iquote", "-idirafter", "-L", "-F", "-B")
PREFIXED = (
    re.compile(r"--sysroot[=\s]+(\S+)"),
    re.compile(r"-Wl,-rpath[,=](\S+)"),
    re.compile(r"-Wl,-rpath-link[,=](\S+)"),
    re.compile(r"-isysroot\s+(\S+)"),
)
FLAG_PATTERN = re.compile(
    r"(?<![\w/.-])(" + "|".join(re.escape(flag) for flag in PATH_FLAGS) + r")\s*([^\s\"']+)"
)
# Path-like tokens are compared after normalisation, so a directory whose name
# merely ends in a forbidden root (".../external-consumer/src" against "/src")
# is not mistaken for a leak.
ABSOLUTE_PATH = re.compile(r"/[A-Za-z0-9._+@-]+(?:/[A-Za-z0-9._+@-]+)*")
ENVIRONMENT_ROOTS = ("GITHUB_WORKSPACE",)


class IsolationError(RuntimeError):
    pass


def environment_roots() -> list[str]:
    return [os.environ[name] for name in ENVIRONMENT_ROOTS if os.environ.get(name)]


def normalise_roots(roots: list[str]) -> list[str]:
    normalised: list[str] = []
    for root in roots:
        if not root.startswith("/"):
            raise IsolationError(f"forbidden roots must be absolute paths: {root!r}")
        candidate = os.path.normpath(root).rstrip("/")
        if candidate in ("", "/"):
            raise IsolationError("'/' cannot be a forbidden root; the whole prefix lives there")
        if candidate not in normalised:
            normalised.append(candidate)
    if not normalised:
        raise IsolationError("no forbidden root was given; isolation cannot be proven")
    return normalised


def inside(path: str, roots: list[str], *, base: str | None = None) -> str | None:
    candidate = path.strip().strip("\"'")
    if not candidate:
        return None
    if not candidate.startswith("/"):
        if base is None:
            return None
        candidate = os.path.join(base, candidate)
    candidate = os.path.normpath(candidate)
    for root in roots:
        if candidate == root or candidate.startswith(root + "/"):
            return root
    return None


def scan_text(text: str, roots: list[str], *, origin: str) -> list[str]:
    """Every absolute path in the text is resolved; a leaked root is a violation.

    A path reached through a search-path flag is reported as such, because that is
    the actionable form; the same path is not also reported as a bare mention.
    """
    # Keyed by offending path so one leak is one violation, with the most
    # specific message winning: flag forms are recorded before bare mentions.
    findings: dict[str, str] = {}

    def add(path: str, describe: Callable[[str, str], str]) -> None:
        root = inside(path, roots)
        if root is not None:
            normalised = os.path.normpath(path.strip().strip("\"'"))
            findings.setdefault(normalised, describe(normalised, root))

    for match in FLAG_PATTERN.finditer(text):
        flag = match.group(1)
        add(match.group(2), lambda p, r: f"{origin}: {flag} search path inside {r}: {p}")
    for pattern in PREFIXED:
        for match in pattern.finditer(text):
            whole = match.group(0)
            add(match.group(1), lambda p, r: f"{origin}: {whole} resolves inside {r}")
    for match in ABSOLUTE_PATH.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        add(match.group(0), lambda p, r: f"{origin}:{line}: {p} is inside {r}")
    return sorted(findings.values())


def scan_compile_commands(path: Path, roots: list[str]) -> list[str]:
    try:
        entries = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise IsolationError(f"invalid compile commands {path}: {error}") from error
    if not isinstance(entries, list) or not entries:
        raise IsolationError(f"compile commands must be a non-empty array: {path}")
    findings: list[str] = []
    for entry in entries:
        if not isinstance(entry, dict):
            raise IsolationError(f"malformed compile command entry in {path}")
        directory = entry.get("directory")
        arguments = entry.get("arguments")
        if arguments is None:
            command = entry.get("command")
            if not isinstance(command, str):
                raise IsolationError(f"compile command entry has no arguments: {path}")
            arguments = command.split()
        origin = f"{path.name}[{entry.get('file', '?')}]"
        for index, argument in enumerate(arguments):
            flagged = False
            for flag in PATH_FLAGS:
                if argument == flag and index + 1 < len(arguments):
                    value = arguments[index + 1]
                elif argument.startswith(flag) and len(argument) > len(flag):
                    value = argument[len(flag) :]
                else:
                    continue
                flagged = True
                # A relative search path is resolved against the compilation
                # directory, which is where an in-tree include actually hides.
                root = inside(value, roots, base=directory)
                if root is not None:
                    resolved = os.path.normpath(os.path.join(directory or "", value))
                    findings.append(f"{origin}: {flag} search path inside {root}: {resolved}")
            if not flagged:
                findings.extend(scan_text(argument, roots, origin=origin))
    return sorted(set(findings))


def assert_isolated(
    files: list[Path],
    roots: list[str],
    *,
    compile_commands: Path | None = None,
) -> list[str]:
    resolved = normalise_roots(roots)
    findings: list[str] = []
    if compile_commands is not None:
        if compile_commands.is_symlink() or not compile_commands.is_file():
            raise IsolationError(f"missing compile commands: {compile_commands}")
        findings.extend(scan_compile_commands(compile_commands, resolved))
    for path in files:
        if path.is_symlink() or not path.is_file():
            raise IsolationError(f"missing file to scan: {path}")
        text = path.read_text(encoding="utf-8", errors="replace")
        findings.extend(scan_text(text, resolved, origin=path.name))
    return sorted(set(findings))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", type=Path, nargs="*", help="build logs and installed metadata")
    parser.add_argument("--forbidden-root", action="append", default=[])
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument(
        "--no-environment-roots",
        action="store_true",
        help="do not add GITHUB_WORKSPACE to the forbidden roots",
    )
    arguments = parser.parse_args()
    roots = list(arguments.forbidden_root)
    if not arguments.no_environment_roots:
        roots.extend(environment_roots())
    try:
        findings = assert_isolated(
            arguments.files, roots, compile_commands=arguments.compile_commands
        )
    except (IsolationError, OSError) as error:
        print(f"consumer isolation FAILED: {error}", file=sys.stderr)
        return 1
    if findings:
        print(f"consumer isolation FAILED: {len(findings)} contamination(s)", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    scanned = len(arguments.files) + (1 if arguments.compile_commands else 0)
    print(f"consumer isolation OK ({scanned} artefact(s), {len(normalise_roots(roots))} root(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
