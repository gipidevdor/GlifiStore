#!/usr/bin/env python3
"""Validate N↔N-1 compatibility matrix structure and referenced evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

try:
    import yaml
except ImportError:  # pragma: no cover
    print("PyYAML required", file=sys.stderr)
    sys.exit(2)

ALLOWED_STATUS = {
    "supported",
    "supported_offline_only",
    "intentionally_rejected",
    "unsupported",
    "not_promised",
}
SHA256_LINE = re.compile(r"^([0-9a-f]{64})  ([^/]+)$")


def validate_released_label(
    label_dir: Path, persistence_format: int, wire_protocol: int
) -> list[str]:
    errors: list[str] = []
    if (
        len(label_dir.name) > 128
        or not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z._+-]*", label_dir.name)
        or ".." in label_dir.name
    ):
        errors.append(f"{label_dir}: unsafe released fixture label")
    metadata_path = label_dir / "METADATA.txt"
    sums_path = label_dir / "SHA256SUMS"
    if not metadata_path.is_file():
        errors.append(f"{label_dir}: missing METADATA.txt")
    if not sums_path.is_file():
        errors.append(f"{label_dir}: missing SHA256SUMS")
    if errors:
        return errors

    metadata: dict[str, str] = {}
    for line in metadata_path.read_text(encoding="utf-8").splitlines():
        if not line or "=" not in line:
            errors.append(f"{metadata_path}: malformed metadata line {line!r}")
            continue
        key, value = line.split("=", 1)
        if key in metadata:
            errors.append(f"{metadata_path}: duplicate metadata key {key}")
        metadata[key] = value

    required = {
        "schema_version": "2",
        "label": label_dir.name,
        "persistence_format": str(persistence_format),
        "wire_protocol": str(wire_protocol),
    }
    for key, expected in required.items():
        if metadata.get(key) != expected:
            errors.append(
                f"{metadata_path}: {key} must be {expected!r}, got {metadata.get(key)!r}"
            )

    for key in ("glifistore_version", "packaged_at", "git_commit", "fixture_count"):
        if not metadata.get(key):
            errors.append(f"{metadata_path}: missing non-empty {key}")
    if metadata.get("git_commit") and not re.fullmatch(r"[0-9a-f]{40}", metadata["git_commit"]):
        errors.append(f"{metadata_path}: git_commit must be a 40-character lowercase SHA")
    if metadata.get("packaged_at") and not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", metadata["packaged_at"]
    ):
        errors.append(f"{metadata_path}: packaged_at must be UTC ISO-8601 seconds")

    fixtures = sorted(path for path in label_dir.glob("*.hex") if path.is_file())
    try:
        recorded_count = int(metadata.get("fixture_count", ""))
    except ValueError:
        recorded_count = -1
    if recorded_count != len(fixtures):
        errors.append(
            f"{metadata_path}: fixture_count={recorded_count} but found {len(fixtures)} hex files"
        )

    recorded_sums: dict[str, str] = {}
    for line in sums_path.read_text(encoding="utf-8").splitlines():
        match = SHA256_LINE.fullmatch(line)
        if not match:
            errors.append(f"{sums_path}: malformed checksum line {line!r}")
            continue
        digest, name = match.groups()
        if name in recorded_sums:
            errors.append(f"{sums_path}: duplicate checksum for {name}")
        recorded_sums[name] = digest

    fixture_names = {path.name for path in fixtures}
    if set(recorded_sums) != fixture_names:
        errors.append(
            f"{sums_path}: checksum names {sorted(recorded_sums)} do not match fixtures "
            f"{sorted(fixture_names)}"
        )
    for fixture in fixtures:
        actual = hashlib.sha256(fixture.read_bytes()).hexdigest()
        if recorded_sums.get(fixture.name) != actual:
            errors.append(f"{sums_path}: digest mismatch for {fixture.name}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=None)
    args = parser.parse_args()
    root = (args.root or Path(__file__).resolve().parents[2]).resolve()
    path = root / "engineering" / "compatibility" / "n-n1-matrix.yaml"
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    errors: list[str] = []

    sdk_contract_path = root / "engineering/compatibility/sdk-release-contract.json"
    try:
        sdk_contract = json.loads(sdk_contract_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"invalid SDK release contract: {exc}")
        sdk_contract = {}
    for key in ("wire_protocol", "persistence_format"):
        if sdk_contract.get(key) != doc.get(key):
            errors.append(
                f"SDK release contract {key}={sdk_contract.get(key)!r} "
                f"does not match matrix {doc.get(key)!r}"
            )
    if sdk_contract.get("schema_version") != 1:
        errors.append("SDK release contract schema_version must be 1")
    if sdk_contract.get("client_semantics") != 1:
        errors.append("SDK release contract client_semantics must be 1")
    clients = sdk_contract.get("clients", {})
    expected_clients = {"cpp", "python", "perl", "ruby", "go", "erlang"}
    if not isinstance(clients, dict):
        errors.append("SDK release contract clients must be an object")
        clients = {}
    if set(clients) != expected_clients:
        errors.append(f"SDK release contract clients must be {sorted(expected_clients)}")
    artifact_names: set[str] = set()
    for client, spec in clients.items():
        if not isinstance(spec, dict):
            errors.append(f"SDK release contract {client} must be an object")
            continue
        if not spec.get("distribution"):
            errors.append(f"SDK release contract {client} has no distribution")
        required_artifacts = spec.get("required_artifacts", [])
        if not isinstance(required_artifacts, list):
            errors.append(f"SDK release contract {client} required_artifacts must be a list")
            continue
        for artifact in required_artifacts:
            if not isinstance(artifact, dict):
                errors.append(f"SDK release contract {client} artifact must be an object")
                continue
            name = artifact.get("name", "")
            role = artifact.get("role", "")
            if not name or not role or "{version}" not in name:
                errors.append(f"SDK release contract {client} has invalid artifact template")
            if name in artifact_names:
                errors.append(f"duplicate SDK artifact template: {name}")
            artifact_names.add(name)

    for section in ("store_open", "wire", "abi"):
        if section not in doc:
            errors.append(f"missing section {section}")
            continue
        rows = doc[section]
        if not isinstance(rows, list) or not rows:
            errors.append(f"{section} must be a non-empty list")
            continue
        ids: set[str] = set()
        for row in rows:
            rid = row.get("id", "<missing>")
            if rid in ids:
                errors.append(f"duplicate row id {rid}")
            ids.add(rid)
            status = row.get("status")
            if status not in ALLOWED_STATUS:
                errors.append(f"{rid}: invalid status {status!r}")
            for ev in row.get("evidence", []) or []:
                if isinstance(ev, str):
                    if not (root / ev).exists():
                        errors.append(f"{rid}: missing evidence path {ev}")
                elif isinstance(ev, dict) and "workflow" in ev:
                    wf = root / ".github" / "workflows" / ev["workflow"]
                    if not wf.is_file():
                        errors.append(f"{rid}: missing workflow {ev['workflow']}")

    policy = doc.get("released_fixture_policy", {})
    if policy.get("artifact_schema") != 2:
        errors.append("released_fixture_policy.artifact_schema must be 2")
    for label in policy.get("in_tree_labels", []):
        label_dir = root / "tests" / "fixtures" / "released" / label
        if not label_dir.is_dir():
            errors.append(f"in_tree label missing: tests/fixtures/released/{label}")
    script = policy.get("packaging_script")
    if script and not (root / script).is_file():
        errors.append(f"missing packaging script {script}")

    sdk_policy = doc.get("sdk_artifact_policy", {})
    if sdk_policy.get("compatibility_evidence") != "not-inferred-from-index":
        errors.append("sdk_artifact_policy must not infer compatibility from the index")
    if sdk_policy.get("generated_index") != "sdk-release-index.json":
        errors.append("sdk_artifact_policy.generated_index must be sdk-release-index.json")
    for key in ("contract", "index_writer"):
        ref = sdk_policy.get(key)
        if not isinstance(ref, str) or not (root / ref).is_file():
            errors.append(f"sdk_artifact_policy.{key} must reference an existing file")

    released_root = root / "tests" / "fixtures" / "released"
    if released_root.is_dir():
        for label_dir in sorted(path for path in released_root.iterdir() if path.is_dir()):
            errors.extend(
                validate_released_label(
                    label_dir,
                    int(doc.get("persistence_format", -1)),
                    int(doc.get("wire_protocol", -1)),
                )
            )

    tagged_store_policy = doc.get("tagged_store_fixture_policy", {})
    if tagged_store_policy.get("artifact_schema") != 1:
        errors.append("tagged_store_fixture_policy.artifact_schema must be 1")
    if tagged_store_policy.get("selection") != (
        "newest-valid-tagged-release-strictly-older-than-candidate"
    ):
        errors.append("tagged_store_fixture_policy selection must be strict N-1")
    for key in ("metadata_schema", "fixture_tool", "installed_harness"):
        ref = tagged_store_policy.get(key)
        if not isinstance(ref, str) or not (root / ref).is_file():
            errors.append(f"tagged_store_fixture_policy.{key} must reference an existing file")
    workflow = tagged_store_policy.get("release_workflow")
    if not isinstance(workflow, str) or not (root / ".github" / "workflows" / workflow).is_file():
        errors.append("tagged_store_fixture_policy.release_workflow must reference an existing workflow")
    permanent_drop = tagged_store_policy.get("permanent_drop")
    if permanent_drop != "tests/fixtures/released-stores/<semver>/":
        errors.append("tagged_store_fixture_policy.permanent_drop is invalid")

    abi_policy = doc.get("abi_artifact_policy", {})
    if abi_policy.get("fixture_schema") != 1:
        errors.append("abi_artifact_policy.fixture_schema must be 1")
    if abi_policy.get("selection") != (
        "newest-annotated-release-strictly-older-with-same-abi-major-"
        "then-require-complete-attested-assets"
    ):
        errors.append("abi_artifact_policy selection must be strict and ABI-major matched")
    for key in (
        "metadata_schema",
        "compiled_consumer_packager",
        "fixture_validator",
        "prior_release_validator",
        "installed_harness",
    ):
        ref = abi_policy.get(key)
        if not isinstance(ref, str) or not (root / ref).is_file():
            errors.append(f"abi_artifact_policy.{key} must reference an existing file")
    abi_workflow = abi_policy.get("release_workflow")
    if not isinstance(abi_workflow, str) or not (
        root / ".github" / "workflows" / abi_workflow
    ).is_file():
        errors.append("abi_artifact_policy.release_workflow must reference an existing workflow")

    wire_policy = doc.get("wire_artifact_policy", {})
    if wire_policy.get("fixture_schema") != 1:
        errors.append("wire_artifact_policy.fixture_schema must be 1")
    if wire_policy.get("selection") != (
        "newest-annotated-release-strictly-older-with-same-abi-major-"
        "then-require-complete-attested-assets"
    ):
        errors.append("wire_artifact_policy selection must be strict and ABI-major matched")
    if wire_policy.get("directions") != [
        "new-client-new-server",
        "old-client-new-server",
        "new-client-old-server",
    ]:
        errors.append("wire_artifact_policy directions must contain the complete ordered matrix")
    for key in (
        "metadata_schema",
        "compiled_client_packager",
        "fixture_validator",
        "prior_release_validator",
        "installed_harness",
    ):
        ref = wire_policy.get(key)
        if not isinstance(ref, str) or not (root / ref).is_file():
            errors.append(f"wire_artifact_policy.{key} must reference an existing file")
    wire_workflow = wire_policy.get("release_workflow")
    if not isinstance(wire_workflow, str) or not (
        root / ".github" / "workflows" / wire_workflow
    ).is_file():
        errors.append("wire_artifact_policy.release_workflow must reference an existing workflow")

    if errors:
        for err in errors:
            print(f"ERROR: {err}", file=sys.stderr)
        print(f"Compat matrix validation FAILED ({len(errors)} error(s))", file=sys.stderr)
        return 1
    n = sum(len(doc.get(s, [])) for s in ("store_open", "wire", "abi"))
    print(f"Compat matrix validation OK ({n} rows)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
