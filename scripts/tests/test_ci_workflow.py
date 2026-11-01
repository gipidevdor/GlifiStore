from __future__ import annotations

import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class CiWorkflowTests(unittest.TestCase):
    def test_manpage_job_installs_the_authoritative_runtime_component(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertIn("-DBUILD_TESTING=OFF", workflow)
        self.assertIn("cmake --build build/man-ci\n", workflow)
        self.assertNotIn(
            "cmake --build build/man-ci --target glifistore_manpages",
            workflow,
        )
        self.assertIn(
            'cmake --install build/man-ci --prefix "${dest}" --component Runtime',
            workflow,
        )
        self.assertNotIn("--component ManPages", workflow)

    def test_installed_cpp_smoke_cannot_resolve_a_build_tree_package(self) -> None:
        builder = (ROOT / "scripts/build-installed-cpp-interop.sh").read_text(
            encoding="utf-8"
        )
        installed = (
            ROOT / "scripts/test-secure-profile-installed-artifacts.sh"
        ).read_text(encoding="utf-8")

        self.assertIn('PATHS "\\${GLIFISTORE_PACKAGE_DIR}"', builder)
        self.assertIn("NO_DEFAULT_PATH", builder)
        self.assertIn('-DGLIFISTORE_PACKAGE_DIR="$package_dir"', builder)
        self.assertNotIn('-DGlifiStore_DIR="$package_dir"', builder)
        self.assertIn(
            '--target glifistore_client glifistore_abi',
            builder,
        )
        self.assertIn('--component AbiRuntime', builder)
        self.assertIn('--component Development', builder)
        self.assertIn(
            'cpp_artifact="$(cd "$(dirname "$daemon")" && pwd -P)"',
            installed,
        )
        self.assertNotIn('$(dirname "$daemon")/..', installed)

    def test_lto_package_propagates_compatible_link_requirements(self) -> None:
        config = (ROOT / "cmake/GlifiStoreConfig.cmake.in").read_text(
            encoding="utf-8"
        )
        optimizations = (ROOT / "cmake/ToolchainOptimizations.cmake").read_text(
            encoding="utf-8"
        )

        self.assertIn('set(GLIFISTORE_BUILT_WITH_LTO', config)
        self.assertIn("check_ipo_supported", config)
        self.assertIn(
            'CMAKE_CXX_COMPILER_VERSION}" VERSION_EQUAL',
            config,
        )
        self.assertIn("INTERFACE_LINK_OPTIONS", config)
        self.assertIn("GLIFISTORE_IPO_LINK_OPTIONS", optimizations)

    def test_supply_chain_retains_primary_archives_for_cross_builder_diagnostics(self) -> None:
        workflow = (ROOT / ".github/workflows/supply-chain.yml").read_text(encoding="utf-8")
        upload = workflow[workflow.index("Retain SDK artifacts, checksums") :]
        for pattern in ("*.whl", "*.gem", "*.tar.gz", "*package-info.txt"):
            self.assertIn(f"dist/sdk-artifacts/{pattern}", upload)

    def test_sanitizers_cancel_only_obsolete_runs_for_the_same_branch(self) -> None:
        workflow = (ROOT / ".github/workflows/sanitizers.yml").read_text(encoding="utf-8")
        self.assertIn("group: sanitizers-${{ github.head_ref || github.ref_name }}", workflow)
        self.assertIn("cancel-in-progress: true", workflow)

    def test_clang_tidy_fails_closed_on_high_signal_diagnostics(self) -> None:
        workflow = (ROOT / ".github/workflows/static-analysis.yml").read_text(encoding="utf-8")
        gate = (ROOT / "engineering/tools/run_clang_tidy_gate.py").read_text(encoding="utf-8")
        for check in (
            "bugprone-unchecked-optional-access",
            "bugprone-use-after-move",
            "clang-analyzer-deadcode.DeadStores",
            "readability-inconsistent-declaration-parameter-name",
        ):
            self.assertIn(check, gate)
        self.assertIn(
            "python engineering/tools/run_clang_tidy_gate.py --build-dir build/unix-clang-tidy",
            workflow,
        )

    def test_formal_models_are_required_and_supply_chain_pinned(self) -> None:
        workflow_path = ROOT / ".github/workflows/formal-models.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        self.assertFalse((ROOT / ".github/workflows/formal-shard-pair.yml").exists())
        for model in ("shard_pair", "persistence"):
            self.assertIn(f"model: {model}", workflow)
        self.assertIn("(bounded, required)", workflow)
        self.assertIn('timeout 480 "./engineering/formal/${{ matrix.model }}/run-tlc.sh"', workflow)
        self.assertIn(
            "011267fca25d0752cf18894dd585cd28b96960c112ea8a20b823ea770e7e10cd",
            workflow,
        )
        self.assertIn("releases/assets/692577622", workflow)
        self.assertIn("Accept: application/octet-stream", workflow)
        self.assertIn("sha256sum -c -", workflow)
        self.assertNotIn("best-effort", workflow)
        self.assertNotIn("set +e", workflow)

        shard_config = (ROOT / "engineering/formal/shard_pair/ShardPair.cfg").read_text(
            encoding="utf-8"
        )
        persistence_config = (
            ROOT / "engineering/formal/persistence/PersistenceRecovery.cfg"
        ).read_text(encoding="utf-8")
        self.assertIn("PROPERTIES\n  Liveness", shard_config)
        for invariant in (
            "WriteOrder",
            "NormalAuthorityComplete",
            "RecoveryOracle",
            "CommittedCorruptionFailsClosed",
        ):
            self.assertIn(invariant, persistence_config)
        self.assertIn("PROPERTIES\n  RecoveryTerminates", persistence_config)
        self.assertIn("CommittedCorruptionIsRejected", persistence_config)

    def test_random_crash_campaign_is_bounded_reproducible_e2_evidence(self) -> None:
        workflow = (ROOT / ".github/workflows/durability-evidence.yml").read_text(
            encoding="utf-8"
        )
        collector = (ROOT / "scripts/collect-durability-evidence.sh").read_text(
            encoding="utf-8"
        )
        harness = (ROOT / "tests/crash/crash_persistence.cpp").read_text(encoding="utf-8")

        self.assertIn("e2-random-crash-campaign:", workflow)
        self.assertIn("iterations=256", workflow)
        self.assertIn('"$iterations" -le 512', workflow)
        self.assertIn('"$seed" =~ ^[0-9]+$', workflow)
        self.assertIn("int(sys.argv[1]) > (1 << 64) - 1", workflow)
        self.assertIn("--run random-campaign", workflow)
        self.assertIn("--campaign-seed", workflow)
        self.assertIn("--iterations", workflow)
        self.assertIn('Physical power loss exercised: `no`', workflow)
        self.assertIn("random-campaign-${iteration}.tsv", collector)
        self.assertIn("manifest.sha256", collector)
        self.assertIn("kMaxRandomCampaignIterations{10'000}", harness)
        self.assertIn("glifistore-random-crash-campaign-v1", harness)
        self.assertIn("random campaign report already exists", harness)
        self.assertIn("CampaignPrng", harness)

    def test_e3_worker_pause_and_dm_fault_matrix_are_fail_closed(self) -> None:
        workflow = (ROOT / ".github/workflows/durability-evidence.yml").read_text(
            encoding="utf-8"
        )
        script = (ROOT / "scripts/run-e3-block-reset.sh").read_text(
            encoding="utf-8"
        )
        harness = (ROOT / "tests/crash/crash_persistence.cpp").read_text(
            encoding="utf-8"
        )
        checkpoint = (ROOT / "tests/crash/crash_checkpoint.hpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("--checkpoint-action pause", script)
        self.assertIn('worker_stop_confirmed="yes"', script)
        self.assertIn('if perform_reset >>"$case_log" 2>&1; then', script)
        self.assertIn('reset_confirmed_global="yes"', script)
        self.assertNotIn('reset_confirmed="$(perform_reset', script)
        self.assertIn('work_root="$work_parent/glifistore-e3-work-$$"', script)
        self.assertIn('[[ "$candidate" != "$mount_point"/store-?* ]]', script)
        self.assertIn('rm -rf -- "$candidate"', script)
        self.assertGreaterEqual(
            script.count('cleanup_case_or_abort "$data_dir" "$case_log"'), 7
        )
        self.assertIn('dmsetup suspend --noflush "$mapper_name"', script)
        self.assertIn(
            'dmsetup create "$mapper_name" --table "0 $sectors linear $loop_device 0"',
            script,
        )
        self.assertNotIn("flakey $loop_device 0 0 0", script)
        flakey_reset = script.split("arm_reset_linux_flakey() {", 1)[1].split(
            "\n}", 1
        )[0]
        self.assertLess(
            flakey_reset.index('umount -l "$mount_point"'),
            flakey_reset.index('dmsetup remove --force "$mapper_name"'),
        )
        self.assertIn("terminate_worker_hard", flakey_reset)
        self.assertIn('-path "$work_root" -prune', script)
        self.assertIn("CheckpointAction::pause", harness)
        self.assertIn("::raise(SIGSTOP)", checkpoint)
        for mode in ("drop-writes", "error-writes", "all-io-error"):
            self.assertIn(mode, script)
            self.assertIn(mode, workflow)
        self.assertIn("fault_modes=drop-writes error-writes all-io-error", workflow)
        self.assertIn('"$REQUESTED_REPEAT" -le 100', workflow)
        self.assertNotIn('echo "repeat=${{ inputs.e3_repeat }}', workflow)
        self.assertIn('"$repeat" -le 1000', script)
        self.assertIn("e3_certified=no", script)

    def test_diagnostic_coverage_runs_only_the_built_test_target(self) -> None:
        script = (ROOT / "scripts/ci-coverage.sh").read_text(encoding="utf-8")
        workflow = (ROOT / ".github/workflows/coverage.yml").read_text(encoding="utf-8")
        self.assertIn('cmake --build "$builddir" --target glifistore_tests', script)
        self.assertIn("--tests-regex '^glifistore_tests$'", script)
        self.assertIn("-fprofile-instr-generate -fcoverage-mapping", script)
        self.assertIn('LLVM_PROFILE_FILE="$outdir/%p.profraw"', script)
        self.assertIn('"$coverage_llvm_profdata" merge -sparse', script)
        self.assertIn('"$coverage_llvm_cov" export --format=lcov', script)
        self.assertIn("--ignore-errors mismatch,inconsistent,unused", script)
        self.assertIn("--ignore-errors inconsistent,unused", script)
        self.assertIn('| tee "$outdir/coverage-report.txt"', script)
        self.assertNotIn("lcov --list", script)
        self.assertNotIn("--ignore-errors mismatch,gcov", script)
        self.assertIn('command -v "$tool-$clang_major"', script)
        self.assertNotIn("--ignore-errors format", script)
        self.assertIn('-s "$outdir/coverage-report.txt"', script)
        self.assertIn("grep -q '^SF:'", script)
        self.assertIn("grep -q '^DA:'", script)
        self.assertNotIn("Always succeed", script)
        self.assertIn("if-no-files-found: error", workflow)

        wrapper = (ROOT / "scripts/llvm-gcov.sh").read_text(encoding="utf-8")
        self.assertIn('exec "${LLVM_COV:-llvm-cov}" gcov "$@"', wrapper)

    def test_clang_pgo_keeps_partial_profile_diagnostics_non_fatal(self) -> None:
        optimizations = (ROOT / "cmake/ToolchainOptimizations.cmake").read_text(
            encoding="utf-8"
        )
        use_mode = optimizations.split(
            'if(GLIFISTORE_PGO STREQUAL "USE")', 1
        )[1].split('message(FATAL_ERROR "GLIFISTORE_PGO must', 1)[0]
        self.assertIn("-Wno-error=profile-instr-unprofiled", use_mode)

    def test_e3_artifact_validator_rejects_unconfirmed_pass(self) -> None:
        validator = ROOT / "scripts/assert-e3-rehearsal-honesty.sh"
        header = (
            "iteration\tscenario\tboundary\tcheckpoint_action\tworker_stop_confirmed\t"
            "reset_mechanism\tdm_fault_mode\treset_confirmed\tfsck_status\trecovery\toutcome\n"
        )
        with tempfile.TemporaryDirectory() as temporary:
            artifact = Path(temporary)
            (artifact / "provenance.txt").write_text(
                "\n".join(
                    (
                        "schema=glifistore-durability-e3-harness-v2",
                        "checkpoint_action=pause",
                        "dm_fault_mode=drop-writes",
                        "passed=1",
                        "failed=0",
                        "inconclusive=0",
                        "reset_confirmed_any=yes",
                        "harness_result=passed",
                        "e3_certified=no",
                        "e4_certified=no",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            (artifact / "summary.md").write_text(
                "E3 certified: `no`\nE4 certified: `no`\n"
                "Release-certification eligible: `no`\n",
                encoding="utf-8",
            )
            results = artifact / "results.tsv"
            results.write_text(
                header
                + "1\tput\twrite_record\tpause\tyes\tdm-flakey\tdrop-writes\tyes\t"
                "clean-or-ok\tpassed\tPASS\n",
                encoding="utf-8",
            )
            manifest_lines = []
            for filename in ("provenance.txt", "results.tsv", "summary.md"):
                digest = hashlib.sha256((artifact / filename).read_bytes()).hexdigest()
                manifest_lines.append(f"{digest}  {filename}")
            (artifact / "manifest.sha256").write_text(
                "\n".join(manifest_lines) + "\n", encoding="utf-8"
            )
            unreadable_work = artifact / "work" / "mnt" / "lost+found"
            unreadable_work.mkdir(parents=True)
            unreadable_work.chmod(0)
            try:
                accepted = subprocess.run(
                    ["bash", str(validator), "--dir", str(artifact), "--kind", "harness"],
                    check=False,
                    capture_output=True,
                    text=True,
                )
            finally:
                unreadable_work.chmod(0o700)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)

            results.write_text(
                header
                + "1\tput\twrite_record\tpause\tno\tdm-flakey\tdrop-writes\tyes\t"
                "clean-or-ok\tpassed\tPASS\n",
                encoding="utf-8",
            )
            rejected = subprocess.run(
                ["bash", str(validator), "--dir", str(artifact), "--kind", "harness"],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("PASS lacks stop/reset/recovery confirmation", rejected.stderr)

            (artifact / "manifest.sha256").write_text(
                f"{'0' * 64}  ../outside\n", encoding="utf-8"
            )
            unsafe = subprocess.run(
                ["bash", str(validator), "--dir", str(artifact), "--kind", "harness"],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(unsafe.returncode, 0)
            self.assertIn("unsafe or malformed SHA-256 manifest entry", unsafe.stderr)


if __name__ == "__main__":
    unittest.main()
