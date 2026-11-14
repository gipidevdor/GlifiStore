#!/usr/bin/env python3
"""Prove that a macOS package links only inside its own package-manager prefix.

A MacPorts port that links a Homebrew or /usr/local library, or a Homebrew
formula that links a MacPorts library, produces a binary that breaks as soon as
the foreign prefix changes. The check is therefore fail-closed: any foreign link
is a defect unless an exception is written down, with a reason, in the backend's
prefix-exceptions file.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

if __package__ in (None, ""):  # direct script execution
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from engineering.tools.package_framework import REPO_ROOT

FOREIGN_PREFIXES = {
    "macports": ("/opt/homebrew", "/usr/local"),
    "homebrew": ("/opt/local",),
}
EXCEPTIONS_FILE = "prefix-exceptions.txt"
# otool -L prints the install name on the first line and then one dependency per
# indented line, each followed by its compatibility/current version.
OTOOL_DEPENDENCY = re.compile(r"^\s+(\S+)\s+\(compatibility version")
RELATIVE_LOADER_PREFIXES = ("@rpath/", "@loader_path/", "@executable_path/")


class PrefixIsolationError(RuntimeError):
    pass


@dataclass(frozen=True)
class ForeignLink:
    subject: str
    dependency: str
    prefix: str

    def __str__(self) -> str:
        return f"{self.subject} links {self.dependency} from the foreign prefix {self.prefix}"


def parse_otool(output: str) -> list[str]:
    """Dependency install names from `otool -L`, ignoring loader-relative ones."""
    dependencies: list[str] = []
    for line in output.splitlines():
        match = OTOOL_DEPENDENCY.match(line)
        if match is None:
            continue
        name = match.group(1)
        if name.startswith(RELATIVE_LOADER_PREFIXES):
            continue
        dependencies.append(name)
    return dependencies


def load_exceptions(backend: str, root: Path = REPO_ROOT) -> dict[str, str]:
    """Documented foreign links: `<install name> # <reason>`, reason mandatory."""
    path = root / "packaging" / backend / EXCEPTIONS_FILE
    if path.is_symlink() or not path.is_file():
        return {}
    exceptions: dict[str, str] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        name, separator, reason = text.partition("#")
        if not separator or not name.strip() or not reason.strip():
            raise PrefixIsolationError(
                f"{path}:{number}: an exception must be written as '<install name> # <reason>'"
            )
        exceptions[name.strip()] = reason.strip()
    return exceptions


def foreign_links(
    backend: str, subject: str, dependencies: list[str], exceptions: dict[str, str]
) -> list[ForeignLink]:
    if backend not in FOREIGN_PREFIXES:
        raise PrefixIsolationError(f"no prefix isolation policy for backend {backend!r}")
    found: list[ForeignLink] = []
    for dependency in dependencies:
        if dependency in exceptions:
            continue
        for prefix in FOREIGN_PREFIXES[backend]:
            if dependency == prefix or dependency.startswith(prefix + "/"):
                found.append(ForeignLink(subject, dependency, prefix))
                break
    return found


def inspect(
    backend: str,
    paths: list[Path],
    *,
    root: Path = REPO_ROOT,
    otool: str = "otool",
) -> list[ForeignLink]:
    if not paths:
        raise PrefixIsolationError("prefix isolation needs at least one installed file")
    exceptions = load_exceptions(backend, root)
    violations: list[ForeignLink] = []
    for path in paths:
        if path.is_symlink() or not path.is_file():
            raise PrefixIsolationError(f"installed file is missing: {path}")
        try:
            completed = subprocess.run(
                [otool, "-L", str(path)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=120,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise PrefixIsolationError(f"cannot run {otool}: {error}") from error
        if completed.returncode != 0:
            raise PrefixIsolationError(
                f"{otool} -L failed for {path}: {completed.stderr.strip() or completed.returncode}"
            )
        violations.extend(
            foreign_links(backend, str(path), parse_otool(completed.stdout), exceptions)
        )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=sorted(FOREIGN_PREFIXES), required=True)
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--otool", default="otool")
    parser.add_argument("paths", type=Path, nargs="+")
    arguments = parser.parse_args()
    try:
        violations = inspect(
            arguments.backend, arguments.paths, root=arguments.root, otool=arguments.otool
        )
    except (PrefixIsolationError, OSError) as error:
        print(f"prefix isolation FAILED: {error}", file=sys.stderr)
        return 1
    for violation in violations:
        print(f"prefix isolation violation: {violation}", file=sys.stderr)
    if violations:
        print(
            f"prefix isolation FAILED: {len(violations)} foreign link(s) for {arguments.backend}",
            file=sys.stderr,
        )
        return 1
    forbidden = ", ".join(FOREIGN_PREFIXES[arguments.backend])
    print(
        f"prefix isolation OK ({len(arguments.paths)} file(s); no link into {forbidden})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
