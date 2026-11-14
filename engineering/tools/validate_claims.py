#!/usr/bin/env python3
"""Validate engineering/claims/*.yaml against claim.schema.json."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

try:
    import yaml
    import jsonschema
except ImportError:  # pragma: no cover
    print("PyYAML and jsonschema required", file=sys.stderr)
    sys.exit(2)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=None)
    args = parser.parse_args()
    root = (args.root or Path(__file__).resolve().parents[2]).resolve()
    schema = json.loads((root / "engineering" / "schemas" / "claim.schema.json").read_text())
    claims_dir = root / "engineering" / "claims"
    errors: list[str] = []
    count = 0
    for path in sorted(claims_dir.glob("*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        items = doc.get("claims", doc if isinstance(doc, list) else [doc])
        if isinstance(items, dict):
            items = [items]
        for item in items:
            count += 1
            where = f"{path.relative_to(root)}:{item.get('id', '<missing>')}"
            try:
                jsonschema.validate(item, schema)
            except jsonschema.ValidationError as exc:
                errors.append(f"{where}: {exc.message}")
                continue
            for ev in item.get("evidence", []):
                if not isinstance(ev, dict):
                    continue
                if "path" in ev:
                    target = root / ev["path"]
                    # Trailing slash dirs may not exist until a tag packages fixtures.
                    if ev["path"].endswith("/"):
                        continue
                    if not target.exists():
                        errors.append(f"{where}: missing evidence path {ev['path']}")
                if "workflow" in ev:
                    wf = root / ".github" / "workflows" / ev["workflow"]
                    if not wf.is_file():
                        errors.append(f"{where}: missing workflow {ev['workflow']}")
    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"Claims validation FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    print(f"Claims validation OK ({count} claim(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
