#!/usr/bin/env python3
"""Rewrite a Ruby gem as a bounded, deterministic pair of nested tar archives."""

from __future__ import annotations

import binascii
import gzip
import hashlib
import io
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import tarfile
import tempfile


EXPECTED_PARTS = ("metadata.gz", "data.tar.gz", "checksums.yaml.gz")
MAX_MEMBERS = 100_000
MAX_UNCOMPRESSED_BYTES = 512 * 1024 * 1024


class NormalizationError(RuntimeError):
    pass


def _read_regular_member(archive: tarfile.TarFile, member: tarfile.TarInfo) -> bytes:
    if not member.isfile():
        raise NormalizationError(f"gem member is not a regular file: {member.name}")
    if member.size < 0 or member.size > MAX_UNCOMPRESSED_BYTES:
        raise NormalizationError(f"gem member has an unsafe size: {member.name}")
    source = archive.extractfile(member)
    if source is None:
        raise NormalizationError(f"cannot read gem member: {member.name}")
    payload = source.read(MAX_UNCOMPRESSED_BYTES + 1)
    if len(payload) != member.size or len(payload) > MAX_UNCOMPRESSED_BYTES:
        raise NormalizationError(f"gem member size changed while reading: {member.name}")
    return payload


def _safe_data_name(name: str) -> bool:
    path = PurePosixPath(name)
    return (
        bool(name)
        and str(path) == name
        and not path.is_absolute()
        and "\\" not in name
        and all(part not in ("", ".", "..") for part in path.parts)
    )


def _decompress_bounded(payload: bytes, member_name: str) -> bytes:
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(payload), mode="rb") as stream:
            output = stream.read(MAX_UNCOMPRESSED_BYTES + 1)
    except (EOFError, OSError) as error:
        raise NormalizationError(f"invalid {member_name}: {error}") from error
    if len(output) > MAX_UNCOMPRESSED_BYTES:
        raise NormalizationError(f"{member_name} expands beyond the normalization limit")
    return output


def _canonical_data_tar(compressed: bytes, epoch: int) -> bytes:
    raw = _decompress_bounded(compressed, "data.tar.gz")

    entries: list[tuple[str, int, bytes]] = []
    seen: set[str] = set()
    total_size = 0
    try:
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as source:
            members = source.getmembers()
            if len(members) > MAX_MEMBERS:
                raise NormalizationError("data.tar.gz contains too many members")
            for member in members:
                if not _safe_data_name(member.name):
                    raise NormalizationError(f"data.tar.gz contains an unsafe path: {member.name}")
                if member.name in seen:
                    raise NormalizationError(f"data.tar.gz contains a duplicate path: {member.name}")
                seen.add(member.name)
                payload = _read_regular_member(source, member)
                total_size += len(payload)
                if total_size > MAX_UNCOMPRESSED_BYTES:
                    raise NormalizationError("data.tar.gz payload exceeds the normalization limit")
                mode = 0o755 if member.mode & 0o111 else 0o644
                entries.append((member.name, mode, payload))
    except tarfile.TarError as error:
        raise NormalizationError(f"invalid nested data tar: {error}") from error

    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as target:
        for name, mode, payload in sorted(entries):
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            info.mode = mode
            info.mtime = epoch
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            target.addfile(info, io.BytesIO(payload))
    return output.getvalue()


def _canonical_gzip(payload: bytes) -> bytes:
    # Emit RFC 1951 stored blocks directly. Their byte representation does not
    # depend on the host zlib version, unlike ordinary DEFLATE compression.
    output = bytearray(b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff")
    offset = 0
    while True:
        chunk = payload[offset : offset + 65_535]
        offset += len(chunk)
        final = offset >= len(payload)
        output.append(1 if final else 0)
        length = len(chunk)
        output.extend(length.to_bytes(2, "little"))
        output.extend((length ^ 0xFFFF).to_bytes(2, "little"))
        output.extend(chunk)
        if final:
            break
    output.extend((binascii.crc32(payload) & 0xFFFFFFFF).to_bytes(4, "little"))
    output.extend((len(payload) & 0xFFFFFFFF).to_bytes(4, "little"))
    return bytes(output)


def _canonical_metadata_yaml(payload: bytes) -> bytes:
    for field in (b"autorequire", b"post_install_message", b"signing_key"):
        payload = re.sub(rb"(?m)^" + field + rb":[ \t]*$", field + b":", payload)
    return payload


def _checksums(metadata: bytes, data: bytes) -> bytes:
    return (
        "---\n"
        "SHA256:\n"
        f"  metadata.gz: {hashlib.sha256(metadata).hexdigest()}\n"
        f"  data.tar.gz: {hashlib.sha256(data).hexdigest()}\n"
        "SHA512:\n"
        f"  metadata.gz: {hashlib.sha512(metadata).hexdigest()}\n"
        f"  data.tar.gz: {hashlib.sha512(data).hexdigest()}\n"
    ).encode("ascii")


def normalize(path: Path, epoch: int) -> None:
    original_mode = stat.S_IMODE(path.stat().st_mode)
    try:
        with tarfile.open(path, mode="r:") as source:
            members = source.getmembers()
            names = [member.name for member in members]
            if len(names) != len(set(names)) or set(names) != set(EXPECTED_PARTS):
                raise NormalizationError(
                    f"gem must contain exactly {', '.join(EXPECTED_PARTS)}"
                )
            parts = {member.name: _read_regular_member(source, member) for member in members}
    except tarfile.TarError as error:
        raise NormalizationError(f"invalid gem tar: {error}") from error

    metadata_raw = _canonical_metadata_yaml(
        _decompress_bounded(parts["metadata.gz"], "metadata.gz")
    )

    metadata = _canonical_gzip(metadata_raw)
    data = _canonical_gzip(_canonical_data_tar(parts["data.tar.gz"], epoch))
    checksums = _canonical_gzip(_checksums(metadata, data))
    canonical = {
        "metadata.gz": metadata,
        "data.tar.gz": data,
        "checksums.yaml.gz": checksums,
    }

    temporary_name = ""
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as temporary:
            temporary_name = temporary.name
        with tarfile.open(temporary_name, mode="w", format=tarfile.USTAR_FORMAT) as target:
            for name in EXPECTED_PARTS:
                payload = canonical[name]
                info = tarfile.TarInfo(name)
                info.size = len(payload)
                info.mode = 0o444
                info.mtime = epoch
                info.uid = 0
                info.gid = 0
                info.uname = ""
                info.gname = ""
                target.addfile(info, io.BytesIO(payload))
        os.chmod(temporary_name, original_mode)
        os.replace(temporary_name, path)
    finally:
        if temporary_name:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <artifact.gem>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1]).resolve()
    if not path.is_file() or path.is_symlink():
        print(f"normalize-ruby-gem: artifact is not a regular file: {path}", file=sys.stderr)
        return 2
    try:
        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
        if epoch < 0 or epoch > 8_589_934_591:
            raise ValueError
        normalize(path, epoch)
    except (NormalizationError, OSError, ValueError) as error:
        print(f"normalize-ruby-gem: {error}", file=sys.stderr)
        return 1
    print(f"normalized {path} (SOURCE_DATE_EPOCH={epoch})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
