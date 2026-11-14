from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from scripts.benchmark_report import (
    ENVIRONMENT_IDENTITY_FIELDS,
    add_comparisons,
    add_diagnostic_comparisons,
    build_tcp_scaling_analysis,
    compare_with_baseline,
    comparison_environment_status,
    environment_identity,
    has_durable_pipeline_profile,
    load_source_contract,
    main as benchmark_report_main,
    parse_environment,
    parse_output,
    regressions_over_threshold,
    render_markdown,
    validate_runs,
    validate_source_contract,
)


def environment(**overrides: object) -> dict[str, object]:
    values: dict[str, object] = {
        "runner_os": "Linux",
        "runner_arch": "X64",
        "runner_image": "ubuntu24",
        "runner_image_version": "20260820.1",
        "kernel_release": "6.11.0",
        "cpu_model": "Fixture CPU",
        "logical_cpu_count": 4,
        "physical_cpu_count": 2,
        "compiler_identity": "clang version 20.1.0",
        "cmake_identity": "cmake version 4.1.0",
        "ninja_identity": "1.13.1",
        "build_preset": "unix-release",
        "benchmark_contract_sha256": "a" * 64,
        "benchmark_subject_sha256": "b" * 64,
    }
    values.update(overrides)
    return values


def runs(
    rate: float, minimum: float | None = None, maximum: float | None = None
) -> list[dict[str, object]]:
    return [
        {
            "source": "core.txt",
            "metadata": {},
            "results": [
                {
                    "name": "store_get",
                    "operations": 100,
                    "median_ops_per_second": rate,
                    "min_ops_per_second": minimum
                    if minimum is not None
                    else rate * 0.9,
                    "max_ops_per_second": maximum
                    if maximum is not None
                    else rate * 1.1,
                }
            ],
        }
    ]


def strict_runs() -> list[dict[str, object]]:
    fixture = runs(100.0, 90.0, 110.0)
    fixture[0]["metadata"] = {
        "git_sha": "abc123",
        "arch": "x86_64",
        "platform": "linux",
        "compiler": "clang 20",
        "benchmark_warmup": 1,
        "benchmark_repeats": 3,
    }
    fixture[0]["results"][0].update(
        {
            "samples": 3,
            "warmup": 1,
            "median_seconds": 1.0,
            "min_seconds": 0.9,
            "max_seconds": 1.1,
        }
    )
    return fixture


def tcp_runs() -> list[dict[str, object]]:
    rates = {
        1: {1: 100.0, 8: 200.0, 32: 180.0, 128: 150.0},
        2: {1: 180.0, 8: 360.0, 32: 350.0, 128: 300.0},
        4: {1: 320.0, 8: 640.0, 32: 600.0, 128: 500.0},
    }
    return [
        {
            "source": f"server-tcp-w{workers}-p{pipeline}.txt",
            "metadata": {
                "pipeline": pipeline,
                "execution_scope": "same-process-loopback",
                "timed_thread_model": "client-threads+one-reactor+one-writer-per-worker",
                "timed_foreground_thread_floor": workers * 3,
            },
            "results": [
                {
                    "workers": workers,
                    "operations": 1_000,
                    "median_ops_per_second": rate,
                    "min_ops_per_second": rate * 0.9,
                    "max_ops_per_second": rate * 1.1,
                    "median_reactor_input_buffer_compactions": pipeline,
                    "maximum_reactor_input_buffer_compactions": pipeline + 1,
                    "median_reactor_input_buffer_bytes_moved": workers * pipeline * 100,
                    "maximum_reactor_input_buffer_bytes_moved": workers
                    * pipeline
                    * 125,
                    "median_reactor_output_buffer_compactions": workers,
                    "maximum_reactor_output_buffer_compactions": workers + 1,
                    "median_reactor_output_buffer_bytes_moved": workers * pipeline * 50,
                    "maximum_reactor_output_buffer_bytes_moved": workers
                    * pipeline
                    * 75,
                }
            ],
        }
        for workers, pipelines in rates.items()
        for pipeline, rate in pipelines.items()
    ]


def strict_tcp_run() -> list[dict[str, object]]:
    fixture = strict_runs()
    fixture[0]["source"] = "server-tcp-w2-p32.txt"
    fixture[0]["metadata"].update(
        {
            "pipeline": 32,
            "client_mode": "raw-wire",
            "execution_scope": "same-process-loopback",
            "timed_thread_model": "client-threads+one-reactor+one-writer-per-worker",
            "timed_foreground_thread_floor": 6,
            "storage_mode": "volatile",
            "latency_measurement": "disabled",
        }
    )
    fixture[0]["results"][0].update(
        {
            "workers": 2,
            "threads": 2,
            "distribution": "owner-bound",
            "median_reactor_input_buffer_compactions": 2,
            "maximum_reactor_input_buffer_compactions": 3,
            "median_reactor_input_buffer_bytes_moved": 64,
            "maximum_reactor_input_buffer_bytes_moved": 96,
            "median_reactor_output_buffer_compactions": 1,
            "maximum_reactor_output_buffer_compactions": 2,
            "median_reactor_output_buffer_bytes_moved": 32,
            "maximum_reactor_output_buffer_bytes_moved": 48,
        }
    )
    return fixture


class BenchmarkEnvironmentTests(unittest.TestCase):
    def test_main_rejects_missing_inputs_and_non_object_baselines(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            json_path = root / "report.json"
            markdown_path = root / "report.md"
            missing_argv = [
                "benchmark_report.py",
                str(root / "missing.txt"),
                "--json",
                str(json_path),
                "--markdown",
                str(markdown_path),
            ]
            with (
                patch("sys.argv", missing_argv),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                benchmark_report_main()

            source = root / "source.txt"
            baseline = root / "baseline.json"
            source.write_text("", encoding="utf-8")
            baseline.write_text("[]\n", encoding="utf-8")
            baseline_argv = [
                "benchmark_report.py",
                str(source),
                "--baseline",
                str(baseline),
                "--json",
                str(json_path),
                "--markdown",
                str(markdown_path),
            ]
            with (
                patch("sys.argv", baseline_argv),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit),
            ):
                benchmark_report_main()

    def test_main_emits_schema_ten_with_specialized_match_count(self) -> None:
        content = (
            "# git_sha=abc123\n"
            "# arch=x86_64\n"
            "# platform=linux\n"
            "# compiler=clang 20\n"
            "# benchmark_warmup=0\n"
            "# benchmark_repeats=1\n"
            "scenario,repeat,compact_ms\n"
            "no-gain,1,1.5\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "compaction.txt"
            json_path = root / "report.json"
            markdown_path = root / "report.md"
            source.write_text(content, encoding="utf-8")
            argv = [
                "benchmark_report.py",
                str(source),
                "--strict",
                "--json",
                str(json_path),
                "--markdown",
                str(markdown_path),
            ]
            with patch("sys.argv", argv):
                self.assertEqual(benchmark_report_main(), 0)
            report = json.loads(json_path.read_text(encoding="utf-8"))
            markdown = markdown_path.read_text(encoding="utf-8")
        self.assertEqual(report["schema_version"], 10)
        self.assertEqual(report["matched_baseline_diagnostics"], 0)
        self.assertIn("1 specialized diagnostic(s)", markdown)

    def test_specialized_formats_are_parsed_as_directional_diagnostics(self) -> None:
        common = (
            "# git_sha=abc123\n"
            "# arch=x86_64\n"
            "# platform=linux\n"
            "# compiler=clang 20\n"
            "# benchmark_warmup=1\n"
            "# benchmark_repeats=2\n"
        )
        fixtures = {
            "compaction.txt": (
                "scenario,repeat,compact_ms,seed_s,reopen_ms\n"
                "high-reclaim,1,12,1.0,2\n"
                "high-reclaim,2,10,0.8,1\n",
                "compact_ms",
                False,
            ),
            "maintenance-throughput.txt": (
                "mode,repeat,foreground_ops_s,p99_us,max_us\n"
                "background,1,100,20,30\n"
                "background,2,120,18,28\n",
                "foreground_ops_s",
                True,
            ),
            "maintenance-rotation.txt": (
                "mode,repeat,rotation_ms,publication_wait_ms,seal_ms,create_ms,manifest_publication_ms\n"
                "background,1,20,5,4,3,2\n"
                "background,2,18,4,3,2,1\n",
                "rotation_ms",
                False,
            ),
            "maintenance-idle.txt": (
                "mode,repeat,process_cpu_duty_pct,last_eval_us\n"
                "background,1,0.1,20\n"
                "background,2,0.2,30\n",
                "process_cpu_duty_pct",
                False,
            ),
            "generation-shell.txt": (
                "implementation\trepeat\tops_per_second\tns_per_op\n"
                "direct\t0\t100\t10\n"
                "direct\t1\t120\t8\n",
                "ops_per_second",
                True,
            ),
            "generation-publication.txt": (
                "implementation\trepeat\tpublications_per_second\tns_per_publication\t"
                "sample_p99_ns\treader_get_p99_ns\n"
                "direct\t0\t100\t10\t15\t20\n"
                "direct\t1\t120\t8\t12\t18\n",
                "publications_per_second",
                True,
            ),
            "paired-shard.txt": (
                "kind,implementation,workload,repeat,ops_per_second,p99_get_ns\n"
                "sample,paired,get100,0,100,20\n"
                "sample,paired,get100,1,120,18\n"
                "summary,paired,get100,2,999,1\n",
                "ops_per_second",
                True,
            ),
            "paired-reactor.txt": (
                "kind,implementation,repeat,ops_per_second,p99_batch_us,p999_batch_us\n"
                "sample,paired,0,100,20,30\n"
                "sample,paired,1,120,18,28\n"
                "summary,paired,2,999,1,1\n",
                "ops_per_second",
                True,
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            for filename, (body, metric, higher_is_better) in fixtures.items():
                path = Path(directory) / filename
                path.write_text(common + body, encoding="utf-8")
                parsed = parse_output(path)
                self.assertEqual(len(parsed["diagnostics"]), 1, filename)
                diagnostic = parsed["diagnostics"][0]
                self.assertEqual(diagnostic["metric_name"], metric, filename)
                self.assertIs(
                    diagnostic["higher_is_better"], higher_is_better, filename
                )
                self.assertEqual(diagnostic["samples"], 2, filename)
                validate_runs([parsed])

    def test_memory_census_is_a_strict_single_sample_diagnostic(self) -> None:
        content = (
            "# git_sha=abc123\n"
            "# arch=x86_64\n"
            "# platform=linux\n"
            "# compiler=clang 20\n"
            "# benchmark_warmup=0\n"
            "# benchmark_repeats=1\n"
            "entries=100 key_bytes=16 value_bytes=64 workers=1 process_rss_bytes=1048576 "
            "attributed_live_payload_lower_bound_bytes=524288 unattributed_rss_bytes=524288\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "memory-census.txt"
            path.write_text(content, encoding="utf-8")
            parsed = parse_output(path)
        self.assertEqual(parsed["diagnostics"][0]["metric_name"], "process_rss_bytes")
        self.assertEqual(parsed["diagnostics"][0]["median"], 1048576)
        validate_runs([parsed])
        parsed["diagnostics"][0]["warmup"] = 1
        with self.assertRaisesRegex(ValueError, "warmup does not match"):
            validate_runs([parsed])

        parsed["diagnostics"][0]["warmup"] = 0
        parsed["diagnostics"][0]["median"] = 0
        with self.assertRaisesRegex(ValueError, "median must be finite and positive"):
            validate_runs([parsed])

    def test_specialized_comparisons_respect_metric_direction(self) -> None:
        def diagnostic_run(
            value: float, higher_is_better: bool
        ) -> list[dict[str, object]]:
            return [
                {
                    "source": "specialized.txt",
                    "metadata": {},
                    "results": [],
                    "diagnostics": [
                        {
                            "name": "case",
                            "dimensions": {"mode": "case"},
                            "metric_name": "primary",
                            "unit": "ms" if not higher_is_better else "ops/s",
                            "higher_is_better": higher_is_better,
                            "samples": 3,
                            "median": value,
                            "min": value - 1,
                            "max": value + 1,
                        }
                    ],
                }
            ]

        lower_current = diagnostic_run(20, False)
        self.assertEqual(
            add_diagnostic_comparisons(
                lower_current, {"runs": diagnostic_run(10, False)}
            ),
            1,
        )
        self.assertEqual(
            lower_current[0]["diagnostics"][0]["comparison"]["interpretation"],
            "regression-candidate",
        )

        higher_current = diagnostic_run(120, True)
        add_diagnostic_comparisons(higher_current, {"runs": diagnostic_run(100, True)})
        self.assertEqual(
            higher_current[0]["diagnostics"][0]["comparison"]["interpretation"],
            "improvement-candidate",
        )

        different_sampling = diagnostic_run(120, True)
        different_sampling[0]["diagnostics"][0]["samples"] = 5
        self.assertEqual(
            add_diagnostic_comparisons(
                different_sampling, {"runs": diagnostic_run(100, True)}
            ),
            0,
        )

        same_current = diagnostic_run(20, False)
        same_prior = diagnostic_run(10, False)
        same_current[0]["metadata"] = {"git_sha": "abc123"}
        same_prior[0]["metadata"] = {"git_sha": "abc123"}
        add_diagnostic_comparisons(same_current, {"runs": same_prior})
        self.assertEqual(
            same_current[0]["diagnostics"][0]["comparison"]["interpretation"],
            "same-revision-variance",
        )
        markdown = render_markdown(
            same_current, "now", "before", {"status": "compatible"}
        )
        self.assertIn("same Git revision", markdown)

        same_subject_current = diagnostic_run(20, False)
        same_subject_prior = diagnostic_run(10, False)
        same_subject_current[0]["metadata"] = {"git_sha": "current"}
        same_subject_prior[0]["metadata"] = {"git_sha": "prior"}
        subject = "c" * 64
        add_diagnostic_comparisons(
            same_subject_current,
            {
                "environment": {"benchmark_subject_sha256": subject},
                "runs": same_subject_prior,
            },
            subject,
        )
        comparison = same_subject_current[0]["diagnostics"][0]["comparison"]
        self.assertFalse(comparison["same_revision"])
        self.assertTrue(comparison["same_benchmark_subject"])
        self.assertEqual(
            comparison["interpretation"], "same-benchmark-subject-variance"
        )

    def test_environment_file_and_identity_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "environment.txt"
            path.write_text(
                "runner_os=Linux\nlogical_cpu_count=4\nignored line\n", encoding="utf-8"
            )
            parsed = parse_environment(path)
        self.assertEqual(parsed, {"runner_os": "Linux", "logical_cpu_count": 4})
        self.assertEqual(
            environment_identity(parsed),
            environment_identity(dict(reversed(parsed.items()))),
        )

    def test_exact_identity_authorizes_comparison(self) -> None:
        current = environment()
        status = comparison_environment_status(current, {"environment": dict(current)})
        self.assertEqual(status["status"], "compatible")

        current_runs = runs(90.0)
        matched = add_comparisons(current_runs, {"runs": runs(100.0)})
        self.assertEqual(matched, 1)
        comparison = current_runs[0]["results"][0]["comparison"]
        self.assertEqual(comparison["median_ops_per_second_delta_percent"], -10.0)
        self.assertEqual(comparison["interpretation"], "inconclusive-overlap")

    def test_hardware_change_suppresses_delta_and_is_rendered(self) -> None:
        current = environment(cpu_model="Current CPU")
        baseline = {"environment": environment(cpu_model="Prior CPU")}
        status = comparison_environment_status(current, baseline)
        self.assertEqual(status["status"], "incompatible")
        self.assertIn("cpu_model", status["differences"])

        current_runs = runs(90.0)
        applied_status, matched = compare_with_baseline(current_runs, baseline, current)
        self.assertEqual(applied_status["status"], "incompatible")
        self.assertEqual(matched, 0)
        self.assertNotIn("comparison", current_runs[0]["results"][0])

        markdown = render_markdown(runs(90.0), "now", "before", status)
        self.assertIn("benchmark deltas are suppressed", markdown)
        self.assertIn("Current CPU", markdown)
        self.assertNotIn("-10.00%", markdown)

    def test_contract_change_suppresses_all_deltas(self) -> None:
        current = environment(benchmark_contract_sha256="b" * 64)
        baseline = {"environment": environment(benchmark_contract_sha256="a" * 64)}
        current_runs = runs(90.0)
        status, matched = compare_with_baseline(current_runs, baseline, current)
        self.assertEqual(status["status"], "incompatible")
        self.assertIn("benchmark_contract_sha256", status["differences"])
        self.assertEqual(matched, 0)

    def test_sampling_change_does_not_match_result_identity(self) -> None:
        current = runs(90.0)
        prior = runs(100.0)
        current[0]["results"][0].update({"warmup": 1, "samples": 7})
        prior[0]["results"][0].update({"warmup": 1, "samples": 3})
        self.assertEqual(add_comparisons(current, {"runs": prior}), 0)
        self.assertNotIn("comparison", current[0]["results"][0])

    def test_old_baseline_without_environment_is_not_compared(self) -> None:
        status = comparison_environment_status(environment(), {"schema_version": 2})
        self.assertEqual(status["status"], "incompatible")
        self.assertEqual(status["reason"], "baseline-environment-missing")

    def test_every_identity_field_is_required(self) -> None:
        for field in ENVIRONMENT_IDENTITY_FIELDS:
            with self.subTest(field=field):
                current = environment()
                current[field] = "unknown"
                status = comparison_environment_status(
                    current, {"environment": environment()}
                )
                self.assertEqual(status["status"], "incompatible")
                self.assertEqual(status["reason"], "identity-fields-missing")
                self.assertEqual(status["missing_current"], [field])

    def test_disjoint_lower_range_is_regression_candidate(self) -> None:
        current_runs = runs(70.0, 65.0, 75.0)
        matched = add_comparisons(current_runs, {"runs": runs(100.0, 95.0, 105.0)})
        self.assertEqual(matched, 1)
        comparison = current_runs[0]["results"][0]["comparison"]
        self.assertEqual(comparison["interpretation"], "regression-candidate")
        self.assertEqual(len(regressions_over_threshold(current_runs, 10.0)), 1)

    def test_overlap_never_triggers_optional_threshold(self) -> None:
        current_runs = runs(80.0, 70.0, 100.0)
        add_comparisons(current_runs, {"runs": runs(100.0, 90.0, 110.0)})
        self.assertEqual(
            current_runs[0]["results"][0]["comparison"]["interpretation"],
            "inconclusive-overlap",
        )
        self.assertEqual(regressions_over_threshold(current_runs, 10.0), [])
        markdown = render_markdown(
            current_runs, "now", "before", {"status": "compatible"}
        )
        self.assertIn("inconclusive (ranges overlap)", markdown)

    def test_disjoint_higher_range_is_improvement_candidate(self) -> None:
        current_runs = runs(130.0, 125.0, 135.0)
        add_comparisons(current_runs, {"runs": runs(100.0, 95.0, 105.0)})
        self.assertEqual(
            current_runs[0]["results"][0]["comparison"]["interpretation"],
            "improvement-candidate",
        )

    def test_same_benchmark_subject_across_revisions_is_repeat_variance(self) -> None:
        subject = "c" * 64
        current_environment = environment(benchmark_subject_sha256=subject)
        baseline = {
            "environment": environment(benchmark_subject_sha256=subject),
            "runs": runs(100.0, 95.0, 105.0),
        }
        current_runs = runs(70.0, 65.0, 75.0)
        current_runs[0]["metadata"] = {"git_sha": "current123"}
        baseline["runs"][0]["metadata"] = {"git_sha": "prior456"}

        status, matched = compare_with_baseline(
            current_runs, baseline, current_environment
        )
        self.assertEqual(status["status"], "compatible")
        self.assertEqual(matched, 1)
        comparison = current_runs[0]["results"][0]["comparison"]
        self.assertFalse(comparison["same_revision"])
        self.assertTrue(comparison["same_benchmark_subject"])
        self.assertEqual(
            comparison["interpretation"], "same-benchmark-subject-variance"
        )
        self.assertEqual(regressions_over_threshold(current_runs, 10.0), [])

        markdown = render_markdown(current_runs, "now", "before", status)
        self.assertIn("same tracked benchmark subject", markdown)
        self.assertIn("same-subject repeat variance", markdown)

    def test_changed_benchmark_subject_keeps_disjoint_candidate(self) -> None:
        current_environment = environment(benchmark_subject_sha256="c" * 64)
        baseline = {
            "environment": environment(benchmark_subject_sha256="d" * 64),
            "runs": runs(100.0, 95.0, 105.0),
        }
        current_runs = runs(70.0, 65.0, 75.0)
        status, matched = compare_with_baseline(
            current_runs, baseline, current_environment
        )
        self.assertEqual(status["status"], "compatible")
        self.assertEqual(matched, 1)
        comparison = current_runs[0]["results"][0]["comparison"]
        self.assertFalse(comparison["same_benchmark_subject"])
        self.assertEqual(comparison["interpretation"], "regression-candidate")

    def test_same_revision_delta_is_repeat_variance(self) -> None:
        current_runs = strict_runs()
        current = current_runs[0]["results"][0]
        current.update(
            {
                "median_ops_per_second": 70.0,
                "min_ops_per_second": 65.0,
                "max_ops_per_second": 75.0,
            }
        )
        prior_runs = strict_runs()
        matched = add_comparisons(current_runs, {"runs": prior_runs})
        self.assertEqual(matched, 1)
        comparison = current["comparison"]
        self.assertTrue(comparison["same_revision"])
        self.assertEqual(comparison["interpretation"], "same-revision-variance")
        self.assertEqual(regressions_over_threshold(current_runs, 10.0), [])

        markdown = render_markdown(
            current_runs, "now", "before", {"status": "compatible"}
        )
        self.assertIn("same Git revision", markdown)
        self.assertIn("same-revision repeat variance", markdown)

    def test_dirty_revision_is_not_treated_as_same_source(self) -> None:
        current_runs = runs(70.0, 65.0, 75.0)
        prior_runs = runs(100.0, 95.0, 105.0)
        current_runs[0]["metadata"] = {"git_sha": "abc123-dirty"}
        prior_runs[0]["metadata"] = {"git_sha": "abc123-dirty"}
        add_comparisons(current_runs, {"runs": prior_runs})
        comparison = current_runs[0]["results"][0]["comparison"]
        self.assertFalse(comparison["same_revision"])
        self.assertEqual(comparison["interpretation"], "regression-candidate")

    def test_strict_report_accepts_complete_results(self) -> None:
        validate_runs(strict_runs())

    def test_strict_report_rejects_empty_suite(self) -> None:
        fixture = strict_runs()
        fixture[0]["results"] = []
        with self.assertRaisesRegex(ValueError, "no benchmark results"):
            validate_runs(fixture)

    def test_strict_report_rejects_duplicate_result_key(self) -> None:
        fixture = strict_runs()
        fixture[0]["results"].append(dict(fixture[0]["results"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate benchmark result key"):
            validate_runs(fixture)

    def test_strict_report_rejects_sample_mismatch(self) -> None:
        fixture = strict_runs()
        fixture[0]["results"][0]["samples"] = 2
        with self.assertRaisesRegex(ValueError, "samples do not match"):
            validate_runs(fixture)

    def test_strict_tcp_coordinates_match_source_name(self) -> None:
        validate_runs(strict_tcp_run())
        fixture = strict_tcp_run()
        fixture[0]["metadata"]["pipeline"] = 8
        with self.assertRaisesRegex(ValueError, "metadata.pipeline is 8, expected 32"):
            validate_runs(fixture)

    def test_strict_tcp_requires_same_process_thread_profile(self) -> None:
        fixture = strict_tcp_run()
        del fixture[0]["metadata"]["execution_scope"]
        with self.assertRaisesRegex(
            ValueError,
            "metadata.execution_scope is None, expected 'same-process-loopback'",
        ):
            validate_runs(fixture)

        fixture = strict_tcp_run()
        fixture[0]["metadata"]["timed_foreground_thread_floor"] = 5
        with self.assertRaisesRegex(
            ValueError,
            "metadata.timed_foreground_thread_floor is 5, expected 6",
        ):
            validate_runs(fixture)

    def test_strict_tcp_requires_ordered_reactor_input_profile(self) -> None:
        fixture = strict_tcp_run()
        del fixture[0]["results"][0]["median_reactor_input_buffer_bytes_moved"]
        with self.assertRaisesRegex(ValueError, "must be finite and non-negative"):
            validate_runs(fixture)

        fixture = strict_tcp_run()
        fixture[0]["results"][0]["median_reactor_input_buffer_compactions"] = 4
        with self.assertRaisesRegex(ValueError, "compactions median/maximum ordering"):
            validate_runs(fixture)

        fixture = strict_tcp_run()
        fixture[0]["results"][0]["median_reactor_output_buffer_bytes_moved"] = 49
        with self.assertRaisesRegex(
            ValueError, "output bytes_moved median/maximum ordering"
        ):
            validate_runs(fixture)

    def test_source_contract_accepts_exact_suite(self) -> None:
        contract = {
            "schema_version": 5,
            "suite": "fixture",
            "tcp_near_peak_fraction": 0.95,
            "required_tcp_result_fields": [
                "median_reactor_input_buffer_compactions",
                "maximum_reactor_input_buffer_compactions",
                "median_reactor_input_buffer_bytes_moved",
                "maximum_reactor_input_buffer_bytes_moved",
                "median_reactor_output_buffer_compactions",
                "maximum_reactor_output_buffer_compactions",
                "median_reactor_output_buffer_bytes_moved",
                "maximum_reactor_output_buffer_bytes_moved",
            ],
            "expected_sources": [
                {"source": "core.txt", "benchmark_warmup": 1, "benchmark_repeats": 3}
            ],
        }
        validate_source_contract(strict_runs(), contract)

    def test_source_contract_rejects_missing_and_extra_suites(self) -> None:
        contract = {
            "schema_version": 5,
            "suite": "fixture",
            "tcp_near_peak_fraction": 0.95,
            "required_tcp_result_fields": [
                "median_reactor_input_buffer_compactions",
                "maximum_reactor_input_buffer_compactions",
                "median_reactor_input_buffer_bytes_moved",
                "maximum_reactor_input_buffer_bytes_moved",
                "median_reactor_output_buffer_compactions",
                "maximum_reactor_output_buffer_compactions",
                "median_reactor_output_buffer_bytes_moved",
                "maximum_reactor_output_buffer_bytes_moved",
            ],
            "expected_sources": [
                {"source": "core.txt", "benchmark_warmup": 1, "benchmark_repeats": 3},
                {"source": "server.txt", "benchmark_warmup": 1, "benchmark_repeats": 3},
            ],
        }
        with self.assertRaisesRegex(ValueError, "missing sources: server.txt"):
            validate_source_contract(strict_runs(), contract)

        contract["expected_sources"] = [
            {"source": "server.txt", "benchmark_warmup": 1, "benchmark_repeats": 3}
        ]
        with self.assertRaisesRegex(ValueError, "unexpected sources: core.txt"):
            validate_source_contract(strict_runs(), contract)

    def test_tracked_source_contract_is_valid_json_contract(self) -> None:
        path = (
            Path(__file__).resolve().parents[2]
            / "engineering"
            / "performance"
            / "hosted-benchmark-contract.json"
        )
        contract = load_source_contract(path)
        self.assertEqual(contract["schema_version"], 5)
        self.assertEqual(contract["tcp_near_peak_fraction"], 0.95)
        self.assertEqual(len(contract["expected_sources"]), 21)
        validate_source_contract(
            [
                {
                    "source": entry["source"],
                    "metadata": {
                        "benchmark_warmup": entry["benchmark_warmup"],
                        "benchmark_repeats": entry["benchmark_repeats"],
                    },
                }
                for entry in contract["expected_sources"]
            ],
            contract,
        )

    def test_source_contract_rejects_weakened_sampling(self) -> None:
        contract = {
            "schema_version": 5,
            "suite": "fixture",
            "tcp_near_peak_fraction": 0.95,
            "required_tcp_result_fields": [
                "median_reactor_input_buffer_compactions",
                "maximum_reactor_input_buffer_compactions",
                "median_reactor_input_buffer_bytes_moved",
                "maximum_reactor_input_buffer_bytes_moved",
                "median_reactor_output_buffer_compactions",
                "maximum_reactor_output_buffer_compactions",
                "median_reactor_output_buffer_bytes_moved",
                "maximum_reactor_output_buffer_bytes_moved",
            ],
            "expected_sources": [
                {"source": "core.txt", "benchmark_warmup": 1, "benchmark_repeats": 7}
            ],
        }
        with self.assertRaisesRegex(ValueError, "benchmark_repeats is 3, expected 7"):
            validate_source_contract(strict_runs(), contract)

    def test_tcp_scaling_analysis_reports_best_pipeline_and_efficiency(self) -> None:
        analysis = build_tcp_scaling_analysis(tcp_runs(), environment())
        self.assertIsNotNone(analysis)
        assert analysis is not None
        self.assertEqual(analysis["status"], "complete")
        best = {
            cell["workers"]: cell
            for cell in analysis["highest_observed_median_by_workers"]
        }
        self.assertEqual(best[4]["pipeline"], 8)
        self.assertAlmostEqual(best[4]["speedup_vs_one_worker"], 3.2)
        self.assertAlmostEqual(best[4]["scaling_efficiency_percent"], 80.0)
        self.assertAlmostEqual(best[4]["gain_vs_pipeline_one_percent"], 100.0)
        self.assertEqual(
            best[1]["capacity_class"], "within-reported-capacity"
        )
        self.assertEqual(
            best[2]["capacity_class"], "combined-load-oversubscribed"
        )
        self.assertEqual(
            best[4]["capacity_class"],
            "worker-and-combined-load-oversubscribed",
        )
        self.assertAlmostEqual(
            best[4]["median_input_buffer_bytes_moved_per_operation"], 3.2
        )
        economical = {
            cell["workers"]: cell for cell in analysis["smallest_near_peak_by_workers"]
        }
        self.assertEqual(economical[4]["pipeline"], 8)
        self.assertEqual(economical[4]["retained_peak_percent"], 100.0)

        markdown = render_markdown(
            tcp_runs(), "now", None, {"status": "no-baseline"}, analysis
        )
        self.assertIn("## TCP scaling summary", markdown)
        self.assertIn("Reported runner capacity: 2 physical core(s), 4 logical CPU(s).", markdown)
        self.assertIn(
            "| 4 | Workers > physical cores; combined load > logical CPUs | 12 | 8 | 8 | 100.00% | 640 | 576–704 | +100.00% | 3.20× | 80.00% | 8 | 3.20 B | 4 | 1.60 B |",
            markdown,
        )
        self.assertIn("oversubscription are sensitivity measurements", markdown)

    def test_tcp_scaling_selects_smallest_near_peak_pipeline(self) -> None:
        fixture = tcp_runs()
        for run in fixture:
            if run["source"] == "server-tcp-w4-p32.txt":
                run["results"][0]["median_ops_per_second"] = 650.0
                run["results"][0]["min_ops_per_second"] = 630.0
                run["results"][0]["max_ops_per_second"] = 670.0
        analysis = build_tcp_scaling_analysis(fixture)
        assert analysis is not None
        economical = {
            cell["workers"]: cell for cell in analysis["smallest_near_peak_by_workers"]
        }
        self.assertEqual(economical[4]["peak_pipeline"], 32)
        self.assertEqual(economical[4]["pipeline"], 8)
        self.assertAlmostEqual(economical[4]["retained_peak_percent"], 640 / 650 * 100)

    def test_tcp_scaling_analysis_marks_missing_cells_partial(self) -> None:
        analysis = build_tcp_scaling_analysis(tcp_runs()[:1])
        self.assertIsNotNone(analysis)
        assert analysis is not None
        self.assertEqual(analysis["status"], "partial")
        self.assertEqual(len(analysis["missing_cells"]), 11)

    def test_durable_profile_excludes_volatile_completion_counters(self) -> None:
        result = {"durable_completed": 100}
        volatile = {"metadata": {"storage_mode": "volatile"}}
        durable = {"metadata": {"storage_mode": "durable-periodic"}}
        self.assertFalse(has_durable_pipeline_profile(volatile, result))
        self.assertTrue(has_durable_pipeline_profile(durable, result))

        report_runs = [
            {"source": "volatile.txt", **volatile, "results": [dict(result)]},
            {"source": "durable.txt", **durable, "results": [dict(result)]},
        ]
        markdown = render_markdown(report_runs, "now", None, {"status": "no-baseline"})
        durable_section = markdown.split("## Durable pipeline profile", 1)[1]
        self.assertIn("| durable |", durable_section)
        self.assertNotIn("| volatile |", durable_section)


if __name__ == "__main__":
    unittest.main()
