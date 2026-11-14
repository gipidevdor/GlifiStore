from __future__ import annotations

import gzip
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def write_test_gem(
    path: Path,
    *,
    compresslevel: int,
    mtime: int,
    data_name: str = "lib/glifistore.rb",
    null_spacing: bytes = b"",
) -> None:
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        payload = b"canonical gem payload"
        info = tarfile.TarInfo(data_name)
        info.size = len(payload)
        info.mtime = mtime
        info.uid = mtime
        info.gid = mtime
        archive.addfile(info, io.BytesIO(payload))
    members = {
        "metadata.gz": gzip.compress(
            b"---\nname: glifistore\nautorequire:"
            + null_spacing
            + b"\nsigning_key:"
            + null_spacing
            + b"\n",
            compresslevel=compresslevel,
            mtime=mtime,
        ),
        "data.tar.gz": gzip.compress(data.getvalue(), compresslevel=compresslevel, mtime=mtime),
        "checksums.yaml.gz": gzip.compress(b"stale\n", compresslevel=compresslevel, mtime=mtime),
    }
    with tarfile.open(path, "w") as archive:
        for name, payload in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            info.mtime = mtime
            info.mode = 0o444
            archive.addfile(info, io.BytesIO(payload))


def write_test_perl_dist(path: Path, *, generated_by: str, backend: str) -> None:
    payloads = {
        "GlifiStore-0.1.0/META.json": (
            '{\n   "generated_by" : "'
            + generated_by
            + '",\n   "x_serialization_backend" : "'
            + backend
            + '"\n}\n'
        ).encode(),
        "GlifiStore-0.1.0/META.yml": (
            f"generated_by: '{generated_by}'\n"
            f"x_serialization_backend: '{backend}'\n"
        ).encode(),
        "GlifiStore-0.1.0/lib/GlifiStore.pm": b"package GlifiStore; 1;\n",
    }
    with tarfile.open(path, "w:gz") as archive:
        for name, payload in payloads.items():
            info = tarfile.TarInfo(name)
            info.size = len(payload)
            archive.addfile(info, io.BytesIO(payload))


class PackagingReproducibilityTests(unittest.TestCase):
    def test_perl_metadata_normalization_removes_generator_version_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.tar.gz"
            second = root / "second.tar.gz"
            write_test_perl_dist(first, generated_by="ExtUtils 7.62", backend="JSON::PP 4.07")
            write_test_perl_dist(second, generated_by="ExtUtils 7.78", backend="JSON::PP 4.16")
            normalizer = ROOT / "scripts/normalize-perl-dist-metadata.sh"
            tar_normalizer = ROOT / "scripts/normalize-tar-gz.sh"
            env = dict(os.environ, SOURCE_DATE_EPOCH="1780000000")
            for archive in (first, second):
                subprocess.run([normalizer, archive], check=True, env=env, capture_output=True, text=True)
                subprocess.run([tar_normalizer, archive], check=True, env=env, capture_output=True, text=True)
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_ruby_gem_normalization_removes_compressor_and_metadata_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.gem"
            second = root / "second.gem"
            write_test_gem(first, compresslevel=1, mtime=100, null_spacing=b"")
            write_test_gem(second, compresslevel=9, mtime=200, null_spacing=b" ")
            env = dict(os.environ, SOURCE_DATE_EPOCH="1780000000")
            normalizer = ROOT / "scripts/normalize-ruby-gem.sh"
            subprocess.run([normalizer, first], check=True, env=env, capture_output=True, text=True)
            subprocess.run([normalizer, second], check=True, env=env, capture_output=True, text=True)
            self.assertEqual(first.read_bytes(), second.read_bytes())

            with tarfile.open(first) as archive:
                metadata = archive.extractfile("metadata.gz")
                data = archive.extractfile("data.tar.gz")
                checksums = archive.extractfile("checksums.yaml.gz")
                assert metadata is not None and data is not None and checksums is not None
                metadata_bytes = metadata.read()
                data_bytes = data.read()
                checksum_text = gzip.decompress(checksums.read()).decode("utf-8")
            with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data_bytes)), mode="r:") as data_tar:
                members = data_tar.getmembers()
            self.assertEqual([member.name for member in members], ["lib/glifistore.rb"])
            self.assertEqual(members[0].mtime, 1_780_000_000)
            self.assertEqual((members[0].uid, members[0].gid), (0, 0))
            self.assertEqual(members[0].mode, 0o644)
            self.assertIn(hashlib.sha256(metadata_bytes).hexdigest(), checksum_text)
            self.assertIn(hashlib.sha512(metadata_bytes).hexdigest(), checksum_text)
            self.assertIn(hashlib.sha256(data_bytes).hexdigest(), checksum_text)
            self.assertIn(hashlib.sha512(data_bytes).hexdigest(), checksum_text)

    def test_ruby_gem_normalization_rejects_unsafe_nested_paths_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            archive = Path(directory) / "unsafe.gem"
            write_test_gem(
                archive,
                compresslevel=9,
                mtime=100,
                data_name="../outside.rb",
            )
            original = archive.read_bytes()
            result = subprocess.run(
                [ROOT / "scripts/normalize-ruby-gem.sh", archive],
                check=False,
                env=dict(os.environ, SOURCE_DATE_EPOCH="1780000000"),
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsafe path", result.stderr)
            self.assertEqual(archive.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()
