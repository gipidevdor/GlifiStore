from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ReleaseCompatibilityWorkflowTests(unittest.TestCase):
    def test_packaging_job_installs_compatibility_validator_dependency(self) -> None:
        workflow = (ROOT / ".github/workflows/release-compat.yml").read_text(encoding="utf-8")
        self.assertIn("package-release-compatibility-artifacts.sh", workflow)
        self.assertRegex(workflow, re.compile(r"pip install[^\n]*\bPyYAML\b"))


if __name__ == "__main__":
    unittest.main()
