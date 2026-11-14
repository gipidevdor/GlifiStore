#!/usr/bin/env python3
"""Run a broad, version-adaptive clang-tidy correctness sweep.

This is intentionally separate from the small fail-closed high-signal gate.  It
discovers the checks supported by the selected clang-tidy binary, runs the real
production translation units from compile_commands.json, and emits a
machine-readable report for manual triage.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
import fnmatch
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
from typing import Iterable

try:
    from engineering.tools.run_clang_tidy_gate import production_sources
except ModuleNotFoundError:  # Direct script execution adds engineering/tools to sys.path.
    from run_clang_tidy_gate import production_sources


REQUIRED_PATTERNS = (
    "clang-analyzer-core.*",
    "clang-analyzer-cplusplus.*",
    "clang-analyzer-deadcode.*",
    "clang-analyzer-nullability.*",
    "clang-analyzer-optin.*",
    "clang-analyzer-security.*",
    "clang-analyzer-unix.*",
    "bugprone-*",
    "concurrency-*",
    "cert-*",
    "portability-*",
)

# These checks are selected individually because their diagnostics can expose
# lifetime, ownership, synchronization, bounds, or exception-safety defects.
OPTIONAL_CHECKS = (
    "cppcoreguidelines-avoid-capturing-lambda-coroutines",
    "cppcoreguidelines-misleading-capture-default-by-value",
    "cppcoreguidelines-no-suspend-with-lock",
    "cppcoreguidelines-noexcept-destructor",
    "cppcoreguidelines-noexcept-move-operations",
    "cppcoreguidelines-noexcept-swap",
    "cppcoreguidelines-owning-memory",
    "cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",
    "cppcoreguidelines-pro-type-member-init",
    "cppcoreguidelines-slicing",
    "cppcoreguidelines-virtual-class-destructor",
    "misc-coroutine-hostile-raii",
    "misc-misleading-bidirectional",
    "misc-misleading-identifier",
    "misc-redundant-expression",
    "misc-throw-by-value-catch-by-reference",
    "misc-uniqueptr-reset-release",
    "performance-move-constructor-init",
    "performance-no-int-to-ptr",
    "performance-noexcept-destructor",
    "performance-noexcept-move-constructor",
    "performance-noexcept-swap",
)

# Category A means the checker is designed to report a concrete safety or
# semantic violation.  It still requires source-level confirmation.
HIGH_CONFIDENCE_PATTERNS = (
    "clang-analyzer-core.*",
    "clang-analyzer-cplusplus.ArrayDelete",
    "clang-analyzer-cplusplus.InnerPointer",
    "clang-analyzer-cplusplus.Move",
    "clang-analyzer-cplusplus.NewDelete",
    "clang-analyzer-cplusplus.NewDeleteLeaks",
    "clang-analyzer-cplusplus.PlacementNew",
    "clang-analyzer-cplusplus.PureVirtualCall",
    "clang-analyzer-security.ArrayBound",
    "clang-analyzer-security.PointerSub",
    "clang-analyzer-security.VAList",
    "clang-analyzer-security.cert.env.InvalidPtr",
    "clang-analyzer-unix.Malloc",
    "clang-analyzer-unix.MallocSizeof",
    "clang-analyzer-unix.MismatchedDeallocator",
    "clang-analyzer-unix.cstring.BadSizeArg",
    "clang-analyzer-unix.cstring.NotNullTerminated",
    "clang-analyzer-unix.cstring.NullArg",
    "bugprone-dangling-handle",
    "bugprone-default-operator-new-on-overaligned-type",
    "bugprone-incorrect-enable-shared-from-this",
    "bugprone-infinite-loop",
    "bugprone-misplaced-operator-in-strlen-in-alloc",
    "bugprone-misplaced-pointer-arithmetic-in-alloc",
    "bugprone-multiple-new-in-one-expression",
    "bugprone-return-const-ref-from-parameter",
    "bugprone-shared-ptr-array-mismatch",
    "bugprone-suspicious-memory-comparison",
    "bugprone-suspicious-memset-usage",
    "bugprone-suspicious-realloc-usage",
    "bugprone-swapped-arguments",
    "bugprone-unchecked-optional-access",
    "bugprone-undefined-memory-manipulation",
    "bugprone-unique-ptr-array-mismatch",
    "bugprone-unused-raii",
    "bugprone-use-after-move",
    "cert-arr39-c",
    "cert-con54-cpp",
    "cert-mem57-cpp",
    "cppcoreguidelines-no-suspend-with-lock",
    "cppcoreguidelines-slicing",
    "misc-uniqueptr-reset-release",
    "performance-no-int-to-ptr",
)

# Category C checks are deliberately executed because the requested families
# include them, but their diagnostics are maintainability/style evidence and do
# not fail a correctness campaign.
MAINTAINABILITY_PATTERNS = (
    "bugprone-argument-comment",
    "bugprone-branch-clone",
    "bugprone-easily-swappable-parameters",
    "bugprone-forward-declaration-namespace",
    "bugprone-reserved-identifier",
    "bugprone-suspicious-include",
    "cert-dcl03-c",
    "cert-dcl16-c",
    "cert-dcl37-c",
    "cert-dcl50-cpp",
    "cert-dcl51-cpp",
    "cert-dcl54-cpp",
    "cert-dcl58-cpp",
    "cert-dcl59-cpp",
    "clang-analyzer-optin.performance.*",
    "cppcoreguidelines-pro-bounds-avoid-unchecked-container-access",
    "portability-avoid-pragma-once",
    "portability-restrict-system-includes",
    "portability-simd-intrinsics",
)

DIAGNOSTIC_RE = re.compile(
    r"^(.*?):(\d+):(\d+):\s+(warning|error):\s+(.*?)\s+\[([^\]]+)\]\s*$"
)


@dataclass(frozen=True)
class Finding:
    classification: str
    original_classification: str
    check: str
    file: str
    line: int
    column: int
    severity: str
    message: str
    translation_units: tuple[str, ...]
    triage_rule: str | None = None
    triage_rationale: str | None = None


@dataclass(frozen=True)
class TriageRule:
    identifier: str
    check: str
    file: str
    message: str
    classification: str
    rationale: str
    line: int | None = None


@dataclass(frozen=True)
class ToolFailure:
    source: str
    returncode: int
    output: str


def _matches(name: str, patterns: Iterable[str]) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)


def classify(check: str) -> str:
    if _matches(check, HIGH_CONFIDENCE_PATTERNS):
        return "A"
    if _matches(check, MAINTAINABILITY_PATTERNS):
        return "C"
    return "B"


def load_triage(path: Path) -> tuple[TriageRule, ...]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if raw.get("schema_version") != 1 or not isinstance(raw.get("rules"), list):
        raise ValueError("triage file must use schema_version 1 and contain a rules array")
    rules: list[TriageRule] = []
    identifiers: set[str] = set()
    for index, item in enumerate(raw["rules"]):
        if not isinstance(item, dict):
            raise ValueError(f"triage rule {index} must be an object")
        required = ("id", "check", "file", "message", "classification", "rationale")
        if any(not isinstance(item.get(field), str) or not item[field] for field in required):
            raise ValueError(f"triage rule {index} has missing or empty required fields")
        identifier = item["id"]
        if identifier in identifiers:
            raise ValueError(f"duplicate triage rule id: {identifier}")
        identifiers.add(identifier)
        if item["classification"] != "D":
            raise ValueError(
                f"triage rule {identifier} must classify as D (false positive/tool limitation)"
            )
        line = item.get("line")
        if line is not None and (not isinstance(line, int) or line <= 0):
            raise ValueError(f"triage rule {identifier} has an invalid line")
        rules.append(
            TriageRule(
                identifier=identifier,
                check=item["check"],
                file=item["file"],
                message=item["message"],
                classification=item["classification"],
                rationale=item["rationale"],
                line=line,
            )
        )
    return tuple(rules)


def apply_triage(
    findings: tuple[Finding, ...], rules: tuple[TriageRule, ...]
) -> tuple[tuple[Finding, ...], tuple[str, ...]]:
    used: set[str] = set()
    output: list[Finding] = []
    for finding in findings:
        matches = [
            rule
            for rule in rules
            if fnmatch.fnmatchcase(finding.check, rule.check)
            and fnmatch.fnmatchcase(finding.file, rule.file)
            and fnmatch.fnmatchcase(finding.message, rule.message)
            and (rule.line is None or finding.line == rule.line)
        ]
        if len(matches) > 1:
            identifiers = ", ".join(rule.identifier for rule in matches)
            raise ValueError(
                f"finding {finding.file}:{finding.line} [{finding.check}] matches multiple "
                f"triage rules: {identifiers}"
            )
        if not matches:
            output.append(finding)
            continue
        rule = matches[0]
        used.add(rule.identifier)
        output.append(
            Finding(
                classification=rule.classification,
                original_classification=finding.original_classification,
                check=finding.check,
                file=finding.file,
                line=finding.line,
                column=finding.column,
                severity=finding.severity,
                message=finding.message,
                translation_units=finding.translation_units,
                triage_rule=rule.identifier,
                triage_rationale=rule.rationale,
            )
        )
    unused = tuple(rule.identifier for rule in rules if rule.identifier not in used)
    return tuple(output), unused


def available_checks(clang_tidy: str) -> tuple[str, ...]:
    completed = subprocess.run(
        [clang_tidy, "--list-checks", "--checks=*"],
        text=True,
        capture_output=True,
        check=True,
    )
    return tuple(
        sorted(
            line.strip()
            for line in completed.stdout.splitlines()
            if line.startswith("    ") and line.strip()
        )
    )


def clang_tidy_environment() -> dict[str, str]:
    environment = os.environ.copy()
    if platform.system() != "Darwin" or environment.get("SDKROOT"):
        return environment
    xcrun = shutil.which("xcrun")
    if xcrun is None:
        return environment
    completed = subprocess.run(
        [xcrun, "--show-sdk-path"], text=True, capture_output=True, check=False
    )
    sdk_root = completed.stdout.strip()
    if completed.returncode == 0 and sdk_root:
        environment["SDKROOT"] = sdk_root
    return environment


def selected_checks(available: Iterable[str]) -> tuple[str, ...]:
    available_set = set(available)
    selected = {
        check
        for check in available_set
        if _matches(check, REQUIRED_PATTERNS) or check in OPTIONAL_CHECKS
    }
    return tuple(sorted(selected))


def _display_path(root: Path, raw: str) -> str:
    path = Path(raw)
    if not path.is_absolute():
        return raw
    try:
        return str(path.resolve().relative_to(root))
    except ValueError:
        return str(path)


def _is_production_path(root: Path, raw: str) -> bool:
    path = Path(raw)
    if not path.is_absolute():
        path = root / path
    try:
        relative = path.resolve().relative_to(root)
    except ValueError:
        return False
    return bool(relative.parts) and relative.parts[0] in {"include", "src"}


def run_sweep(
    root: Path,
    build_directory: Path,
    clang_tidy: str,
    jobs: int,
) -> tuple[tuple[str, ...], int, tuple[Finding, ...], tuple[ToolFailure, ...]]:
    root = root.resolve()
    build_directory = build_directory.resolve()
    sources = production_sources(root, build_directory)
    checks = selected_checks(available_checks(clang_tidy))
    if not checks:
        raise RuntimeError("selected clang-tidy binary exposes no requested correctness checks")
    check_argument = "-*," + ",".join(checks)
    header_filter = "^(" + re.escape(str(root / "include")) + "|" + re.escape(str(root / "src")) + ")/"
    environment = clang_tidy_environment()

    def inspect(source: Path) -> tuple[Path, subprocess.CompletedProcess[str]]:
        completed = subprocess.run(
            [
                clang_tidy,
                "-p",
                str(build_directory),
                "--quiet",
                f"--checks={check_argument}",
                f"--header-filter={header_filter}",
                str(source),
            ],
            cwd=root,
            env=environment,
            text=True,
            capture_output=True,
        )
        return source, completed

    findings_by_key: dict[tuple[str, int, int, str, str], set[str]] = {}
    severities: dict[tuple[str, int, int, str, str], str] = {}
    failures: list[ToolFailure] = []
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        for source, completed in executor.map(inspect, sources):
            output = "\n".join(
                part.rstrip() for part in (completed.stdout, completed.stderr) if part.strip()
            )
            parsed = 0
            for line in output.splitlines():
                match = DIAGNOSTIC_RE.match(line)
                if match is None:
                    continue
                parsed += 1
                raw_file, raw_line, raw_column, severity, message, check = match.groups()
                if not _is_production_path(root, raw_file):
                    continue
                key = (
                    _display_path(root, raw_file),
                    int(raw_line),
                    int(raw_column),
                    check,
                    message,
                )
                findings_by_key.setdefault(key, set()).add(str(source.relative_to(root)))
                severities[key] = severity
            if completed.returncode != 0:
                failures.append(
                    ToolFailure(
                        source=str(source.relative_to(root)),
                        returncode=completed.returncode,
                        output=output,
                    )
                )
            elif output and parsed == 0:
                failures.append(
                    ToolFailure(
                        source=str(source.relative_to(root)),
                        returncode=0,
                        output="unparsed clang-tidy output:\n" + output,
                    )
                )

    findings = tuple(
        Finding(
            classification=classify(check),
            original_classification=classify(check),
            check=check,
            file=file,
            line=line,
            column=column,
            severity=severities[(file, line, column, check, message)],
            message=message,
            translation_units=tuple(sorted(translation_units)),
        )
        for (file, line, column, check, message), translation_units in sorted(findings_by_key.items())
    )
    return checks, len(sources), findings, tuple(failures)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 4))
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--triage",
        type=Path,
        help="versioned JSON rules for manually confirmed category-D diagnostics",
    )
    parser.add_argument(
        "--fail-on",
        choices=("none", "a", "ab"),
        default="none",
        help="fail for category A, A+B, or only tool execution failures",
    )
    args = parser.parse_args()
    if args.jobs <= 0:
        parser.error("--jobs must be greater than zero")

    root = Path(__file__).resolve().parents[2]
    build_directory = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    executable = shutil.which(args.clang_tidy)
    if executable is None:
        print(f"clang-tidy correctness sweep UNAVAILABLE: executable not found: {args.clang_tidy}")
        return 2

    try:
        checks, source_count, findings, failures = run_sweep(
            root, build_directory, executable, args.jobs
        )
        unused_triage_rules: tuple[str, ...] = ()
        if args.triage is not None:
            triage_path = args.triage if args.triage.is_absolute() else root / args.triage
            findings, unused_triage_rules = apply_triage(findings, load_triage(triage_path))
            if unused_triage_rules:
                raise ValueError(
                    "stale triage rules did not match any finding: "
                    + ", ".join(unused_triage_rules)
                )
        version = subprocess.run(
            [executable, "--version"], text=True, capture_output=True, check=True
        ).stdout.splitlines()[0]
    except (OSError, ValueError, KeyError, json.JSONDecodeError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"clang-tidy correctness sweep FAILED: {error}")
        return 1

    counts = {category: 0 for category in "ABCD"}
    for finding in findings:
        counts[finding.classification] += 1
    report = {
        "schema_version": 1,
        "tool": executable,
        "version": version,
        "sdk_root": clang_tidy_environment().get("SDKROOT"),
        "build_directory": str(build_directory),
        "production_sources": source_count,
        "checks": checks,
        "classifications": counts,
        "findings": [asdict(finding) for finding in findings],
        "unused_triage_rules": list(unused_triage_rules),
        "tool_failures": [asdict(failure) for failure in failures],
    }
    if args.output is not None:
        output = args.output if args.output.is_absolute() else root / args.output
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(
        "clang-tidy correctness sweep completed "
        f"({source_count} production sources; {len(checks)} supported checks; "
        + ", ".join(f"{category}={counts[category]}" for category in "ABCD")
        + f"; tool_failures={len(failures)})"
    )
    if failures:
        for failure in failures:
            print(f"== tool failure: {failure.source} (exit {failure.returncode}) ==")
            print(failure.output)
        return 1
    failing_categories = {"A"} if args.fail_on == "a" else {"A", "B"} if args.fail_on == "ab" else set()
    if any(finding.classification in failing_categories for finding in findings):
        print(f"clang-tidy correctness sweep FAILED: untriaged category {args.fail_on.upper()} findings")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
