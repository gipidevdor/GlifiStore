from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "benchmarks.yml"
CONTRACT = ROOT / "engineering" / "performance" / "hosted-benchmark-contract.json"
EXPECTED_PUSH_PATHS = (
    ".github/workflows/benchmarks.yml",
    "CMakeLists.txt",
    "CMakePresets.json",
    "VERSION",
    "cmake/**",
    "include/**",
    "src/**",
    "benchmarks/**",
    "engineering/performance/hosted-benchmark-contract.json",
    "engineering/tools/benchmark_subject_digest.py",
    "scripts/benchmark_report.py",
)


def push_paths(text: str) -> tuple[str, ...]:
    lines = text.splitlines()
    start = lines.index("    paths:") + 1
    paths: list[str] = []
    for line in lines[start:]:
        if not line.startswith("      - "):
            break
        paths.append(line.removeprefix("      - ").strip('"'))
    return tuple(paths)


class BenchmarkWorkflowTests(unittest.TestCase):
    def test_expensive_push_trigger_has_exact_dependency_scope(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(push_paths(text), EXPECTED_PUSH_PATHS)
        for excluded in ("docs/**", "artwork/**", "sdk/**"):
            self.assertNotIn(excluded, push_paths(text))

    def test_scheduled_and_manual_safety_nets_remain(self) -> None:
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("  workflow_dispatch:", text)
        self.assertIn('    - cron: "23 4 * * 1"', text)
        self.assertIn("    branches: [main]", text)

    def test_hosted_matrix_contract_has_all_twenty_one_sources(self) -> None:
        contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
        self.assertEqual(contract["schema_version"], 5)
        self.assertEqual(contract["tcp_near_peak_fraction"], 0.95)
        self.assertEqual(
            contract["required_tcp_result_fields"],
            [
                "median_reactor_input_buffer_compactions",
                "maximum_reactor_input_buffer_compactions",
                "median_reactor_input_buffer_bytes_moved",
                "maximum_reactor_input_buffer_bytes_moved",
                "median_reactor_output_buffer_compactions",
                "maximum_reactor_output_buffer_compactions",
                "median_reactor_output_buffer_bytes_moved",
                "maximum_reactor_output_buffer_bytes_moved",
            ],
        )
        expected = [
            "core.txt",
            "parallel-uniform.txt",
            "parallel-worker-affine.txt",
            "parallel-single-worker.txt",
            "parallel-zipf.txt",
            "durable-sync.txt",
            "durable-periodic.txt",
            "durable-group.txt",
            *[
                f"server-tcp-w{workers}-p{pipeline}.txt"
                for workers in (1, 2, 4)
                for pipeline in (1, 8, 32, 128)
            ],
            "server-latency-w2-p32.txt",
        ]
        self.assertEqual(
            [entry["source"] for entry in contract["expected_sources"]], expected
        )
        sampling = {
            entry["source"]: (entry["benchmark_warmup"], entry["benchmark_repeats"])
            for entry in contract["expected_sources"]
        }
        self.assertEqual(sampling["durable-sync.txt"], (1, 3))
        self.assertEqual(sampling["durable-periodic.txt"], (1, 5))
        self.assertEqual(sampling["durable-group.txt"], (1, 3))
        self.assertTrue(
            all(
                values == (1, 7)
                for source, values in sampling.items()
                if source not in {"durable-sync.txt", "durable-periodic.txt", "durable-group.txt"}
            )
        )
        text = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("for distribution in uniform worker-affine single-worker zipf", text)
        self.assertIn("for workers in 1 2 4", text)
        self.assertIn("for pipeline in 1 8 32 128", text)
        self.assertIn("benchmark_contract_sha256=$(sha256sum", text)
        self.assertIn(
            "benchmark_subject_sha256=$(python engineering/tools/benchmark_subject_digest.py)",
            text,
        )
        self.assertIn("physical_cpu_count=$(lscpu -p=CORE,SOCKET", text)
        self.assertIn('cmake_identity=$(cmake --version | head -1)', text)
        self.assertIn('ninja_identity=$(ninja --version)', text)
        self.assertIn(
            "--source-contract engineering/performance/hosted-benchmark-contract.json", text
        )
        for command_fragment in (
            "--filter all --ops 200000 --warmup 1 --repeats 7 --pin-cpu",
            '--distribution "$distribution" --warmup 1 --repeats 7',
            "--filter store-durable-put --ops 256 --workers 1 --warmup 1 --repeats 3",
            "--filter store-durable-periodic-read-after-write",
            "--ops 20000 --workers 1 --warmup 1 --repeats 5",
            '--latency --warmup 1 --repeats 3',
            '--executor-affinity --warmup 1 --repeats 7',
            '--executor-affinity --latency --warmup 1 --repeats 7',
        ):
            self.assertIn(command_fragment, text)


if __name__ == "__main__":
    unittest.main()
