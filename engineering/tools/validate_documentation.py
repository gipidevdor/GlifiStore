#!/usr/bin/env python3
"""Validate repository-local links in every tracked Markdown document."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit


INLINE_LINK = re.compile(
    r"!?\[[^\]]*\]\(\s*(?P<target><[^>\n]+>|[^\s)]+)"
    r"(?:\s+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?\s*\)"
)
REFERENCE_LINK = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(?P<target><[^>\n]+>|\S+)")
FENCE = re.compile(r"^\s{0,3}(?P<marker>`{3,}|~{3,})")


@dataclass(frozen=True)
class DocumentationIssue:
    source: Path
    line: int
    target: str
    reason: str


@dataclass(frozen=True)
class DocumentationReport:
    files: int
    local_links: int
    issues: tuple[DocumentationIssue, ...]


def tracked_markdown_files(root: Path) -> list[Path]:
    completed = subprocess.run(
        ["git", "ls-files", "-z", "--", "*.md"],
        cwd=root,
        check=True,
        capture_output=True,
    )
    return [Path(item.decode()) for item in completed.stdout.split(b"\0") if item]


def _destinations(text: str) -> list[tuple[int, str]]:
    destinations: list[tuple[int, str]] = []
    fence_marker: str | None = None
    fence_length = 0

    for line_number, line in enumerate(text.splitlines(), 1):
        fence = FENCE.match(line)
        if fence:
            marker = fence.group("marker")
            if fence_marker is None:
                fence_marker = marker[0]
                fence_length = len(marker)
            elif marker[0] == fence_marker and len(marker) >= fence_length:
                fence_marker = None
                fence_length = 0
            continue
        if fence_marker is not None:
            continue

        destinations.extend((line_number, match.group("target")) for match in INLINE_LINK.finditer(line))
        reference = REFERENCE_LINK.match(line)
        if reference:
            destinations.append((line_number, reference.group("target")))

    return destinations


def _local_path(target: str) -> str | None:
    destination = target[1:-1] if target.startswith("<") and target.endswith(">") else target
    parsed = urlsplit(destination)
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    return unquote(parsed.path)


def _has_case_mismatch(root: Path, candidate: Path) -> bool:
    normalized = Path(os.path.normpath(candidate))
    current = root
    try:
        parts = normalized.relative_to(root).parts
    except ValueError:
        return False
    for part in parts:
        try:
            names = [entry.name for entry in current.iterdir()]
        except OSError:
            return False
        if part in names:
            current /= part
            continue
        case_matches = [name for name in names if name.casefold() == part.casefold()]
        if case_matches:
            return True
        return False
    return False


def validate_documentation(root: Path, markdown_files: list[Path] | None = None) -> DocumentationReport:
    root = root.resolve()
    files = tracked_markdown_files(root) if markdown_files is None else markdown_files
    issues: list[DocumentationIssue] = []
    local_links = 0

    for relative_source in files:
        source = root / relative_source
        if not source.is_file():
            issues.append(DocumentationIssue(relative_source, 0, str(relative_source), "tracked file is missing"))
            continue
        try:
            text = source.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            issues.append(DocumentationIssue(relative_source, 0, str(relative_source), "not valid UTF-8"))
            continue

        for line_number, raw_target in _destinations(text):
            local = _local_path(raw_target)
            if local is None:
                continue
            local_links += 1
            candidate = source.parent / local
            resolved = candidate.resolve(strict=False)
            try:
                resolved.relative_to(root)
            except ValueError:
                issues.append(
                    DocumentationIssue(relative_source, line_number, raw_target, "target escapes repository")
                )
                continue
            if _has_case_mismatch(root, candidate):
                issues.append(DocumentationIssue(relative_source, line_number, raw_target, "path case does not match"))
            elif not resolved.exists():
                issues.append(DocumentationIssue(relative_source, line_number, raw_target, "target does not exist"))

    return DocumentationReport(len(files), local_links, tuple(issues))


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    report = validate_documentation(root)
    if report.issues:
        for issue in report.issues:
            print(f"{issue.source}:{issue.line}: {issue.reason}: {issue.target}")
        print(
            f"Documentation validation FAILED "
            f"({report.files} Markdown files, {report.local_links} local links, {len(report.issues)} issue(s))"
        )
        return 1
    print(f"Documentation validation OK ({report.files} Markdown files, {report.local_links} local links)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
