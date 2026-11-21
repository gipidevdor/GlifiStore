#!/usr/bin/env python3
"""Export an unprivileged, deterministic GDev view of the assurance authority."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
DIRECTORIES = tuple(f"engineering/{name}" for name in (
    "requirements", "hazards", "gates", "waivers", "claims", "schemas"
))
FILES = ("engineering/tools/export_gdev_catalog.py",)
OUTPUT = ".gdev-assurance/catalog.json"


def source_paths(root: Path) -> list[str]:
    paths = set(FILES)
    for directory in DIRECTORIES:
        folder = root / directory
        if not folder.is_dir():
            raise ValueError(f"Missing catalog directory: {directory}")
        paths.update(p.relative_to(root).as_posix() for p in folder.rglob("*") if p.is_file())
    for path in paths:
        if not (root / path).resolve().is_relative_to(root.resolve()):
            raise ValueError(f"External catalog source: {path}")
    return sorted(paths)


def build_catalog(root: Path = ROOT) -> dict:
    inputs = []
    records = []
    seen = set()
    for path in source_paths(root):
        raw = (root / path).read_bytes()
        inputs.append({"path": path, "sha256": hashlib.sha256(raw).hexdigest()})
        family = Path(path).parent.name
        if family not in {"requirements", "hazards", "gates", "waivers", "claims"}:
            continue
        document = yaml.safe_load(raw)
        rows = document.get(family) if isinstance(document, dict) else None
        if not isinstance(rows, list):
            raise ValueError(f"Expected '{family}' list: {path}")
        for row in rows:
            if not isinstance(row, dict) or not isinstance(row.get("id"), str) or row["id"] in seen:
                raise ValueError(f"Invalid or duplicate record: {path}")
            seen.add(row["id"])
            records.append({"id": row["id"], "kind": family,
                            "path": path, "state": row.get("stato", row.get("claim_massimo", "UNSPECIFIED")),
                            "data": row})
    return {"schema_version": 1, "id": "GS-ASSURANCE",
            "inputs": inputs, "records": sorted(records, key=lambda row: row["id"])}


def json_default(value):
    # PyYAML recognizes unquoted ISO dates; keep their literal meaning in JSON.
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    raise TypeError(f"Unsupported catalog value: {type(value).__name__}")


def render(root: Path = ROOT) -> str:
    return json.dumps(build_catalog(root), ensure_ascii=False, indent=2,
                      sort_keys=True, default=json_default) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = render()
    target = ROOT / OUTPUT
    if args.check:
        if not target.is_file() or target.read_text(encoding="utf-8") != content:
            parser.exit(1, "GDev catalog is stale. Run export_gdev_catalog.py after validating assurance.\n")
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
