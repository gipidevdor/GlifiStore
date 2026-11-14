from __future__ import annotations

import hashlib
import json
import lzma
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

from engineering.tools.generate_package_matrix import backend as matrix_backend, load_matrix
from engineering.tools.generate_release_context import build_context
from engineering.tools.macos_prefix_isolation import (
    PrefixIsolationError,
    foreign_links,
    load_exceptions,
    parse_otool,
)
from engineering.tools.render_macos_packaging import (
    MacPackagingError,
    render,
    render_to_file,
    resolve_source,
    source_basename,
)

ROOT = Path(__file__).resolve().parents[2]
PACKAGE_CI = ROOT / "scripts/package-ci.sh"
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
SEALED_URL = (
    f"https://github.com/gpicchiarelli/GlifiStore/releases/download/v{VERSION}/"
    f"{source_basename(VERSION)}"
)
SEALED_SHA256 = "a" * 64


def temporary_directory(case: unittest.TestCase, prefix: str) -> Path:
    directory = tempfile.TemporaryDirectory(prefix=prefix)
    case.addCleanup(directory.cleanup)
    return Path(directory.name)


def local_archive(directory: Path, *, name: str | None = None) -> tuple[Path, str]:
    path = directory / (name or source_basename(VERSION))
    with lzma.open(path, "wb", preset=0) as stream:
        stream.write(b"not a real source archive, only bytes to hash\n")
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


class MacOsPackagingRenderTests(unittest.TestCase):
    """Version injection: the templates own the shape, VERSION owns the number."""

    def context(self, *, package_revision: int = 0, **git_overrides: Any) -> dict[str, Any]:
        # The guards under test read the git identity, so a test that needs a
        # sealed-looking release states that explicitly instead of tagging a repo.
        value = build_context(ROOT, package_revision=package_revision)
        value["git"].update(git_overrides)
        return value

    def sealed(self) -> Any:
        return resolve_source(
            url=SEALED_URL,
            sha256=SEALED_SHA256,
            size=1024,
            product_version=VERSION,
            profile="release",
        )

    def released_context(self, *, package_revision: int = 0) -> dict[str, Any]:
        return self.context(
            package_revision=package_revision,
            tree_clean=True,
            tag_is_annotated=True,
            tag_matches_commit=True,
        )

    def test_the_portfile_takes_its_version_from_the_version_authority(self) -> None:
        rendered = render(
            "macports", self.released_context(), self.sealed(), profile="release", root=ROOT
        )

        self.assertIn(f"\nversion                 {VERSION}\n", rendered)
        self.assertIn("\nrevision                0\n", rendered)
        self.assertIn("distname                GlifiStore-${version}", rendered)
        self.assertIn(f"sha256  {SEALED_SHA256}", rendered)
        self.assertIn("size    1024", rendered)
        self.assertIn(
            "master_sites            "
            f"https://github.com/gpicchiarelli/GlifiStore/releases/download/v{VERSION}/",
            rendered,
        )
        self.assertNotIn("{{", rendered)

    def test_the_portfile_declares_an_unprivileged_launchd_startup_item(self) -> None:
        rendered = render(
            "macports", self.released_context(), self.sealed(), profile="release", root=ROOT
        )

        self.assertIn("startupitem.create      yes", rendered)
        self.assertIn("startupitem.user        glifistore", rendered)
        self.assertIn("startupitem.group       glifistore", rendered)
        self.assertIn("startupitem.executable  ${prefix}/bin/glifistored", rendered)
        self.assertIn("add_users               glifistore group=glifistore", rendered)

    def test_the_portfile_carries_the_package_revision(self) -> None:
        rendered = render(
            "macports",
            self.released_context(package_revision=4),
            self.sealed(),
            profile="release",
            root=ROOT,
        )

        self.assertIn("\nrevision                4\n", rendered)
        self.assertIn(f"\nversion                 {VERSION}\n", rendered)

    def test_the_formula_takes_its_version_and_source_from_the_context(self) -> None:
        rendered = render(
            "homebrew", self.released_context(), self.sealed(), profile="release", root=ROOT
        )

        self.assertIn(f'  version "{VERSION}"\n', rendered)
        self.assertIn(f'  url "{SEALED_URL}"\n', rendered)
        self.assertIn(f'  sha256 "{SEALED_SHA256}"\n', rendered)
        self.assertNotIn("revision ", rendered)
        self.assertNotIn("{{", rendered)

    def test_the_formula_carries_a_non_zero_package_revision(self) -> None:
        rendered = render(
            "homebrew",
            self.released_context(package_revision=2),
            self.sealed(),
            profile="release",
            root=ROOT,
        )

        self.assertIn("\n  revision 2\n", rendered)

    def test_neither_template_hard_codes_a_version_or_a_checksum(self) -> None:
        for name in ("packaging/macports/Portfile.in", "packaging/homebrew/glifistore.rb.in"):
            with self.subTest(template=name):
                template = (ROOT / name).read_text(encoding="utf-8")
                self.assertNotIn(VERSION, template)
                self.assertNotIn("sha256  ", template)
                self.assertIn("{{VERSION}}", template)

    def test_rendering_writes_the_name_each_package_manager_expects(self) -> None:
        directory = temporary_directory(self, "glifistore-render-")
        context = self.released_context()

        portfile = render_to_file(
            "macports", context, self.sealed(), profile="release", directory=directory, root=ROOT
        )
        formula = render_to_file(
            "homebrew", context, self.sealed(), profile="release", directory=directory, root=ROOT
        )

        self.assertEqual(portfile.name, "Portfile")
        self.assertEqual(formula.name, "glifistore.rb")


class ReleaseSourceAdmissionTests(unittest.TestCase):
    """The release profile admits sealed bytes only; never a checkout of HEAD."""

    def test_a_sealed_https_archive_is_admitted(self) -> None:
        source = resolve_source(
            url=SEALED_URL,
            sha256=SEALED_SHA256,
            size=4096,
            product_version=VERSION,
            profile="release",
        )

        self.assertEqual(source.distname, f"GlifiStore-{VERSION}")
        self.assertTrue(source.master_sites.endswith("/"))

    def test_the_release_profile_refuses_a_local_working_copy(self) -> None:
        directory = temporary_directory(self, "glifistore-source-")
        archive, sha256 = local_archive(directory)

        with self.assertRaises(MacPackagingError) as refused:
            resolve_source(
                url=archive.as_uri(),
                sha256=sha256,
                size=None,
                product_version=VERSION,
                profile="release",
            )

        self.assertIn("https", str(refused.exception))

    def test_the_release_profile_refuses_a_git_ref_tarball(self) -> None:
        for url in (
            f"https://github.com/gpicchiarelli/GlifiStore/archive/refs/tags/{source_basename(VERSION)}",
            f"https://codeload.github.com/gpicchiarelli/GlifiStore/{source_basename(VERSION)}",
        ):
            with self.subTest(url=url), self.assertRaises(MacPackagingError) as refused:
                resolve_source(
                    url=url,
                    sha256=SEALED_SHA256,
                    size=None,
                    product_version=VERSION,
                    profile="release",
                )
            self.assertIn("sealed source archive", str(refused.exception))

    def test_an_incomplete_or_mismatched_identity_is_refused(self) -> None:
        cases = {
            "no url": {"url": "", "sha256": SEALED_SHA256},
            "no digest": {"url": SEALED_URL, "sha256": ""},
            "short digest": {"url": SEALED_URL, "sha256": "abc"},
            "uppercase digest": {"url": SEALED_URL, "sha256": "A" * 64},
            "wrong archive": {
                "url": SEALED_URL.replace(source_basename(VERSION), "GlifiStore-9.9.9.tar.xz"),
                "sha256": SEALED_SHA256,
            },
            "unsupported scheme": {
                "url": f"ftp://example.invalid/{source_basename(VERSION)}",
                "sha256": SEALED_SHA256,
            },
        }
        for name, arguments in cases.items():
            with self.subTest(case=name), self.assertRaises(MacPackagingError):
                resolve_source(
                    size=None, product_version=VERSION, profile="release", **arguments
                )

    def test_a_local_archive_must_match_the_digest_it_pins(self) -> None:
        directory = temporary_directory(self, "glifistore-source-")
        archive, sha256 = local_archive(directory)

        admitted = resolve_source(
            url=archive.as_uri(),
            sha256=sha256,
            size=None,
            product_version=VERSION,
            profile="pr",
        )
        self.assertEqual(admitted.size, archive.stat().st_size)

        with self.assertRaises(MacPackagingError) as refused:
            resolve_source(
                url=archive.as_uri(),
                sha256="b" * 64,
                size=None,
                product_version=VERSION,
                profile="pr",
            )
        self.assertIn("digest mismatch", str(refused.exception))

    def test_the_release_profile_refuses_an_unsealed_release_context(self) -> None:
        context = build_context(ROOT)
        source = resolve_source(
            url=SEALED_URL,
            sha256=SEALED_SHA256,
            size=None,
            product_version=VERSION,
            profile="release",
        )
        for overrides in (
            {"tree_clean": False, "tag_is_annotated": True, "tag_matches_commit": True},
            {"tree_clean": True, "tag_is_annotated": True, "tag_matches_commit": False},
            {"tree_clean": True, "tag_is_annotated": False, "tag_matches_commit": False},
        ):
            with self.subTest(**overrides):
                unsealed = json.loads(json.dumps(context))
                unsealed["git"].update(overrides)
                with self.assertRaises(MacPackagingError):
                    render("macports", unsealed, source, profile="release", root=ROOT)


class PrefixIsolationTests(unittest.TestCase):
    OTOOL = (
        "/opt/local/lib/libglifistore.1.0.dylib:\n"
        "\t/opt/local/lib/libglifistore.1.dylib (compatibility version 1.0.0, current version 1.0.0)\n"
        "\t/opt/local/lib/libssl.3.dylib (compatibility version 3.0.0, current version 3.0.0)\n"
        "\t@rpath/libextra.dylib (compatibility version 1.0.0, current version 1.0.0)\n"
        "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1351.0.0)\n"
        "\t/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib (compatibility version 3.0.0, current version 3.0.0)\n"
        "\t/usr/local/lib/libsomething.dylib (compatibility version 1.0.0, current version 1.0.0)\n"
    )

    def test_only_absolute_dependencies_are_parsed(self) -> None:
        dependencies = parse_otool(self.OTOOL)

        self.assertNotIn("/opt/local/lib/libglifistore.1.0.dylib:", dependencies)
        self.assertNotIn("@rpath/libextra.dylib", dependencies)
        self.assertIn("/usr/lib/libSystem.B.dylib", dependencies)
        self.assertEqual(len(dependencies), 5)

    def test_macports_refuses_homebrew_and_usr_local(self) -> None:
        violations = foreign_links("macports", "daemon", parse_otool(self.OTOOL), {})

        self.assertEqual(
            sorted(violation.dependency for violation in violations),
            [
                "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib",
                "/usr/local/lib/libsomething.dylib",
            ],
        )

    def test_homebrew_refuses_macports_but_keeps_usr_local(self) -> None:
        violations = foreign_links("homebrew", "daemon", parse_otool(self.OTOOL), {})

        self.assertEqual(
            sorted(violation.dependency for violation in violations),
            ["/opt/local/lib/libglifistore.1.dylib", "/opt/local/lib/libssl.3.dylib"],
        )

    def test_an_exception_only_counts_when_it_is_documented(self) -> None:
        justified = {"/opt/local/lib/libssl.3.dylib": "documented reason"}

        violations = foreign_links("homebrew", "daemon", parse_otool(self.OTOOL), justified)

        self.assertEqual(
            [violation.dependency for violation in violations],
            ["/opt/local/lib/libglifistore.1.dylib"],
        )

    def test_an_exception_without_a_reason_is_refused(self) -> None:
        root = temporary_directory(self, "glifistore-exceptions-")
        directory = root / "packaging/macports"
        directory.mkdir(parents=True)
        (directory / "prefix-exceptions.txt").write_text(
            "# a comment\n/opt/homebrew/lib/libfoo.dylib\n", encoding="utf-8"
        )

        with self.assertRaises(PrefixIsolationError):
            load_exceptions("macports", root)

    def test_no_backend_ships_a_prefix_exception_today(self) -> None:
        for backend in ("macports", "homebrew"):
            with self.subTest(backend=backend):
                self.assertEqual(load_exceptions(backend, ROOT), {})


class MacOsPackageCiTests(unittest.TestCase):
    def run_package_ci(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(PACKAGE_CI), *arguments],
            check=False,
            capture_output=True,
            text=True,
            cwd=ROOT,
        )

    def evidence(self, directory: Path, backend: str, profile: str) -> dict[str, Any]:
        path = (
            directory / backend / "full" / f"{backend}-{profile}-full-package-evidence.json"
        )
        self.assertTrue(path.is_file(), f"missing evidence: {path}")
        return json.loads(path.read_text(encoding="utf-8"))

    def test_macports_renders_metadata_and_never_reports_pass(self) -> None:
        directory = temporary_directory(self, "glifistore-macports-") / "run"

        completed = self.run_package_ci(
            "--profile", "pr", "--backend", "macports", "--output-dir", str(directory)
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("PACKAGE-CI macports pr full OPEN_GATE", completed.stdout)
        portfile = directory / "macports/full/rendered/Portfile"
        self.assertTrue(portfile.is_file())
        self.assertIn(f"version                 {VERSION}", portfile.read_text(encoding="utf-8"))

        evidence = self.evidence(directory, "macports", "pr")
        statuses = {check["id"]: check["status"] for check in evidence["checks"]}
        self.assertEqual(evidence["result"], "OPEN_GATE")
        self.assertEqual(evidence["lifecycle_state"], "STRUCTURAL")
        self.assertEqual(statuses["structural-metadata"], "PASS")
        self.assertEqual(statuses["package-metadata-render"], "PASS")
        self.assertEqual(statuses["service-lifecycle"], "OPEN_GATE")
        self.assertEqual(statuses["upstream-ports-acceptance"], "OPEN_GATE")
        self.assertEqual(statuses["package-upgrade"], "NOT_APPLICABLE_INITIAL_BASELINE")
        self.assertNotIn("PASS", {statuses[check] for check in ("package-build", "package-install")})

    def test_homebrew_renders_metadata_on_the_main_profile(self) -> None:
        directory = temporary_directory(self, "glifistore-homebrew-") / "run"

        completed = self.run_package_ci(
            "--profile", "main", "--backend", "homebrew", "--output-dir", str(directory)
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("PACKAGE-CI homebrew main full OPEN_GATE", completed.stdout)
        formula = directory / "homebrew/full/rendered/glifistore.rb"
        self.assertTrue(formula.is_file())
        self.assertIn(f'version "{VERSION}"', formula.read_text(encoding="utf-8"))
        self.assertEqual(self.evidence(directory, "homebrew", "main")["result"], "OPEN_GATE")

    def test_the_service_gate_keeps_both_backends_out_of_release_requirements(self) -> None:
        matrix = load_matrix()
        for backend in ("macports", "homebrew"):
            with self.subTest(backend=backend):
                entry = matrix_backend(matrix, backend)
                self.assertFalse(entry["required_for_release"])
                self.assertEqual(entry["status"], "STRUCTURAL")
                self.assertIn("service-lifecycle", entry["checks"])
                self.assertIn("prefix-isolation", entry["checks"])


class ReleaseProfileDriverTests(unittest.TestCase):
    """scripts/package-ci.sh --profile release must not fall back to a checkout."""

    def test_the_driver_fails_without_a_sealed_source_archive(self) -> None:
        directory = temporary_directory(self, "glifistore-release-")
        context_path = directory / "release-context.json"
        context_path.write_text(
            json.dumps(build_context(ROOT), indent=2, sort_keys=True), encoding="utf-8"
        )
        output = directory / "run"

        environment = dict(os.environ)
        for name in (
            "GLIFISTORE_SOURCE_ARCHIVE_URL",
            "GLIFISTORE_SOURCE_ARCHIVE_SHA256",
            "GLIFISTORE_PACKAGE_CI_NATIVE",
        ):
            environment.pop(name, None)

        completed = subprocess.run(
            [
                sys.executable,
                str(ROOT / "engineering/tools/run_macos_package_backend.py"),
                "--backend",
                "macports",
                "--profile",
                "release",
                "--release-context",
                str(context_path),
                "--output-dir",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
            cwd=ROOT,
            env=environment,
        )

        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        evidence = json.loads(
            (output / "macports-release-full-package-evidence.json").read_text(encoding="utf-8")
        )
        checks = {check["id"]: check for check in evidence["checks"]}
        self.assertEqual(evidence["result"], "FAIL")
        self.assertEqual(evidence["lifecycle_state"], "NONE")
        self.assertEqual(checks["package-metadata-render"]["status"], "FAIL")
        self.assertIn(
            "GLIFISTORE_SOURCE_ARCHIVE_URL", checks["package-metadata-render"]["detail"]
        )
        self.assertFalse((output / "rendered/Portfile").exists())


if __name__ == "__main__":
    unittest.main()
