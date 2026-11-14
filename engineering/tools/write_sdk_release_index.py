#!/usr/bin/env python3
"""Write and verify the machine-readable SDK release artifact index."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

SHA_LINE = re.compile(r"^([0-9a-f]{64})  ([^/]+)$")
INDEX_NAME = "sdk-release-index.json"


def digest(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def read_sums(path: Path) -> dict[str, str]:
    sums: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = SHA_LINE.fullmatch(line)
        if not match:
            raise ValueError(f"malformed SHA256SUMS line: {line!r}")
        sha256, name = match.groups()
        if name in sums:
            raise ValueError(f"duplicate SHA256SUMS entry: {name}")
        sums[name] = sha256
    return sums


def write_sums(path: Path, sums: dict[str, str]) -> None:
    content = "".join(f"{sha256}  {name}\n" for name, sha256 in sorted(sums.items()))
    path.write_text(content, encoding="utf-8")


def load_contract(root: Path) -> dict[str, Any]:
    path = root / "engineering/compatibility/sdk-release-contract.json"
    return json.loads(path.read_text(encoding="utf-8"))


def git_commit(root: Path) -> str:
    supplied = os.environ.get("GITHUB_SHA")
    commit = supplied or subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
    ).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("source commit must be a 40-character lowercase SHA")
    return commit


def git_dirty(root: Path) -> bool:
    output = subprocess.check_output(
        ["git", "-C", str(root), "status", "--porcelain"], text=True
    )
    return bool(output.strip())


def build_index(root: Path, artifact_dir: Path, require_complete: bool) -> dict[str, Any]:
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    contract = load_contract(root)
    sums_path = artifact_dir / "SHA256SUMS"
    sums = read_sums(sums_path)
    sums.pop(INDEX_NAME, None)

    for name, expected in sums.items():
        path = artifact_dir / name
        if not path.is_file():
            raise ValueError(f"checksummed SDK artifact is missing: {name}")
        actual = digest(path)
        if actual != expected:
            raise ValueError(f"SDK artifact checksum mismatch: {name}")

    owners: dict[str, tuple[str, str]] = {}
    missing: list[str] = []
    clients: dict[str, Any] = {}
    for client, spec in contract["clients"].items():
        names: list[str] = []
        for required in spec["required_artifacts"]:
            name = required["name"].format(version=version)
            if name not in sums:
                missing.append(f"{client}:{required['role']}")
                continue
            if name in owners:
                raise ValueError(f"artifact assigned to multiple SDK roles: {name}")
            owners[name] = (client, required["role"])
            names.append(name)
        clients[client] = {
            "distribution": spec["distribution"],
            "artifacts": sorted(names),
        }

    complete = not missing
    if require_complete and not complete:
        raise ValueError(f"incomplete SDK release artifact set: {', '.join(missing)}")

    artifacts = []
    for name, sha256 in sorted(sums.items()):
        client, role = owners.get(name, ("release-metadata", "metadata"))
        artifacts.append({"name": name, "sha256": sha256, "client": client, "role": role})

    tag = os.environ.get("GITHUB_REF_NAME") if os.environ.get("GITHUB_REF_TYPE") == "tag" else None
    return {
        "schema_version": contract["schema_version"],
        "glifistore_version": version,
        "source": {"commit": git_commit(root), "tag": tag, "dirty": git_dirty(root)},
        "contracts": {
            "wire_protocol": contract["wire_protocol"],
            "client_semantics": contract["client_semantics"],
            "persistence_format": contract["persistence_format"],
        },
        "complete": complete,
        "missing_artifacts": missing,
        "clients": clients,
        "artifacts": artifacts,
        "compatibility_evidence": "not-inferred-from-index",
    }


def verify_index(artifact_dir: Path, index: dict[str, Any], contract: dict[str, Any]) -> None:
    sums = read_sums(artifact_dir / "SHA256SUMS")
    expected_index_sha = sums.get(INDEX_NAME)
    if expected_index_sha != digest(artifact_dir / INDEX_NAME):
        raise ValueError("SDK release index is not bound by SHA256SUMS")
    entries = index.get("artifacts", [])
    indexed = {entry["name"]: entry["sha256"] for entry in entries}
    if len(indexed) != len(entries):
        raise ValueError("SDK release index contains duplicate artifact names")
    payload_sums = {name: sha256 for name, sha256 in sums.items() if name != INDEX_NAME}
    if indexed != payload_sums:
        raise ValueError("SDK release index artifacts do not match SHA256SUMS")
    for name, expected in payload_sums.items():
        path = artifact_dir / name
        if not path.is_file() or digest(path) != expected:
            raise ValueError(f"SDK release payload checksum mismatch: {name}")
    version = index.get("glifistore_version")
    if not isinstance(version, str) or not version:
        raise ValueError("SDK release index version is invalid")
    clients = index.get("clients", {})
    if not isinstance(clients, dict) or set(clients) != set(contract["clients"]):
        raise ValueError("SDK release index client set is incomplete")
    expected_missing: list[str] = []
    expected_owners: dict[str, tuple[str, str]] = {}
    for client, spec in contract["clients"].items():
        expected_names: list[str] = []
        for required in spec["required_artifacts"]:
            name = required["name"].format(version=version)
            if name in payload_sums:
                expected_names.append(name)
                expected_owners[name] = (client, required["role"])
            else:
                expected_missing.append(f"{client}:{required['role']}")
        client_entry = clients.get(client, {})
        if client_entry.get("distribution") != spec["distribution"] or client_entry.get(
            "artifacts"
        ) != sorted(expected_names):
            raise ValueError(f"SDK release index client mapping is invalid: {client}")
    missing = index.get("missing_artifacts")
    complete = index.get("complete")
    if not isinstance(complete, bool) or missing != expected_missing or complete != (not missing):
        raise ValueError("SDK release index completeness fields disagree")
    for entry in entries:
        expected_client, expected_role = expected_owners.get(
            entry["name"], ("release-metadata", "metadata")
        )
        if entry.get("client") != expected_client or entry.get("role") != expected_role:
            raise ValueError(f"SDK release index role mapping is invalid: {entry['name']}")
    if index.get("schema_version") != contract["schema_version"]:
        raise ValueError("unsupported SDK release index schema")
    source = index.get("source", {})
    if not re.fullmatch(r"[0-9a-f]{40}", source.get("commit", "")) or not isinstance(
        source.get("dirty"), bool
    ):
        raise ValueError("SDK release index source identity is invalid")
    contracts = index.get("contracts", {})
    expected_contracts = {
        "wire_protocol": contract["wire_protocol"],
        "client_semantics": contract["client_semantics"],
        "persistence_format": contract["persistence_format"],
    }
    if contracts != expected_contracts:
        raise ValueError("SDK release index contract versions are invalid")
    if index.get("compatibility_evidence") != "not-inferred-from-index":
        raise ValueError("SDK release index must not claim compatibility evidence")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact_dir", type=Path)
    parser.add_argument("--require-complete", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    artifact_dir = args.artifact_dir.resolve()
    try:
        contract = load_contract(root)
        index_path = artifact_dir / INDEX_NAME
        if args.verify_only:
            index = json.loads(index_path.read_text(encoding="utf-8"))
            if args.require_complete and not index.get("complete"):
                raise ValueError("SDK release index is partial")
        else:
            index = build_index(root, artifact_dir, args.require_complete)
            index_path.write_text(
                json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
            sums_path = artifact_dir / "SHA256SUMS"
            sums = read_sums(sums_path)
            sums[INDEX_NAME] = digest(index_path)
            write_sums(sums_path, sums)
        verify_index(artifact_dir, index, contract)
    except (KeyError, OSError, TypeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"SDK release index FAILED: {exc}", file=sys.stderr)
        return 1
    state = "complete" if index["complete"] else "partial"
    action = "verified" if args.verify_only else "written"
    print(f"SDK release index {action} ({state}, {len(index['artifacts'])} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
