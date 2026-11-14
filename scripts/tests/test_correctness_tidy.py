from __future__ import annotations

import json
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from engineering.tools.run_correctness_tidy import (
    Finding,
    TriageRule,
    apply_triage,
    available_checks,
    clang_tidy_environment,
    classify,
    run_sweep,
    selected_checks,
)


class CorrectnessTidyTests(unittest.TestCase):
    def test_selects_only_supported_requested_checks(self) -> None:
        selected = selected_checks(
            (
                "bugprone-use-after-move",
                "cppcoreguidelines-no-suspend-with-lock",
                "modernize-use-nullptr",
            )
        )
        self.assertEqual(
            (
                "bugprone-use-after-move",
                "cppcoreguidelines-no-suspend-with-lock",
            ),
            selected,
        )

    def test_classification_separates_correctness_and_maintainability(self) -> None:
        self.assertEqual("A", classify("clang-analyzer-core.NullDereference"))
        self.assertEqual("B", classify("bugprone-empty-catch"))
        self.assertEqual("C", classify("bugprone-argument-comment"))

    def test_available_checks_parses_indented_tool_output(self) -> None:
        completed = subprocess.CompletedProcess(
            ["clang-tidy"], 0, "Enabled checks:\n    bugprone-use-after-move\n", ""
        )
        with patch("engineering.tools.run_correctness_tidy.subprocess.run", return_value=completed):
            self.assertEqual(("bugprone-use-after-move",), available_checks("clang-tidy"))

    def test_macos_environment_discovers_sdk_root(self) -> None:
        completed = subprocess.CompletedProcess(["xcrun"], 0, "/sdk\n", "")
        with (
            patch("engineering.tools.run_correctness_tidy.platform.system", return_value="Darwin"),
            patch("engineering.tools.run_correctness_tidy.os.environ", {}),
            patch("engineering.tools.run_correctness_tidy.shutil.which", return_value="/usr/bin/xcrun"),
            patch("engineering.tools.run_correctness_tidy.subprocess.run", return_value=completed),
        ):
            self.assertEqual("/sdk", clang_tidy_environment().get("SDKROOT"))

    def test_sweep_deduplicates_header_findings_across_translation_units(self) -> None:
        with TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "include").mkdir()
            build = root / "build"
            build.mkdir()
            sources = [root / "src/a.cpp", root / "src/b.cpp"]
            for source in sources:
                source.write_text("", encoding="utf-8")
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {"directory": str(root), "file": str(source), "command": "c++ -c x.cpp"}
                        for source in sources
                    ]
                ),
                encoding="utf-8",
            )

            def fake_run(command: list[str], **_: object) -> subprocess.CompletedProcess[str]:
                if "--list-checks" in command:
                    return subprocess.CompletedProcess(
                        command, 0, "Enabled checks:\n    clang-analyzer-core.NullDereference\n", ""
                    )
                diagnostic = (
                    f"{root}/include/x.hpp:7:3: warning: null dereference "
                    "[clang-analyzer-core.NullDereference]\n"
                    "/sdk/include/c++/invoke.h:4:2: warning: library diagnostic "
                    "[clang-analyzer-core.NullDereference]\n"
                )
                return subprocess.CompletedProcess(command, 0, diagnostic, "")

            with patch("engineering.tools.run_correctness_tidy.subprocess.run", side_effect=fake_run):
                checks, source_count, findings, failures = run_sweep(root, build, "clang-tidy", 2)

            self.assertEqual(("clang-analyzer-core.NullDereference",), checks)
            self.assertEqual(2, source_count)
            self.assertEqual(1, len(findings))
            self.assertEqual(("src/a.cpp", "src/b.cpp"), findings[0].translation_units)
            self.assertEqual((), failures)

    def test_triage_records_rationale_and_original_classification(self) -> None:
        finding = Finding(
            classification="A",
            original_classification="A",
            check="performance-no-int-to-ptr",
            file="src/server/poller_kqueue.cpp",
            line=22,
            column=12,
            severity="warning",
            message="integer to pointer cast",
            translation_units=("src/server/poller_kqueue.cpp",),
        )
        rule = TriageRule(
            identifier="kqueue-udata",
            check="performance-no-int-to-ptr",
            file="src/server/poller_kqueue.cpp",
            message="integer to pointer cast*",
            classification="D",
            rationale="The platform ABI requires an opaque pointer round-trip.",
        )

        findings, unused = apply_triage((finding,), (rule,))

        self.assertEqual("D", findings[0].classification)
        self.assertEqual("A", findings[0].original_classification)
        self.assertEqual("kqueue-udata", findings[0].triage_rule)
        self.assertEqual((), unused)

    def test_triage_reports_stale_rules(self) -> None:
        rule = TriageRule(
            identifier="stale",
            check="bugprone-*",
            file="src/missing.cpp",
            message="*",
            classification="D",
            rationale="No longer emitted.",
        )
        findings, unused = apply_triage((), (rule,))
        self.assertEqual((), findings)
        self.assertEqual(("stale",), unused)


if __name__ == "__main__":
    unittest.main()
