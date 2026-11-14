from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

PACKAGE_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PACKAGE_ROOT.parents[1]
sys.path.insert(0, str(PACKAGE_ROOT / "src"))

from glifistore.client import (  # noqa: E402
    Client,
    MutationOutcome,
    _enrich,
    _retryability_for,
)


def load_cases() -> list[dict]:
    path = PACKAGE_ROOT / "tests" / "fixtures" / "error_taxonomy_v1.json"
    if not path.is_file():
        path = REPO_ROOT / "tests" / "fixtures" / "error_taxonomy_v1.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    return list(data["cases"])


class ErrorTaxonomyTests(unittest.TestCase):
    def test_wire_status_category_retryability_matrix(self) -> None:
        for case in load_cases():
            with self.subTest(case["id"]):
                status = case["wire_status"]
                error = Client._status_error(status)
                self.assertEqual(error.category, case["category"])
                self.assertEqual(error.wire_status, case["wire_status"])
                self.assertEqual(error.retryability, case["read_retryability"])

                outcome = MutationOutcome(case["mutation_outcome"])
                enriched = _enrich(
                    Client._status_error(status),
                    bytes_sent=1,
                    mutation_outcome=outcome,
                    wire_status=case["wire_status"],
                )
                self.assertEqual(enriched.retryability, case["mutation_retryability"])
                self.assertEqual(
                    _retryability_for(
                        case["category"],
                        True,
                        case["mutation_outcome"] == "indeterminate",
                    ),
                    case["mutation_retryability"],
                )
                self.assertEqual(
                    case["unhealthy"],
                    case["wire_status"] in (6, 7),
                )


if __name__ == "__main__":
    unittest.main()
