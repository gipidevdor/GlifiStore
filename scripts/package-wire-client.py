#!/usr/bin/env python3
"""Package the compiled wire client as a deterministic release fixture."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "engineering" / "tools"))

from release_identity import ReleaseIdentityError, validate_release_identity  # noqa: E402


def info(name: str, epoch: int, mode: int, kind: bytes) -> tarfile.TarInfo:
    value = tarfile.TarInfo(name)
    value.mtime = epoch
    value.uid = 0
    value.gid = 0
    value.uname = "root"
    value.gname = "root"
    value.mode = mode
    value.type = kind
    return value


def add_bytes(archive: tarfile.TarFile, name: str, payload: bytes, epoch: int, mode: int) -> None:
    value = info(name, epoch, mode, tarfile.REGTYPE)
    value.size = len(payload)
    archive.addfile(value, io.BytesIO(payload))


def read_wire_version(root: Path) -> int:
    header = (root / "include/glifistore/server/protocol.hpp").read_text(encoding="utf-8")
    match = re.search(r"\bkProtocolVersion\s*=\s*([0-9]+)\s*;", header)
    if match is None:
        raise ReleaseIdentityError("wire protocol version authority is missing")
    value = int(match.group(1))
    if value <= 0 or value > 65535:
        raise ReleaseIdentityError("wire protocol version is outside uint16_t range")
    return value


def package(
    root: Path,
    tag: str,
    client: Path,
    target_os: str,
    architecture: str,
    output_directory: Path,
) -> Path:
    identity = validate_release_identity(root, tag)
    if client.is_symlink() or not client.is_file() or not os.access(client, os.X_OK):
        raise ReleaseIdentityError("wire client must be a regular executable")
    wire_version = read_wire_version(root)
    safe_os = target_os.lower()
    safe_arch = architecture.lower()
    for name, value in (("target OS", safe_os), ("architecture", safe_arch)):
        if not value or not value.replace("-", "").replace("_", "").isalnum():
            raise ReleaseIdentityError(f"{name} must be a simple identifier")
    basename = (
        f"glifistore-wire-v{wire_version}-client-"
        f"{identity.product_version}-{safe_os}-{safe_arch}"
    )
    output_directory.mkdir(parents=True, exist_ok=True)
    output = output_directory / f"{basename}.tar.xz"
    if output.exists() or output.is_symlink():
        raise ReleaseIdentityError(f"refusing to replace existing artifact: {output}")
    binary = client.read_bytes()
    metadata = {
        "schema_version": 1,
        "product_version": identity.product_version,
        "wire_version": wire_version,
        "git_sha": identity.git_sha,
        "tag": identity.tag,
        "target_os": safe_os,
        "architecture": safe_arch,
        "client_sha256": hashlib.sha256(binary).hexdigest(),
    }
    with tarfile.open(output, "w:xz", format=tarfile.PAX_FORMAT, preset=9) as archive:
        archive.addfile(info(f"{basename}/", identity.source_date_epoch, 0o755, tarfile.DIRTYPE))
        add_bytes(
            archive,
            f"{basename}/glifistore-wire-v{wire_version}-client",
            binary,
            identity.source_date_epoch,
            0o755,
        )
        add_bytes(
            archive,
            f"{basename}/WIRE-CLIENT.json",
            (json.dumps(metadata, indent=2, sort_keys=True) + "\n").encode("utf-8"),
            identity.source_date_epoch,
            0o644,
        )
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--client", type=Path, required=True)
    parser.add_argument("--target-os", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    try:
        output = package(
            args.root.resolve(),
            args.tag,
            args.client,
            args.target_os,
            args.architecture,
            args.output_directory,
        )
    except (OSError, ReleaseIdentityError, tarfile.TarError) as error:
        print(f"wire client package FAILED: {error}", file=sys.stderr)
        return 1
    print(f"wire_client_archive={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
