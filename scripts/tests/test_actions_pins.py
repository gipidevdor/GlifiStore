from __future__ import annotations

import fnmatch
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = ROOT / "engineering/tools/validate_actions_pins.py"
SHA = "a" * 40
OTHER_SHA = "b" * 40


class ActionsPinTests(unittest.TestCase):
    def validate(self, workflows: dict[str, str]) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workflow_dir = root / ".github/workflows"
            workflow_dir.mkdir(parents=True)
            (root / ".github/dependabot.yml").write_text(
                "version: 2\nupdates:\n  - package-ecosystem: github-actions\n"
                "    directory: /\n    schedule:\n      interval: weekly\n"
                "    # Require 40-character commit SHAs.\n",
                encoding="utf-8",
            )
            for name, body in workflows.items():
                (workflow_dir / name).write_text(body, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(VALIDATOR), "--root", str(root)],
                capture_output=True,
                text=True,
                check=False,
            )

    def test_matching_codeql_steps_across_workflows_pass(self) -> None:
        result = self.validate({
            "codeql.yml": (
                f"- uses: github/codeql-action/init@{SHA}\n"
                f"- uses: github/codeql-action/analyze@{SHA}\n"
            ),
            "scorecard.yml": f"- uses: github/codeql-action/upload-sarif@{SHA}\n",
        })
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_partial_codeql_upgrade_is_rejected(self) -> None:
        result = self.validate({
            "codeql.yml": (
                f"- uses: github/codeql-action/init@{OTHER_SHA}\n"
                f"- uses: github/codeql-action/analyze@{SHA}\n"
            ),
        })
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same commit SHA", result.stderr)
        self.assertIn("codeql.yml:1", result.stderr)
        self.assertIn("codeql.yml:2", result.stderr)

    def test_stale_codeql_in_another_workflow_is_rejected(self) -> None:
        result = self.validate({
            "codeql.yml": f"- uses: github/codeql-action/init@{OTHER_SHA}\n",
            "release.yml": f"- uses: github/codeql-action/analyze@{SHA}\n",
        })
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("codeql.yml:1", result.stderr)
        self.assertIn("release.yml:1", result.stderr)

    def test_unrelated_actions_can_use_different_revisions(self) -> None:
        result = self.validate({
            "ci.yml": (
                f"- uses: actions/checkout@{OTHER_SHA}\n"
                f"- uses: github/codeql-action/init@{SHA}\n"
            ),
        })
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_mutable_codeql_ref_remains_rejected(self) -> None:
        result = self.validate({
            "codeql.yml": "- uses: github/codeql-action/init@v4\n",
        })
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("require 40-char commit SHA", result.stderr)

    def test_dependabot_groups_all_codeql_steps_for_each_update_type(self) -> None:
        config = yaml.safe_load((ROOT / ".github/dependabot.yml").read_text())
        actions = next(
            update for update in config["updates"]
            if update["package-ecosystem"] == "github-actions"
        )
        for update_type in ("version-updates", "security-updates"):
            matching_groups = []
            for dependency in ("init", "analyze", "upload-sarif"):
                name = f"github/codeql-action/{dependency}"
                matches = [
                    group_name for group_name, group in actions["groups"].items()
                    if group.get("applies-to", "version-updates") == update_type
                    and any(fnmatch.fnmatchcase(name, pattern)
                            for pattern in group["patterns"])
                    and not any(fnmatch.fnmatchcase(name, pattern)
                                for pattern in group.get("exclude-patterns", []))
                ]
                self.assertEqual(len(matches), 1, (update_type, name, matches))
                matching_groups.append(matches[0])
            self.assertEqual(len(set(matching_groups)), 1, update_type)


if __name__ == "__main__":
    unittest.main()
