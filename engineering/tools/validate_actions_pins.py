#!/usr/bin/env python3
"""Require immutable Action pins and one consistent CodeQL Action revision."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

USES_RE = re.compile(r"^\s*-?\s*uses:\s*(\S+)")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
LOCAL_RE = re.compile(r"^\./")
ECOSYSTEM_RE = re.compile(
    r"^\s*-?\s*package-ecosystem:\s*github-actions\s*$", re.MULTILINE
)
DISCIPLINE_RE = re.compile(
    r"(40[- ]character|commit SHA|floating[- ]tag)",
    re.IGNORECASE,
)


def validate_dependabot(root: Path, errors: list[str]) -> None:
    path = root / ".github" / "dependabot.yml"
    if not path.is_file():
        errors.append(".github/dependabot.yml: missing Dependabot configuration")
        return
    text = path.read_text(encoding="utf-8")
    if not ECOSYSTEM_RE.search(text):
        errors.append(
            ".github/dependabot.yml: missing package-ecosystem: github-actions"
        )
    if not DISCIPLINE_RE.search(text):
        errors.append(
            ".github/dependabot.yml: missing SHA/40-character/floating-tag "
            "discipline comment"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=None)
    args = parser.parse_args()
    root = (args.root or Path(__file__).resolve().parents[2]).resolve()
    workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
    errors: list[str] = []
    pinned = 0
    codeql_refs: dict[str, list[str]] = {}
    for path in workflows:
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = USES_RE.match(line)
            if not match:
                continue
            action = match.group(1).split("#", 1)[0].strip()
            if LOCAL_RE.match(action) or action.startswith("docker://"):
                continue
            if "@" not in action:
                errors.append(f"{path.relative_to(root)}:{lineno}: uses without @ref: {action}")
                continue
            name, ref = action.rsplit("@", 1)
            if SHA_RE.fullmatch(ref):
                pinned += 1
                if name.startswith("github/codeql-action/"):
                    codeql_refs.setdefault(ref, []).append(
                        f"{path.relative_to(root)}:{lineno}"
                    )
                continue
            errors.append(
                f"{path.relative_to(root)}:{lineno}: mutable Action ref "
                f"{name}@{ref} (require 40-char commit SHA)"
            )
    if len(codeql_refs) > 1:
        revisions = "; ".join(
            f"{ref} at {', '.join(locations)}"
            for ref, locations in sorted(codeql_refs.items())
        )
        errors.append(
            "github/codeql-action steps must use the same commit SHA across "
            f"all workflows: {revisions}"
        )
    validate_dependabot(root, errors)
    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"Actions pin validation FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    print(f"Actions pin validation OK ({pinned} SHA-pinned uses across {len(workflows)} workflows)")
    print("Dependabot discipline is present")
    return 0


if __name__ == "__main__":
    sys.exit(main())
