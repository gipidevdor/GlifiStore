"""Tests for the Debian and RPM packaging helpers (Wave B).

These cover what is decidable without dpkg, rpm, systemd or a container: the
metadata rendering contract, the payload inventory checks, the consumer
isolation guard, and the result/lifecycle summarisation. The rows that actually
build and install a package are exercised by the lifecycle itself, which reports
BLOCKED wherever this suite runs.
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from engineering.tools.assert_consumer_isolation import (
    ENVIRONMENT_ROOTS,
    IsolationError,
    inside,
    normalise_roots,
    scan_compile_commands,
    scan_text,
)
from engineering.tools.generate_package_matrix import backend as matrix_backend, load_matrix
from engineering.tools.generate_release_context import build_context
from engineering.tools.package_framework import encode_json
from engineering.tools.render_package_metadata import (
    LAYOUTS,
    RenderError,
    build_tokens,
    render,
    resolve_layout,
    substitute,
)
from engineering.tools.run_linux_package_backend import (
    container_run_arguments,
    lifecycle_state,
    overall_result,
    prefer_inner_container_evidence,
)
from engineering.tools.verify_package_payload import (
    PayloadError,
    component,
    load_payload,
    resolve,
    verify,
)

ROOT = Path(__file__).resolve().parents[2]
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def temporary_directory(case: unittest.TestCase, prefix: str) -> Path:
    directory = tempfile.TemporaryDirectory(prefix=prefix)
    case.addCleanup(directory.cleanup)
    return Path(directory.name)


def release_context(case: unittest.TestCase, *, revision: int = 1) -> Path:
    """A real release context, so no test invents its own version authority."""
    path = temporary_directory(case, "glifistore-context-") / "release-context.json"
    path.write_text(
        encode_json(build_context(ROOT, package_revision=revision)), encoding="utf-8"
    )
    return path


class MetadataRenderingTests(unittest.TestCase):
    def rendered(self, backend: str, *, revision: int = 1) -> tuple[Path, dict]:
        output = temporary_directory(self, "glifistore-render-") / "metadata"
        manifest = render(release_context(self, revision=revision), backend, output)
        return output, manifest

    def test_the_debian_metadata_comes_out_complete(self) -> None:
        output, manifest = self.rendered("deb")
        for relative in (
            "debian/control",
            "debian/changelog",
            "debian/rules",
            "debian/copyright",
            "debian/source/format",
            "debian/glifistore.postinst",
            "debian/glifistore.postrm",
            "debian/glifistore.glifistored.service",
        ):
            with self.subTest(file=relative):
                self.assertTrue((output / relative).is_file(), relative)
        self.assertEqual((output / "debian/rules").stat().st_mode & 0o777, 0o755)
        self.assertEqual((output / "debian/control").stat().st_mode & 0o777, 0o644)
        self.assertEqual(manifest["backend"], "deb")
        self.assertEqual(manifest["product_version"], VERSION)

    def test_the_rpm_spec_comes_out_complete(self) -> None:
        output, manifest = self.rendered("rpm")
        spec = (output / "glifistore.spec").read_text(encoding="utf-8")
        self.assertTrue((output / "glifistored.service").is_file())
        self.assertIn(f"Version:        {VERSION}", spec)
        self.assertIn("%config(noreplace)", spec)
        self.assertEqual(manifest["layout"]["libdir"], "/usr/lib64")

    def test_the_version_is_never_hard_coded_in_a_template(self) -> None:
        # The templates are the thing most likely to drift into a literal version,
        # so the renderer refuses to read one and this proves the refusal is live.
        for template in sorted(ROOT.glob("packaging/*/templates/*")):
            if template.is_dir():
                continue
            with self.subTest(template=template.name):
                self.assertNotIn(VERSION, template.read_text(encoding="utf-8"))

    def test_the_release_context_drives_the_package_version(self) -> None:
        # Debian revisions start at 1, so package_revision 0 is the first build of
        # an upstream version; semver_policy owns that offset, not the renderer.
        _, first = self.rendered("deb", revision=0)
        _, second = self.rendered("deb", revision=3)
        self.assertEqual(first["package_version"], f"{VERSION}-1")
        self.assertEqual(second["package_version"], f"{VERSION}-4")
        self.assertEqual(second["package_revision"], 3)

    def test_rendering_never_silently_overwrites(self) -> None:
        output, _ = self.rendered("deb")
        with self.assertRaisesRegex(RenderError, "existing directory"):
            render(release_context(self), "deb", output)

    def test_an_unresolved_or_unknown_token_is_refused(self) -> None:
        tokens = build_tokens(
            json.loads(release_context(self).read_text(encoding="utf-8")),
            "deb",
            resolve_layout("deb"),
        )
        with self.assertRaisesRegex(RenderError, "unknown placeholder"):
            substitute("Depends: @NOT_A_TOKEN@", tokens, origin="test")
        self.assertEqual(substitute("@BINDIR@", tokens, origin="test"), "/usr/bin")

    def test_only_the_declared_layout_keys_can_be_overridden(self) -> None:
        layout = resolve_layout("rpm", {"libdir": "/usr/lib"})
        self.assertEqual(layout["libdir"], "/usr/lib")
        self.assertEqual(layout["sysconfdir"], LAYOUTS["rpm"]["sysconfdir"])
        with self.assertRaisesRegex(RenderError, "not an overridable layout entry"):
            resolve_layout("rpm", {"sysconfdir": "/opt/etc"})

    def test_the_service_account_snippet_keeps_its_indentation(self) -> None:
        # The snippet is inlined into an `if` block in postinst; losing the
        # indentation would still be valid shell but would read as a different
        # block to anyone reviewing the maintainer script.
        output, _ = self.rendered("deb")
        postinst = (output / "debian/glifistore.postinst").read_text(encoding="utf-8")
        self.assertIn("    if ! getent group 'glifistore'", postinst)
        self.assertNotIn("\nif ! getent group", postinst)

    def test_both_backends_install_the_same_unit(self) -> None:
        deb_output, _ = self.rendered("deb")
        rpm_output, _ = self.rendered("rpm")
        deb_unit = (deb_output / "debian/glifistore.glifistored.service").read_text(
            encoding="utf-8"
        )
        rpm_unit = (rpm_output / "glifistored.service").read_text(encoding="utf-8")
        self.assertEqual(deb_unit, rpm_unit)
        self.assertIn("StateDirectory=glifistore", deb_unit)
        self.assertIn("ProtectSystem=strict", deb_unit)

    def test_the_unit_is_installed_but_never_enabled(self) -> None:
        output, _ = self.rendered("deb")
        rules = (output / "debian/rules").read_text(encoding="utf-8")
        self.assertIn("--no-enable", rules)
        self.assertIn("--no-start", rules)


class PayloadInventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.payload = load_payload()
        self.tokens = {
            "bindir": "/usr/bin",
            "libdir": "/usr/lib",
            "includedir": "/usr/include",
            "datadir": "/usr/share",
            "mandir": "/usr/share/man",
            "sysconfdir": "/etc",
            "unitdir": "/lib/systemd/system",
            "statedir": "/var/lib/glifistore",
            "abi_major": "1",
            "abi_version": "1.0.0",
        }

    def test_every_declared_token_is_resolvable(self) -> None:
        self.assertEqual(sorted(self.payload["tokens"]), sorted(self.tokens))
        for identifier in [entry["id"] for entry in self.payload["components"]]:
            for entry in component(self.payload, identifier)["entries"]:
                with self.subTest(component=identifier, path=entry["path"]):
                    self.assertNotIn("@", resolve(entry["path"], self.tokens))

    def test_configuration_and_data_are_separate_components(self) -> None:
        # The removal policy depends on this split: configuration is purged, durable
        # data never is, so they can never live in the same component.
        configuration = [
            entry["path"] for entry in component(self.payload, "configuration")["entries"]
        ]
        data = [entry["path"] for entry in component(self.payload, "data")["entries"]]
        self.assertEqual(configuration, ["@sysconfdir@/glifistore/glifistored.conf"])
        self.assertEqual(data, ["@statedir@"])

    def test_a_missing_entry_is_reported_and_a_present_one_is_not(self) -> None:
        root = temporary_directory(self, "glifistore-payload-")
        (root / "etc/glifistore").mkdir(parents=True)
        failures = verify(root, self.tokens, present=["configuration"], absent=[], payload=self.payload)
        self.assertEqual(len(failures), 1)
        self.assertIn("missing", failures[0])

        (root / "etc/glifistore/glifistored.conf").write_text("port = 7000\n", encoding="utf-8")
        self.assertEqual(
            verify(root, self.tokens, present=["configuration"], absent=[], payload=self.payload),
            [],
        )

    def test_a_surviving_entry_fails_an_absence_check(self) -> None:
        root = temporary_directory(self, "glifistore-payload-")
        (root / "lib/systemd/system").mkdir(parents=True)
        (root / "lib/systemd/system/glifistored.service").write_text("[Unit]\n", encoding="utf-8")
        failures = verify(root, self.tokens, present=[], absent=["service"], payload=self.payload)
        self.assertEqual(len(failures), 1)
        self.assertIn("still present", failures[0])

    def test_the_declared_kind_is_enforced(self) -> None:
        root = temporary_directory(self, "glifistore-payload-")
        (root / "usr/bin").mkdir(parents=True)
        (root / "usr/bin/glifistored").write_text("#!/bin/sh\n", encoding="utf-8")
        failures = verify(root, self.tokens, present=["runtime"], absent=[], payload=self.payload)
        self.assertTrue(any("not executable" in failure for failure in failures), failures)

    def test_a_contradictory_or_empty_request_is_refused(self) -> None:
        root = temporary_directory(self, "glifistore-payload-")
        with self.assertRaisesRegex(PayloadError, "at least one"):
            verify(root, self.tokens, present=[], absent=[], payload=self.payload)
        with self.assertRaisesRegex(PayloadError, "present and absent"):
            verify(root, self.tokens, present=["data"], absent=["data"], payload=self.payload)
        with self.assertRaisesRegex(PayloadError, "unknown payload component"):
            verify(root, self.tokens, present=["telemetry"], absent=[], payload=self.payload)


class ConsumerIsolationTests(unittest.TestCase):
    def test_a_source_checkout_include_is_caught(self) -> None:
        violations = scan_text(
            "c++ -I/src/include -o smoke smoke.cpp", ["/src"], origin="build.log"
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("/src/include", violations[0])

    def test_a_path_that_merely_starts_with_the_root_is_not_a_violation(self) -> None:
        # /srclib is a different directory from /src, and a consumer working
        # directory named after it must not be mistaken for the checkout.
        self.assertEqual(scan_text("c++ -I/srclib/include", ["/src"], origin="log"), [])
        self.assertIsNone(inside("/srclib/include", ["/src"]))
        self.assertIsNotNone(inside("/src/include/../include", ["/src"]))

    def test_every_guarded_flag_form_is_recognised(self) -> None:
        text = " ".join(
            (
                "-I/src/a",
                "-isystem/src/b",
                "-L/src/c",
                "--sysroot=/src/d",
                "-Wl,-rpath,/src/e",
                "-isysroot/src/f",
            )
        )
        violations = scan_text(text, ["/src"], origin="log")
        self.assertEqual(len(violations), 6, violations)

    def test_compile_commands_are_scanned_with_relative_includes_resolved(self) -> None:
        directory = temporary_directory(self, "glifistore-isolation-")
        database = directory / "compile_commands.json"
        database.write_text(
            json.dumps(
                [
                    {
                        "directory": "/src/build",
                        "file": "/work/smoke.cpp",
                        "arguments": ["c++", "-I../include", "-c", "/work/smoke.cpp"],
                    },
                    {
                        "directory": "/work/build",
                        "file": "/work/smoke.cpp",
                        "command": "c++ -I/usr/include -c /work/smoke.cpp",
                    },
                ]
            ),
            encoding="utf-8",
        )
        violations = scan_compile_commands(database, ["/src"])
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("/src/include", violations[0])

    def test_the_ci_workspace_is_a_forbidden_root(self) -> None:
        # On a GitHub runner the checkout lives in GITHUB_WORKSPACE, which is where
        # a consumer would accidentally pick up headers instead of the installed ones.
        workspace = "/home/runner/work/GlifiStore/GlifiStore"
        roots = normalise_roots([workspace])
        violations = scan_text(f"c++ -I{workspace}/include -L/usr/lib", roots, origin="log")
        self.assertEqual(len(violations), 1, violations)
        self.assertIn(f"{workspace}/include", violations[0])
        self.assertIn("GITHUB_WORKSPACE", ENVIRONMENT_ROOTS)

    def test_an_empty_or_universal_root_set_is_refused(self) -> None:
        with self.assertRaisesRegex(IsolationError, "no forbidden root"):
            normalise_roots([])
        with self.assertRaisesRegex(IsolationError, "cannot be a forbidden root"):
            normalise_roots(["/"])


class SummarisationTests(unittest.TestCase):
    def test_a_single_failure_outranks_everything(self) -> None:
        self.assertEqual(overall_result({"a": "PASS", "b": "BLOCKED", "c": "FAIL"}), "FAIL")

    def test_pass_needs_every_row_settled(self) -> None:
        self.assertEqual(
            overall_result({"a": "PASS", "b": "NOT_APPLICABLE_INITIAL_BASELINE"}), "PASS"
        )
        self.assertEqual(overall_result({"a": "PASS", "b": "NOT_RUN"}), "NOT_RUN")
        self.assertEqual(overall_result({"a": "PASS", "b": "BLOCKED"}), "BLOCKED")
        self.assertEqual(overall_result({"a": "BLOCKED", "b": "OPEN_GATE"}), "OPEN_GATE")

    def test_the_lifecycle_state_stops_at_the_first_incomplete_tier(self) -> None:
        self.assertEqual(lifecycle_state({}), "NONE")
        structural = {"structural-metadata": "PASS", "package-metadata-render": "PASS"}
        self.assertEqual(lifecycle_state(structural), "STRUCTURAL")
        built = {**structural, "package-lint": "PASS", "package-build": "PASS"}
        self.assertEqual(lifecycle_state(built), "BUILT")
        # A skipped row is not a passed row: an upgrade that never applied cannot
        # promote the backend to UPGRADE_VERIFIED.
        installed = {
            **built,
            "package-inspect": "PASS",
            "prefix-isolation": "PASS",
            "package-install": "PASS",
            "external-consumer": "PASS",
            "put-get-erase": "PASS",
            "restart-recovery": "PASS",
            "service-lifecycle": "PASS",
            "config-preservation": "PASS",
            "package-remove": "PASS",
            "package-upgrade": "NOT_APPLICABLE_INITIAL_BASELINE",
        }
        self.assertEqual(lifecycle_state(installed), "LIFECYCLE_VERIFIED")


class MatrixAgreementTests(unittest.TestCase):
    def setUp(self) -> None:
        self.matrix = load_matrix()

    def test_the_linux_backends_stay_unpromoted_but_pin_their_images(self) -> None:
        for identifier in ("deb", "rpm"):
            with self.subTest(backend=identifier):
                entry = matrix_backend(self.matrix, identifier)
                self.assertEqual(entry["status"], "STRUCTURAL")
                self.assertFalse(entry["required_for_release"])
                self.assertTrue(entry["limitations"])
                for target in entry["targets"]:
                    self.assertTrue(
                        str(target["container_digest"]).startswith("sha256:"),
                        target["id"],
                    )

    def test_only_the_host_independent_rows_are_required(self) -> None:
        # Requiring a row that cannot run outside a container would force the
        # lifecycle to either lie or fail every ordinary run.
        for identifier in ("deb", "rpm"):
            entry = matrix_backend(self.matrix, identifier)
            for profile, required in entry["required_checks"].items():
                with self.subTest(backend=identifier, profile=profile):
                    self.assertIn("structural-metadata", required)
                    self.assertIn("package-metadata-render", required)
                    self.assertNotIn("package-build", required)
                    self.assertNotIn("package-install", required)


class ContainerDispatchTests(unittest.TestCase):
    def test_the_container_run_forwards_host_uid_gid_for_out_chown(self) -> None:
        argv = container_run_arguments(
            runtime="docker",
            image="debian:bookworm-slim@sha256:deadbeef",
            root=Path("/repo"),
            output=Path("/tmp/out"),
            release_context=Path("/tmp/release-context.json"),
            backend="deb",
            profile="nightly",
            stage="full",
            host_uid=1001,
            host_gid=1002,
            seal="abc",
            ci_identity={"GITHUB_RUN_ID": "42"},
        )
        joined = " ".join(argv)
        self.assertIn("HOST_UID=1001", joined)
        self.assertIn("HOST_GID=1002", joined)
        self.assertIn("GITHUB_RUN_ID=42", joined)
        self.assertIn("/src/scripts/packaging/linux-container-entry.sh", joined)

    def test_the_systemd_container_create_requests_pid1_prerequisites(self) -> None:
        from engineering.tools.run_linux_package_backend import container_create_arguments

        argv = container_create_arguments(
            runtime="docker",
            image="debian:12@sha256:deadbeef",
            name="glifistore-deb-test",
            root=Path("/repo"),
            output=Path("/tmp/out"),
            release_context=Path("/tmp/release-context.json"),
            host_uid=1001,
            host_gid=1002,
        )
        joined = " ".join(argv)
        self.assertIn("--privileged", joined)
        self.assertIn("--cgroupns=host", joined)
        self.assertIn("/sys/fs/cgroup:/sys/fs/cgroup:rw", joined)
        self.assertIn("/tmp:rw,exec,nosuid,nodev", joined)
        self.assertIn("linux-systemd-pid1.sh", joined)
        self.assertIn("-d", argv)
        self.assertNotIn("--rm", argv)

        private = container_create_arguments(
            runtime="docker",
            image="debian:12@sha256:deadbeef",
            name="glifistore-deb-test",
            root=Path("/repo"),
            output=Path("/tmp/out"),
            release_context=Path("/tmp/release-context.json"),
            variant="cgroupns-private-parent",
        )
        private_joined = " ".join(private)
        self.assertIn("--cgroupns=private", private_joined)
        self.assertIn("--cgroup-parent=docker.slice", private_joined)

    def test_the_container_entry_restores_host_ownership_before_exit(self) -> None:
        # The outer runner is not root: root-owned evidence under /out becomes a
        # Permission denied that was previously reported as packaging FAIL.
        text = (Path(__file__).resolve().parents[2] / "scripts/packaging/linux-container-entry.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("HOST_UID", text)
        self.assertIn("HOST_GID", text)
        self.assertIn("chown -R", text)
        self.assertIn("trap restore_out_ownership EXIT", text)
        self.assertNotIn("exec python3", text)

    def test_inner_fail_evidence_is_preferred_over_outer_blocked(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="glifistore-inner-evidence-")
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        evidence = root / "deb-nightly-full-package-evidence.json"
        evidence.write_text(
            encode_json(
                {
                    "result": "FAIL",
                    "backend": "deb",
                    "profile": "nightly",
                    "stage": "full",
                }
            ),
            encoding="utf-8",
        )
        preferred = prefer_inner_container_evidence(root, "deb", "nightly", "full")
        self.assertIsNotNone(preferred)
        assert preferred is not None
        self.assertEqual(preferred[0], "FAIL")
        self.assertEqual(preferred[1], evidence)
        self.assertIsNone(prefer_inner_container_evidence(root, "rpm", "nightly", "full"))

    def test_debian_rules_do_not_mix_ninja_configure_with_make_build(self) -> None:
        rules = (
            Path(__file__).resolve().parents[2] / "packaging/debian/templates/rules.in"
        ).read_text(encoding="utf-8")
        configure = rules.split("override_dh_auto_configure:", 1)[1].split(
            "override_dh_auto_test:", 1
        )[0]
        self.assertNotIn("-GNinja", configure)
        self.assertIn("-DBUILD_SHARED_LIBS=OFF", configure)

    def test_rpm_install_does_not_inherit_container_nodocs(self) -> None:
        from engineering.tools.run_linux_package_backend import SPECS

        command = " ".join(SPECS["rpm"].install_command)
        self.assertIn("dnf", command)
        self.assertIn("--setopt=tsflags=", command)

    def test_rpm_spec_forces_static_core_linkage(self) -> None:
        spec = (
            Path(__file__).resolve().parents[2] / "packaging/rpm/templates/glifistore.spec.in"
        ).read_text(encoding="utf-8")
        self.assertIn("-DBUILD_SHARED_LIBS=OFF", spec)

    def test_rpm_spec_does_not_expand_cmake_inside_a_comment(self) -> None:
        # A bare %cmake in a # comment inside %build is still macro-expanded by rpm,
        # and an expanded comment with unmatched quotes aborts the shell script after
        # a successful link (nightly Fedora 41 retained that failure; live rpm target is Fedora 43).
        spec = (
            Path(__file__).resolve().parents[2] / "packaging/rpm/templates/glifistore.spec.in"
        ).read_text(encoding="utf-8")
        build = spec.split("%build", 1)[1].split("%install", 1)[0]
        for line in build.splitlines():
            stripped = line.lstrip()
            if stripped.startswith("#"):
                self.assertNotRegex(stripped, r"(?<!%)%cmake\b")

    def test_protocol_backup_does_not_precreate_the_destination(self) -> None:
        # Store backup opens the destination with create_new; a pre-existing empty
        # directory becomes sequence_conflict and the wire maps it to INTERNAL_ERROR.
        text = (
            Path(__file__).resolve().parents[2]
            / "engineering/tools/run_linux_package_backend.py"
        ).read_text(encoding="utf-8")
        protocol = text.split("def run_protocol(", 1)[1].split("def run_restart_recovery(", 1)[0]
        self.assertIn('daemon.client("--command", "backup"', protocol)
        self.assertNotIn('"install"', protocol)
        self.assertNotIn("install -d", protocol)

    def test_service_lifecycle_always_writes_a_non_empty_log_before_recording(self) -> None:
        text = (
            Path(__file__).resolve().parents[2]
            / "engineering/tools/run_linux_package_backend.py"
        ).read_text(encoding="utf-8")
        body = text.split("def run_service_lifecycle(", 1)[1].split(
            "def run_protocol(", 1
        )[0]
        note_at = body.index("_note(log,")
        record_at = body.index('recorder.record(\n            "service-lifecycle"')
        self.assertLess(note_at, record_at)


if __name__ == "__main__":
    unittest.main()
