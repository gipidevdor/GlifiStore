#!/usr/bin/env python3
"""Structural debt gates: file size, TODO/FIXME issue IDs, waiver expiry."""

from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("PyYAML required", file=sys.stderr)
    sys.exit(2)

ROOT = Path(__file__).resolve().parents[2]
WAIVER_DIR = ROOT / "engineering" / "waivers"
THRESHOLDS = ROOT / "engineering" / "build" / "structure-thresholds.yaml"

TODO_RE = re.compile(r"\b(TODO|FIXME|HACK|XXX)\b(?:\s*\(([^)]+)\))?", re.IGNORECASE)
ISSUE_RE = re.compile(r"^(?:#?\d+|GS-[A-Z]+-[A-Z0-9]+-\d{3}|WAV-\d{3}|https?://\S+)$")


def load_thresholds() -> dict:
    if THRESHOLDS.is_file():
        return yaml.safe_load(THRESHOLDS.read_text(encoding="utf-8"))
    return {
        "max_lines_per_source": 1200,
        "todo_requires_tracker": True,
        "source_globs": ["src/**/*.{cpp,hpp}", "include/**/*.hpp", "tests/**/*.{cpp,hpp}"],
    }


def expand_globs(patterns: list[str]) -> list[Path]:
    out: list[Path] = []
    for pattern in patterns:
        # pathlib limited brace support — expand manually for {a,b}
        if "{" in pattern and "}" in pattern:
            pre, rest = pattern.split("{", 1)
            body, post = rest.split("}", 1)
            for alt in body.split(","):
                out.extend(expand_globs([f"{pre}{alt}{post}"]))
            continue
        out.extend(ROOT.glob(pattern))
    # unique
    seen: set[Path] = set()
    uniq: list[Path] = []
    for path in out:
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        uniq.append(path)
    return uniq


def active_waivers() -> dict[str, dict]:
    waivers: dict[str, dict] = {}
    if not WAIVER_DIR.is_dir():
        return waivers
    today = dt.date.today()
    for path in sorted(WAIVER_DIR.glob("*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        items = doc.get("waivers", doc if isinstance(doc, list) else [doc])
        if isinstance(items, dict):
            items = [items]
        for item in items:
            if not isinstance(item, dict) or "id" not in item:
                continue
            wid = item["id"]
            waivers[wid] = item
            if item.get("stato") == "attiva":
                scadenza = dt.date.fromisoformat(item["scadenza"])
                if scadenza < today:
                    item["_expired"] = True
    return waivers


def waived(path: Path, waivers: dict[str, dict], kind: str) -> bool:
    rel = path.relative_to(ROOT).as_posix()
    for wav in waivers.values():
        if wav.get("stato") != "attiva" or wav.get("_expired"):
            continue
        ambito = wav.get("ambito", "")
        if kind in ambito and rel in ambito:
            return True
        paths = wav.get("paths", [])
        if isinstance(paths, list) and rel in paths:
            return True
        if f"path:{rel}" in ambito or ambito.endswith(rel):
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.parse_args()
    thresholds = load_thresholds()
    max_lines = int(thresholds.get("max_lines_per_source", 1200))
    require_tracker = bool(thresholds.get("todo_requires_tracker", True))
    globs = thresholds.get(
        "source_globs",
        ["src/**/*.cpp", "src/**/*.hpp", "include/**/*.hpp", "tests/**/*.cpp", "tests/**/*.hpp"],
    )
    waivers = active_waivers()
    errors: list[str] = []
    warnings: list[str] = []

    for wid, wav in waivers.items():
        if wav.get("_expired"):
            errors.append(f"expired waiver {wid} (scadenza={wav.get('scadenza')})")

    for path in expand_globs(globs):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        lines = text.splitlines()
        rel = path.relative_to(ROOT).as_posix()
        if len(lines) > max_lines and not waived(path, waivers, "size"):
            errors.append(f"size: {rel} has {len(lines)} lines (max {max_lines})")
        if require_tracker:
            for lineno, line in enumerate(lines, 1):
                match = TODO_RE.search(line)
                if not match:
                    continue
                tracker = (match.group(2) or "").strip()
                if tracker and ISSUE_RE.match(tracker):
                    continue
                if waived(path, waivers, "todo"):
                    continue
                errors.append(
                    f"todo: {rel}:{lineno} {match.group(1)} lacks tracker id "
                    f"(use TODO(#123), TODO(GS-...), or TODO(https://...))"
                )

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"Structure debt validation FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    for warn in warnings:
        print(f"WARNING: {warn}")
    print(
        f"Structure debt validation OK "
        f"(max_lines={max_lines}, files={len(expand_globs(globs))}, "
        f"waivers={len(waivers)})"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
