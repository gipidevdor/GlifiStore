from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ArtifactReleaseWorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.candidate = (ROOT / ".github/workflows/release-candidate.yml").read_text(encoding="utf-8")
        self.release = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        self.codeql = (ROOT / ".github/workflows/codeql.yml").read_text(encoding="utf-8")

    def test_candidate_is_a_least_privilege_reusable_build_once_workflow(self) -> None:
        self.assertIn("workflow_call:", self.candidate)
        self.assertRegex(self.candidate, re.compile(r"permissions:\n\s+contents: read"))
        self.assertRegex(self.candidate, re.compile(r"env:\n\s+CC: clang\n\s+CXX: clang\+\+\n\s+LD: ld"))
        self.assertEqual(self.candidate.count("cmake -S . -B build/release-candidate"), 1)
        self.assertEqual(self.candidate.count("cmake --install build/release-candidate"), 1)
        self.assertIn("package-source-release.py", self.candidate)
        self.assertIn("package-install-prefix.py", self.candidate)
        self.assertIn("package-abi-consumer.py", self.candidate)
        self.assertIn("package-wire-client.py", self.candidate)
        self.assertIn("candidate-seal-sha256", self.candidate)
        self.assertIn("steps.seal.outputs.candidate-seal-sha256", self.candidate)
        self.assertIn("release_bundle.py bind-sbom", self.candidate)
        self.assertIn("--cmake-cache build/release-candidate/CMakeCache.txt", self.candidate)
        self.assertLess(
            self.candidate.index("Record release toolchain authority"),
            self.candidate.index("Generate, bind, and validate SPDX SBOMs"),
        )
        self.assertLess(
            self.candidate.index("--seal candidate-seal.json"),
            self.candidate.index("Upload immutable candidate"),
        )

    def test_release_is_tag_only_candidate_verify_publish(self) -> None:
        self.assertIn('tags:\n      - "v*"', self.release)
        self.assertNotIn("branches:", self.release)
        self.assertNotIn("workflow_dispatch:", self.release)
        candidate = self.release.index("  candidate:")
        sdk_evidence = self.release.index("  sdk-installed-evidence:")
        persistence_evidence = self.release.index("  persistent-compatibility-evidence:")
        abi_evidence = self.release.index("  abi-compatibility-evidence:")
        wire_evidence = self.release.index("  wire-compatibility-evidence:")
        freebsd_evidence = self.release.index("  freebsd-package-evidence:")
        openbsd_evidence = self.release.index("  openbsd-package-evidence:")
        security_sanitizers = self.release.index("  security-sanitizers:")
        security_codeql = self.release.index("  security-codeql:")
        security_static = self.release.index("  security-static:")
        security_supply_chain = self.release.index("  security-supply-chain:")
        security_evidence = self.release.index("  security-matrix-evidence:")
        reproducibility_evidence = self.release.index("  reproducibility-evidence:")
        verify = self.release.index("  verify:")
        publish = self.release.index("  publish:")
        self.assertLess(candidate, sdk_evidence)
        self.assertLess(sdk_evidence, persistence_evidence)
        self.assertLess(persistence_evidence, abi_evidence)
        self.assertLess(abi_evidence, wire_evidence)
        self.assertLess(wire_evidence, freebsd_evidence)
        self.assertLess(freebsd_evidence, openbsd_evidence)
        self.assertLess(openbsd_evidence, security_sanitizers)
        self.assertLess(security_sanitizers, security_codeql)
        self.assertLess(security_codeql, security_static)
        self.assertLess(security_static, security_supply_chain)
        self.assertLess(security_supply_chain, security_evidence)
        self.assertLess(security_evidence, reproducibility_evidence)
        self.assertLess(reproducibility_evidence, verify)
        self.assertLess(verify, publish)
        self.assertIn(
            "needs: [candidate, sdk-installed-evidence, persistent-compatibility-evidence, abi-compatibility-evidence, wire-compatibility-evidence, freebsd-package-evidence, openbsd-package-evidence, security-matrix-evidence, reproducibility-evidence]",
            self.release[verify:publish],
        )
        self.assertIn("needs: verify", self.release[publish:])
        self.assertIn("environment: release", self.release[publish:])

    def test_installed_sdk_evidence_uses_candidate_bytes_and_same_run_artifact(self) -> None:
        start = self.release.index("  sdk-installed-evidence:")
        end = self.release.index("  persistent-compatibility-evidence:")
        job = self.release[start:end]
        self.assertIn("needs: candidate", job)
        self.assertIn("needs.candidate.outputs.artifact-name", job)
        self.assertLess(job.index("verify-seal"), job.index("tar -xJf"))
        self.assertIn("GLIFISTORE_CPP_PREFIX", job)
        self.assertIn("INSTALLED_INTEROP_PROFILE=plain", job)
        self.assertIn("release_evidence.py create", job)
        self.assertIn("--type sdk_installed_interop", job)
        self.assertIn("release-input-${{ github.sha }}-sdk-installed", job)
        for check_id in ("source-isolation", "c", "cpp", "python", "perl", "go", "erlang", "ruby"):
            self.assertIn(f'"id":"{check_id}"', job)
        self.assertNotIn("cmake -S . ", job)
        self.assertNotIn("cmake --install", job)

    def test_persistence_evidence_uses_prior_tagged_fixture_and_installed_candidate(self) -> None:
        start = self.release.index("  persistent-compatibility-evidence:")
        end = self.release.index("  abi-compatibility-evidence:")
        job = self.release[start:end]
        self.assertIn("needs: candidate", job)
        self.assertIn("needs.candidate.outputs.artifact-name", job)
        self.assertLess(job.index("verify-seal"), job.index("tar -xJf"))
        self.assertIn("persistence_fixture.py select", job)
        self.assertIn("--before-version", job)
        self.assertIn("build-installed-cpp-interop.sh", job)
        self.assertIn("test-installed-persistence-compat.sh", job)
        self.assertIn("release_evidence.py create", job)
        self.assertIn("--type persistent_compatibility", job)
        self.assertIn("release-input-${{ github.sha }}-persistent-compatibility", job)
        for check_id in (
            "new-opens-old",
            "fixture-recovery",
            "migration",
            "backup-verify-repair",
        ):
            self.assertIn(f'"id":"{check_id}"', job)
        self.assertNotIn("cmake -S . ", job)
        self.assertNotIn("cmake --install", job)

    def test_abi_evidence_uses_attested_prior_release_and_retained_binary(self) -> None:
        start = self.release.index("  abi-compatibility-evidence:")
        end = self.release.index("  wire-compatibility-evidence:")
        job = self.release[start:end]
        self.assertIn("needs: candidate", job)
        self.assertIn("prior_release.py select", job)
        self.assertIn("gh release download", job)
        self.assertIn("prior_release.py validate", job)
        self.assertIn("gh attestation verify", job)
        self.assertIn("abi_consumer_fixture.py", job)
        self.assertIn("test-installed-abi-compat.sh", job)
        self.assertIn("--type abi_compatibility", job)
        self.assertIn("release-input-${{ github.sha }}-abi-compatibility", job)
        for check_id in (
            "exact-symbols",
            "layout",
            "old-consumer-new-library",
            "new-consumer-old-library",
        ):
            self.assertIn(f'"id":"{check_id}"', job)
        self.assertNotIn("cmake -S . ", job)
        self.assertNotIn("cmake --install", job)

    def test_verify_and_publish_consume_sealed_bytes_without_engine_rebuild(self) -> None:
        verify = self.release[self.release.index("  verify:"):self.release.index("  publish:")]
        publish = self.release[self.release.index("  publish:"):]
        self.assertLess(verify.index("admit-candidate"), verify.index("Test installed binary archive"))
        self.assertIn("--candidate-seal candidate-seal.json", verify)
        self.assertIn("pattern: release-input-${{ github.sha }}-*", verify)
        self.assertIn("release_bundle.py import-evidence", verify)
        self.assertIn("release_bundle.py validate-evidence-import", verify)
        self.assertLess(verify.index("admit-candidate"), verify.index("import-evidence"))
        self.assertLess(verify.index("import-evidence"), verify.index("release_bundle.py verification"))
        self.assertIn("validate-release-policy", verify)
        self.assertIn("release_evidence.py --require-ci", verify)
        self.assertIn("validate_bsd_packaging.py --release", verify)
        self.assertNotIn("cmake -S . ", verify)
        self.assertNotIn("cmake --install", verify)
        for forbidden in ("cmake -S", "cmake --build", "cmake --install", "ninja ", "make "):
            self.assertNotIn(forbidden, publish)
        self.assertLess(publish.index("verify-seal"), publish.index("action-gh-release"))

    def test_wire_evidence_uses_attested_prior_release_and_retained_clients(self) -> None:
        start = self.release.index("  wire-compatibility-evidence:")
        end = self.release.index("  freebsd-package-evidence:")
        job = self.release[start:end]
        self.assertIn("needs: candidate", job)
        self.assertIn("prior_release.py select", job)
        self.assertIn("gh release download", job)
        self.assertIn("prior_release.py validate", job)
        self.assertIn("gh attestation verify", job)
        self.assertIn("wire_client_fixture.py", job)
        self.assertIn("test-installed-wire-compat.sh", job)
        self.assertIn("--type wire_compatibility", job)
        self.assertIn("release-input-${{ github.sha }}-wire-compatibility", job)
        for check_id in (
            "new-client-new-server",
            "old-client-new-server",
            "new-client-old-server",
        ):
            self.assertIn(f'"id":"{check_id}"', job)
        self.assertNotIn("cmake -S . ", job)
        self.assertNotIn("cmake --install", job)

    def test_freebsd_package_evidence_runs_only_on_native_package_manager(self) -> None:
        start = self.release.index("  freebsd-package-evidence:")
        end = self.release.index("  openbsd-package-evidence:")
        job = self.release[start:end]
        script = (ROOT / "scripts/test-freebsd-package-lifecycle.sh").read_text(encoding="utf-8")

        self.assertIn("needs: candidate", job)
        self.assertIn("validate_bsd_packaging.py --release", job)
        self.assertIn("vmactions/freebsd-vm@", job)
        self.assertIn("test-freebsd-package-lifecycle.sh", job)
        self.assertIn("needs.candidate.outputs.candidate-seal-sha256", job)
        self.assertIn("release_bundle.py bind-sbom", job)
        self.assertIn("freebsd-package-evidence.json", job)
        self.assertIn("release-input-${{ github.sha }}-freebsd-package", job)

        self.assertIn('[[ "$(uname -s)" == "FreeBSD" ]]', script)
        self.assertIn("$ports_root/UIDs", script)
        self.assertIn("$ports_root/GIDs", script)
        self.assertIn('DISTDIR="$work/distfiles" makesum', script)
        self.assertIn("make -C \"$work/port\" DISTDIR=\"$work/distfiles\" check-plist", script)
        self.assertIn("pkg add -y", script)
        self.assertIn("package-external-consumer.sh", script)
        self.assertIn("service glifistored start", script)
        self.assertIn("service glifistored stop", script)
        self.assertIn("glifistore_verify_store -- /var/db/glifistore", script)
        self.assertIn("pkg delete -y glifistore", script)
        for check_id in (
            "package-build",
            "package-install",
            "file-inventory",
            "external-consumer",
            "service-start",
            "put-get-erase",
            "graceful-shutdown",
            "restart-recovery",
            "uninstall",
            "config-preservation",
        ):
            self.assertIn(f'"id":"{check_id}"', script)

    def test_openbsd_package_evidence_runs_only_on_native_package_manager(self) -> None:
        start = self.release.index("  openbsd-package-evidence:")
        end = self.release.index("  security-sanitizers:")
        job = self.release[start:end]
        script = (ROOT / "scripts/test-openbsd-package-lifecycle.sh").read_text(encoding="utf-8")

        self.assertIn("needs: candidate", job)
        self.assertIn("validate_bsd_packaging.py --release", job)
        self.assertIn("vmactions/openbsd-vm@", job)
        self.assertIn("test-openbsd-package-lifecycle.sh", job)
        self.assertIn("needs.candidate.outputs.candidate-seal-sha256", job)
        self.assertIn("release_bundle.py bind-sbom", job)
        self.assertIn("openbsd-package-evidence.json", job)
        self.assertIn("release-input-${{ github.sha }}-openbsd-package", job)
        self.assertIn('release: "7.9"', job)

        self.assertIn('[[ "$(uname -s)" == "OpenBSD" ]]', script)
        self.assertIn("PORTS_ACCOUNT_REGISTERED", script)
        self.assertIn("$ports_root/databases/glifistore", script)
        self.assertIn("PACKAGE_REPOSITORY=", script)
        self.assertIn("pkg_add", script)
        self.assertIn("package-external-consumer.sh", script)
        self.assertIn("rcctl enable glifistored", script)
        self.assertIn("rcctl start glifistored", script)
        self.assertIn("rcctl stop glifistored", script)
        self.assertIn("glifistore_verify_store -- /var/glifistore", script)
        self.assertIn("pkg_delete glifistore", script)
        self.assertIn("libglifistore.so.${abi_version}", script)
        for check_id in (
            "package-build",
            "package-install",
            "file-inventory",
            "external-consumer",
            "service-start",
            "put-get-erase",
            "graceful-shutdown",
            "restart-recovery",
            "uninstall",
            "config-preservation",
        ):
            self.assertIn(f'"id":"{check_id}"', script)

    def test_security_matrix_evidence_requires_complete_same_run_matrix(self) -> None:
        start = self.release.index("  security-sanitizers:")
        end = self.release.index("  reproducibility-evidence:")
        jobs = self.release[start:end]
        evidence_start = jobs.index("  security-matrix-evidence:")
        evidence = jobs[evidence_start:]

        self.assertIn("preset: unix-asan", jobs)
        self.assertIn("checks: asan ubsan", jobs)
        self.assertIn("preset: unix-tsan", jobs)
        self.assertIn('ctest --preset "$SECURITY_PRESET" --output-on-failure', jobs)
        for language in ("c-cpp", "python", "go", "actions"):
            self.assertIn(f"language: {language}", jobs)
        self.assertIn("github/codeql-action/analyze@", jobs)
        self.assertIn("upload: never", jobs)
        self.assertNotIn("upload: false", jobs)
        self.assertIn("cmake --preset unix-clang-tidy", jobs)
        self.assertIn("cmake --preset unix-strict", jobs)
        self.assertIn("gitleaks/gitleaks-action@", jobs)
        self.assertIn("aquasecurity/trivy-action@", jobs)
        self.assertIn("./scripts/ci-license-check.sh", jobs)
        license_check = (ROOT / "scripts/ci-license-check.sh").read_text(encoding="utf-8")
        self.assertIn("stdlib-only module; no third-party requires", license_check)
        self.assertIn("ERROR: go-licenses report failed", license_check)
        self.assertNotIn("go-licenses.err", license_check)
        self.assertNotIn('|| true', license_check.split("== Go licenses")[1].split("== Python")[0])

        self.assertIn(
            "needs: [candidate, security-sanitizers, security-codeql, security-static, security-supply-chain]",
            evidence,
        )
        self.assertIn("needs.candidate.outputs.candidate-seal-sha256", evidence)
        self.assertIn("release_bundle.py verify-seal", evidence)
        self.assertIn("release_bundle.py validate-sbom", evidence)
        self.assertIn("verify_hardening.py", evidence)
        self.assertIn("--binary build/security-candidate-prefix/bin/glifistored", evidence)
        self.assertIn("--type security_matrix", evidence)
        self.assertIn("release-input-${{ github.sha }}-security-matrix", evidence)
        for check_id in (
            "asan",
            "ubsan",
            "tsan",
            "codeql",
            "static-analysis",
            "warnings-as-errors",
            "dependency-scan",
            "secret-scan",
            "license-validation",
            "sbom-validation",
            "binary-hardening",
        ):
            self.assertIn(f'"id":"{check_id}"', evidence)

    def test_codeql_sarif_upload_policy_uses_supported_v4_value(self) -> None:
        self.assertNotIn("upload: false", self.codeql)
        self.assertEqual(self.codeql.count("upload: never"), 4)

    def test_reproducibility_evidence_rebuilds_without_replacing_candidate(self) -> None:
        start = self.release.index("  reproducibility-evidence:")
        end = self.release.index("  verify:")
        job = self.release[start:end]
        self.assertIn("needs: candidate", job)
        self.assertIn("verify-seal", job)
        self.assertIn("cmake -S . -B build/release-candidate", job)
        self.assertIn("package-source-release.py", job)
        self.assertIn("package-install-prefix.py", job)
        self.assertIn("package-abi-consumer.py", job)
        self.assertIn("package-wire-client.py", job)
        self.assertIn("compare_release_rebuild.py", job)
        self.assertIn("build/repro-build-metadata.json", job)
        self.assertIn("--reference-metadata dist/release-candidate/build-metadata.json", job)
        self.assertIn("--type reproducibility", job)
        self.assertIn("release-input-${{ github.sha }}-reproducibility", job)
        self.assertIn("reproducibility-rebuild-${{ github.sha }}", job)
        for check_id in ("independent-rebuild", "artifact-compare"):
            self.assertIn(f'"id":"{check_id}"', job)
        self.assertNotIn("dist/release-candidate/verified-seal.json", job)

    def test_attestation_and_immutability_are_enforced(self) -> None:
        # Every consumer of the candidate carries the external seal anchor, including
        # the optional package backends and the Wave F package-admission job.
        self.assertEqual(
            self.release.count("needs.candidate.outputs.candidate-seal-sha256"), 14
        )
        self.assertIn("subject-path: dist/release-candidate/verified-seal.json", self.release)
        self.assertIn("--bundle dist/release-provenance/verified-seal.sigstore.json", self.release)
        self.assertIn("--signer-workflow", self.release)
        self.assertIn("--source-ref", self.release)
        self.assertIn("--source-digest", self.release)
        self.assertIn("release $RELEASE_TAG already exists", self.release)
        self.assertNotIn("continue-on-error", self.release)
        self.assertNotIn("--clobber", self.release)


if __name__ == "__main__":
    unittest.main()
