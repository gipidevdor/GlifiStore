#!/usr/bin/env python3
"""Validate engineering/performance/budgets.yaml structure and gate linkage."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("PyYAML required", file=sys.stderr)
    sys.exit(2)

REQUIRED_CLASS_KEYS = {"id", "gate", "requirement", "enforcement", "metric"}
ALLOWED_ENFORCEMENT = {
    "hosted_ci",
    "hardware_self_hosted",
    "scheduled_or_manual",
    "advisory_snapshot_only",
}
ALLOWED_STATUS = {
    "enforced",
    "enforced_path_exists",
    "specified_waiting_for_runner",
    "advisory_not_release_gate",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=None)
    args = parser.parse_args()
    root = (args.root or Path(__file__).resolve().parents[2]).resolve()
    path = root / "engineering" / "performance" / "budgets.yaml"
    doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    errors: list[str] = []

    if doc.get("version") != 1:
        errors.append("version must be 1")
    honesty = doc.get("honesty") or {}
    if honesty.get("claim_ceiling") != "architectural-prototype":
        errors.append("honesty.claim_ceiling must remain architectural-prototype")
    if honesty.get("absolute_product_claims") != "forbidden_without_hardware_evidence":
        errors.append("honesty.absolute_product_claims must forbid absolute claims without hardware")

    classes = doc.get("classes")
    if not isinstance(classes, list) or not classes:
        errors.append("classes must be a non-empty list")
        classes = []

    ids: set[str] = set()
    gates_yaml = yaml.safe_load(
        (root / "engineering" / "gates" / "quality-gates.yaml").read_text(encoding="utf-8")
    )
    gate_ids = {g["id"] for g in gates_yaml.get("gates", [])}
    req_ids: set[str] = set()
    for req_path in (root / "engineering" / "requirements").glob("*.yaml"):
        req_doc = yaml.safe_load(req_path.read_text(encoding="utf-8")) or {}
        for item in req_doc.get("requirements", []):
            req_ids.add(item["id"])

    for idx, cls in enumerate(classes):
        where = f"classes[{idx}]"
        if not isinstance(cls, dict):
            errors.append(f"{where}: must be a mapping")
            continue
        missing = REQUIRED_CLASS_KEYS - set(cls)
        if missing:
            errors.append(f"{where}: missing keys {sorted(missing)}")
        cid = cls.get("id")
        if not isinstance(cid, str) or not cid:
            errors.append(f"{where}: id required")
        elif cid in ids:
            errors.append(f"{where}: duplicate id {cid}")
        else:
            ids.add(cid)
        gate = cls.get("gate")
        if gate not in gate_ids:
            errors.append(f"{where}: unknown gate {gate}")
        req = cls.get("requirement")
        if req not in req_ids:
            errors.append(f"{where}: unknown requirement {req}")
        enf = cls.get("enforcement")
        if enf not in ALLOWED_ENFORCEMENT:
            errors.append(f"{where}: invalid enforcement {enf}")
        status = cls.get("status")
        if status is not None and status not in ALLOWED_STATUS:
            errors.append(f"{where}: invalid status {status}")
        for wf in cls.get("workflows", []) or []:
            wf_path = root / ".github" / "workflows" / wf
            if not wf_path.is_file():
                errors.append(f"{where}: missing workflow {wf}")
        for auth in cls.get("authority", []) or []:
            if not (root / auth).is_file():
                errors.append(f"{where}: missing authority {auth}")
        harness = cls.get("harness")
        if harness and not (root / harness).is_file():
            errors.append(f"{where}: missing harness {harness}")
        snap = cls.get("snapshot")
        if isinstance(snap, dict) and "path" in snap:
            if not (root / snap["path"]).is_file():
                errors.append(f"{where}: missing snapshot path {snap['path']}")
        if enf == "hardware_self_hosted" and not cls.get("runner_label"):
            errors.append(f"{where}: hardware_self_hosted requires runner_label")
        if enf == "advisory_snapshot_only" and status != "advisory_not_release_gate":
            errors.append(f"{where}: advisory budgets must be advisory_not_release_gate")

    # Phase E requires at least one hardware-gated and one soak-linked class.
    enforcements = {c.get("enforcement") for c in classes if isinstance(c, dict)}
    if "hardware_self_hosted" not in enforcements:
        errors.append("at least one hardware_self_hosted budget class is required")
    gates_used = {c.get("gate") for c in classes if isinstance(c, dict)}
    for needed in ("GATE-PERFORMANCE", "GATE-SOAK", "GATE-OPS-RUNBOOKS"):
        if needed not in gates_used:
            errors.append(f"budgets must link {needed}")

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"Perf budget validation FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    print(f"Perf budget validation OK ({len(classes)} classes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
