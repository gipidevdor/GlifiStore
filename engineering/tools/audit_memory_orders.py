#!/usr/bin/env python3
"""Fail closed when an explicit production memory order lacks an audit domain."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import re
import sys
from pathlib import Path


ORDER_RE = re.compile(r"std::memory_order_(relaxed|consume|acquire|release|acq_rel|seq_cst)")
OP_RE = re.compile(
    r"([A-Za-z_][A-Za-z0-9_:\.>\-\[\]]*)\s*(?:\.|->)\s*"
    r"(load|store|exchange|fetch_add|fetch_sub|fetch_or|fetch_and|fetch_xor|"
    r"compare_exchange_weak|compare_exchange_strong|test_and_set|clear|wait)\s*\([^;{}]{0,500}$",
    re.DOTALL,
)


def production_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for source_root in (root / "include" / "glifistore", root / "src"):
        for path in source_root.rglob("*"):
            if path.suffix in {".cpp", ".hpp"} and path.is_file():
                files.append(path)
    return sorted(files)


def operation_before(text: str, offset: int) -> tuple[str, str]:
    prefix = text[max(0, offset - 700) : offset]
    if re.search(r"std::atomic_(?:thread|signal)_fence\s*\([^)]*$", prefix, re.DOTALL):
        return "<fence>", "fence"
    matches = list(OP_RE.finditer(prefix))
    if not matches:
        line = text.count("\n", 0, offset) + 1
        return f"<expression-at-line-{line}>", "atomic-operation"
    match = matches[-1]
    expression = re.sub(r"\s+", "", match.group(1))
    return expression, match.group(2)


def matching_rule(path: str, rules: list[dict]) -> dict:
    matches = [
        rule
        for rule in rules
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in rule["paths"])
    ]
    if len(matches) != 1:
        names = [match.get("id", "<unnamed>") for match in matches]
        raise ValueError(f"{path}: expected exactly one memory-order rule, got {names}")
    return matches[0]


def build_report(root: Path, policy_path: Path) -> dict:
    policy = json.loads(policy_path.read_text(encoding="utf-8"))
    if policy.get("schema_version") != 1:
        raise ValueError("memory-order policy schema_version must be 1")
    rules = policy.get("rules")
    if not isinstance(rules, list) or not rules:
        raise ValueError("memory-order policy must contain nonempty rules")

    used_rules: set[str] = set()
    records: list[dict] = []
    digest = hashlib.sha256()
    for source in production_files(root):
        relative = source.relative_to(root).as_posix()
        text = source.read_text(encoding="utf-8")
        matches = list(ORDER_RE.finditer(text))
        if not matches:
            continue
        rule = matching_rule(relative, rules)
        rule_id = rule["id"]
        used_rules.add(rule_id)
        allowed_orders = set(rule["allowed_orders"])
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(text.encode("utf-8"))
        digest.update(b"\0")
        for match in matches:
            order = match.group(1)
            if order not in allowed_orders:
                raise ValueError(
                    f"{relative}:{text.count(chr(10), 0, match.start()) + 1}: "
                    f"memory_order_{order} is not allowed by {rule_id}"
                )
            if order == "relaxed" and not rule.get("relaxed_justification"):
                raise ValueError(f"{rule_id}: relaxed order requires a positive justification")
            line = text.count("\n", 0, match.start()) + 1
            line_start = text.rfind("\n", 0, match.start()) + 1
            line_end = text.find("\n", match.end())
            if line_end == -1:
                line_end = len(text)
            obj, operation = operation_before(text, match.start())
            records.append(
                {
                    "file": relative,
                    "line": line,
                    "column": match.start() - line_start + 1,
                    "object": obj,
                    "operation": operation,
                    "memory_order": order,
                    "source": text[line_start:line_end].strip(),
                    "domain": rule_id,
                    "classification": rule["classification"],
                    "writer": rule["writer"],
                    "readers": rule["readers"],
                    "published_or_protected_data": rule["published_or_protected_data"],
                    "required_happens_before": rule["required_happens_before"],
                    "invariant": rule["invariant"],
                    "relaxed_justification": rule.get("relaxed_justification", ""),
                }
            )

    unused = sorted({rule["id"] for rule in rules} - used_rules)
    if unused:
        raise ValueError(f"unused memory-order policy rules: {', '.join(unused)}")
    return {
        "schema_version": 1,
        "scope": ["include/glifistore/**/*.hpp", "src/**/*.{cpp,hpp}"],
        "source_sha256": digest.hexdigest(),
        "operation_count": len(records),
        "domain_count": len(used_rules),
        "records": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--policy", default="engineering/correctness/memory-order-policy.json"
    )
    parser.add_argument(
        "--output", default="engineering/evidence/memory-order-inventory-2026-08-30.json"
    )
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    policy = (root / args.policy).resolve()
    output = (root / args.output).resolve()
    try:
        report = build_report(root, policy)
        encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.write:
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(encoded, encoding="utf-8")
        elif not output.exists() or output.read_text(encoding="utf-8") != encoded:
            print(
                f"memory-order evidence is missing or stale: {output.relative_to(root)} "
                "(run with --write)",
                file=sys.stderr,
            )
            return 1
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"memory-order audit failed: {error}", file=sys.stderr)
        return 1
    print(
        f"memory-order audit PASS: {report['operation_count']} explicit orders in "
        f"{report['domain_count']} reviewed domains"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
