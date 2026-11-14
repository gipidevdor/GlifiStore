from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.sdk_benchmark_report import SDK_ORDER, build_report, expected_results


def result_line(sdk: str, runtime: str, execution: str, version: str = "0.1.0") -> str:
    names = {
        "cpp": "cpp_client_pipeline_read_after_write",
        "python": (
            "python_async_client_pipeline_read_after_write"
            if runtime == "async"
            else "python_client_pipeline_read_after_write"
        ),
        "perl": "perl_client_pipeline_read_after_write",
        "go": "go_client_pipeline_read_after_write",
        "erlang": "erlang_client_pipeline_read_after_write",
        "ruby": "ruby_client_pipeline_read_after_write",
    }
    transport = " transport=cleartext" if sdk == "go" else ""
    return (
        f"name={names[sdk]} sdk_version={version} runtime={runtime} execution={execution}"
        f"{transport} "
        "workers=1 pipeline_pairs=8 operations=20 samples=3 "
        "median_seconds=0.2 min_seconds=0.1 max_seconds=0.3 "
        "median_ops_per_second=100 min_ops_per_second=66.6 max_ops_per_second=200\n"
    )


def materialize(root: Path, available: set[str]) -> list[Path]:
    paths: list[Path] = []
    for expected in expected_results(available, [1], [8]):
        path = root / expected.file
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            result_line(expected.sdk, expected.runtime, expected.execution), encoding="utf-8"
        )
        paths.append(path)
    return paths


class SdkBenchmarkReportTests(unittest.TestCase):
    def test_complete_matrix_is_exact(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            materialize(root, set(SDK_ORDER))
            report = build_report(root, "0.1.0", [1], [8], 10, 1, 3, set(SDK_ORDER))
            self.assertEqual(report["comparison_status"], "complete")
            self.assertEqual(report["missing_sdks"], [])
            self.assertEqual(report["matrix"]["parsed_results"], 9)

    def test_missing_runtime_is_explicitly_exploratory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            available = {"cpp", "python", "perl", "go"}
            materialize(root, available)
            report = build_report(root, "0.1.0", [1], [8], 10, 1, 3, available)
            self.assertEqual(report["comparison_status"], "exploratory")
            self.assertEqual(report["missing_sdks"], ["erlang", "ruby"])

    def test_missing_cell_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = materialize(root, {"cpp", "go"})
            paths[0].unlink()
            with self.assertRaisesRegex(ValueError, "missing result files"):
                build_report(root, "0.1.0", [1], [8], 10, 1, 3, {"cpp", "go"})

    def test_version_drift_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = materialize(root, {"cpp"})
            paths[0].write_text(
                result_line("cpp", "native", "concurrent", "0.0.9"), encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "sdk_version"):
                build_report(root, "0.1.0", [1], [8], 10, 1, 3, {"cpp"})


if __name__ == "__main__":
    unittest.main()
