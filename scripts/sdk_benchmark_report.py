#!/usr/bin/env python3
"""Validate and render the native SDK benchmark matrix."""

from __future__ import annotations

import argparse
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path


SDK_ORDER = ("cpp", "python", "perl", "go", "erlang", "ruby")
RESULT_PATTERN = re.compile(
    r"name=(?P<name>\S+)\s+"
    r"sdk_version=(?P<sdk_version>\S+)\s+"
    r"runtime=(?P<runtime>\S+)\s+"
    r"execution=(?P<execution>\S+)\s+"
    r"(?:transport=(?P<transport>\S+)\s+)?"
    r"workers=(?P<workers>\d+)\s+"
    r"pipeline_pairs=(?P<pipeline_pairs>\d+)\s+"
    r"operations=(?P<operations>\d+)\s+"
    r"samples=(?P<samples>\d+)\s+"
    r"median_seconds=(?P<median_seconds>[0-9.eE+-]+)\s+"
    r"min_seconds=(?P<min_seconds>[0-9.eE+-]+)\s+"
    r"max_seconds=(?P<max_seconds>[0-9.eE+-]+)\s+"
    r"median_ops_per_second=(?P<median_ops_per_second>[0-9.eE+-]+)\s+"
    r"min_ops_per_second=(?P<min_ops_per_second>[0-9.eE+-]+)\s+"
    r"max_ops_per_second=(?P<max_ops_per_second>[0-9.eE+-]+)"
)


@dataclass(frozen=True)
class ExpectedResult:
    sdk: str
    runtime: str
    execution: str
    workers: int
    pipeline: int
    file: str


def positive_csv(value: str, label: str) -> list[int]:
    try:
        parsed = [int(item) for item in value.split(",") if item]
    except ValueError as error:
        raise ValueError(f"{label} must be a comma-separated list of integers") from error
    if not parsed or any(item <= 0 for item in parsed) or len(set(parsed)) != len(parsed):
        raise ValueError(f"{label} must contain unique positive integers")
    return parsed


def expected_results(
    available: set[str], workers: list[int], pipelines: list[int]
) -> list[ExpectedResult]:
    expected: list[ExpectedResult] = []
    for worker in workers:
        for pipeline in pipelines:
            label = f"w{worker}-p{pipeline}"
            if "cpp" in available:
                expected.append(
                    ExpectedResult("cpp", "native", "concurrent", worker, pipeline,
                                   f"cpp/concurrent-{label}.txt")
                )
            if "python" in available:
                expected.extend(
                    (
                        ExpectedResult("python", "sync", "concurrent", worker, pipeline,
                                       f"python/sync-concurrent-{label}.txt"),
                        ExpectedResult("python", "sync", "sequential", worker, pipeline,
                                       f"python/sync-sequential-{label}.txt"),
                        ExpectedResult("python", "async", "concurrent", worker, pipeline,
                                       f"python/async-concurrent-{label}.txt"),
                    )
                )
            if "perl" in available:
                expected.append(
                    ExpectedResult(
                        "perl", "sync", "single-process-worker-sequential", worker, pipeline,
                        f"perl/sequential-{label}.txt",
                    )
                )
                if worker > 1:
                    expected.append(
                        ExpectedResult(
                            "perl", "sync", "single-process-worker-concurrent", worker, pipeline,
                            f"perl/concurrent-{label}.txt",
                        )
                    )
            if "go" in available:
                expected.extend(
                    (
                        ExpectedResult("go", "sync", "concurrent", worker, pipeline,
                                       f"go/concurrent-{label}.txt"),
                        ExpectedResult("go", "sync", "sequential", worker, pipeline,
                                       f"go/sequential-{label}.txt"),
                    )
                )
            for sdk in ("erlang", "ruby"):
                if sdk not in available:
                    continue
                expected.append(
                    ExpectedResult(
                        sdk, "sync", "single-process-worker-sequential", worker, pipeline,
                        f"{sdk}/sequential-{label}.txt",
                    )
                )
                if worker > 1:
                    expected.append(
                        ExpectedResult(
                            sdk, "sync", "single-process-worker-concurrent", worker, pipeline,
                            f"{sdk}/concurrent-{label}.txt",
                        )
                    )
    return expected


def parse_result(path: Path, relative: str) -> dict[str, object]:
    match = RESULT_PATTERN.search(path.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"could not parse result line in {relative}")
    row: dict[str, object] = match.groupdict()
    row["file"] = relative
    for key in ("workers", "pipeline_pairs", "operations", "samples"):
        row[key] = int(str(row[key]))
    for key in (
        "median_seconds", "min_seconds", "max_seconds",
        "median_ops_per_second", "min_ops_per_second", "max_ops_per_second",
    ):
        value = float(str(row[key]))
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"{relative}: {key} must be finite and positive")
        row[key] = value
    return row


def validate_row(
    row: dict[str, object], expected: ExpectedResult, sdk_version: str, ops: int, repeats: int
) -> None:
    names = {
        "cpp": "cpp_client_pipeline_read_after_write",
        "python": (
            "python_async_client_pipeline_read_after_write"
            if expected.runtime == "async"
            else "python_client_pipeline_read_after_write"
        ),
        "perl": "perl_client_pipeline_read_after_write",
        "go": "go_client_pipeline_read_after_write",
        "erlang": "erlang_client_pipeline_read_after_write",
        "ruby": "ruby_client_pipeline_read_after_write",
    }
    checks = {
        "name": names[expected.sdk],
        "sdk_version": sdk_version,
        "runtime": expected.runtime,
        "execution": expected.execution,
        "workers": expected.workers,
        "pipeline_pairs": expected.pipeline,
        "operations": ops * 2,
        "samples": repeats,
    }
    for field, wanted in checks.items():
        if row[field] != wanted:
            raise ValueError(
                f"{expected.file}: {field} is {row[field]!r}, expected {wanted!r}"
            )
    transport = row.get("transport")
    if expected.sdk == "go" and transport != "cleartext":
        raise ValueError(f"{expected.file}: transport is {transport!r}, expected 'cleartext'")
    if expected.sdk != "go" and transport is not None:
        raise ValueError(f"{expected.file}: unexpected transport {transport!r}")
    for prefix in ("seconds", "ops_per_second"):
        minimum = float(row[f"min_{prefix}"])
        median = float(row[f"median_{prefix}"])
        maximum = float(row[f"max_{prefix}"])
        if not minimum <= median <= maximum:
            raise ValueError(
                f"{expected.file}: invalid {prefix} ordering {minimum} <= {median} <= {maximum}"
            )


def build_report(
    outdir: Path,
    sdk_version: str,
    workers: list[int],
    pipelines: list[int],
    ops: int,
    warmup: int,
    repeats: int,
    available: set[str],
) -> dict[str, object]:
    unknown = available.difference(SDK_ORDER)
    if unknown:
        raise ValueError(f"unknown SDK(s): {', '.join(sorted(unknown))}")
    expected = expected_results(available, workers, pipelines)
    expected_paths = {item.file for item in expected}
    actual_paths = {
        str(path.relative_to(outdir))
        for sdk in SDK_ORDER
        for path in (outdir / sdk).glob("*.txt")
    }
    missing = sorted(expected_paths - actual_paths)
    unexpected = sorted(actual_paths - expected_paths)
    if missing or unexpected:
        details = []
        if missing:
            details.append(f"missing result files: {', '.join(missing)}")
        if unexpected:
            details.append(f"unexpected result files: {', '.join(unexpected)}")
        raise ValueError("; ".join(details))

    rows: list[dict[str, object]] = []
    for item in expected:
        row = parse_result(outdir / item.file, item.file)
        validate_row(row, item, sdk_version, ops, repeats)
        row["sdk"] = item.sdk
        rows.append(row)

    missing_sdks = [sdk for sdk in SDK_ORDER if sdk not in available]
    return {
        "schema_version": 1,
        "sdk_version": sdk_version,
        "comparison_status": "complete" if not missing_sdks else "exploratory",
        "missing_sdks": missing_sdks,
        "matrix": {
            "workers": workers,
            "pipeline_pairs": pipelines,
            "operations": ops,
            "warmup_samples": warmup,
            "samples": repeats,
            "expected_results": len(expected),
            "parsed_results": len(rows),
        },
        "results": rows,
    }


def render_markdown(report: dict[str, object], outdir: Path) -> str:
    rows = report["results"]
    assert isinstance(rows, list)
    status = str(report["comparison_status"])
    missing = report["missing_sdks"]
    lines = [
        f"# GlifiStore SDK client benchmarks — version `{report['sdk_version']}`",
        "",
        f"Validated `{len(rows)}` result files from `{outdir.name}`.",
        f"Comparison status: **{status}**.",
        "",
    ]
    if missing:
        assert isinstance(missing, list)
        lines.extend(
            [
                f"Unavailable SDKs: `{', '.join(str(item) for item in missing)}`. "
                "This run is exploratory and must not be used as a complete cross-SDK comparison.",
                "",
            ]
        )
    lines.extend(
        [
            "Workload: validated ordered `PUT`/`GET` pipeline read-after-write, "
            "value size 64 bytes,",
            "volatile `glifistored`, same-host loopback. Median ops/s is the "
            "comparison statistic.",
            "",
            "| SDK | Runtime | Execution | Workers | Pipeline pairs | Median ops/s | "
            "Min ops/s | Max ops/s | Median s |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in rows:
        assert isinstance(row, dict)
        lines.append(
            "| {sdk} | {runtime} | {execution} | {workers} | {pipeline_pairs} | "
            "{median_ops_per_second:,.0f} | {min_ops_per_second:,.0f} | "
            "{max_ops_per_second:,.0f} | {median_seconds:.6f} |".format(**row)
        )
    lines.extend(
        [
            "",
            "## Notes",
            "",
            "- The report validator rejects missing/extra cells, version drift, wrong "
            "sample counts, and invalid statistics.",
            "- Concurrent modes overlap one execution context per Worker; sequential "
            "modes drain Workers in order.",
            "- Perl uses process-friendly socket overlap and does not use shared-client ithreads.",
            "- Do not treat same-host loopback numbers as production capacity.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", required=True, type=Path)
    parser.add_argument("--sdk-version", required=True)
    parser.add_argument("--workers", required=True)
    parser.add_argument("--pipelines", required=True)
    parser.add_argument("--ops", required=True, type=int)
    parser.add_argument("--warmup", required=True, type=int)
    parser.add_argument("--repeats", required=True, type=int)
    parser.add_argument("--available", required=True)
    args = parser.parse_args()
    if args.ops <= 0 or args.repeats <= 0 or args.warmup < 0:
        parser.error("ops and repeats must be positive; warmup must be non-negative")
    try:
        report = build_report(
            args.outdir,
            args.sdk_version,
            positive_csv(args.workers, "workers"),
            positive_csv(args.pipelines, "pipelines"),
            args.ops,
            args.warmup,
            args.repeats,
            {item for item in args.available.split(",") if item},
        )
    except ValueError as error:
        parser.error(str(error))
    (args.outdir / "results.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    (args.outdir / "summary.md").write_text(
        render_markdown(report, args.outdir), encoding="utf-8"
    )
    print(
        f"validated {report['matrix']['parsed_results']} results; "
        f"comparison_status={report['comparison_status']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
