import json
import tempfile
import unittest
from pathlib import Path

from engineering.tools.audit_memory_orders import build_report


class MemoryOrderAuditTests(unittest.TestCase):
    def test_every_explicit_order_receives_one_record(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src" / "sample.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(
                "value.store(1, std::memory_order_release);\n"
                "return value.load(std::memory_order_acquire);\n",
                encoding="utf-8",
            )
            (root / "include" / "glifistore").mkdir(parents=True)
            policy = root / "policy.json"
            policy.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "rules": [
                            {
                                "id": "sample",
                                "paths": ["src/sample.cpp"],
                                "classification": "correctness",
                                "allowed_orders": ["release", "acquire"],
                                "writer": "writer",
                                "readers": "reader",
                                "published_or_protected_data": "payload",
                                "required_happens_before": "release/acquire",
                                "invariant": "complete payload",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            report = build_report(root, policy)
            self.assertEqual(report["operation_count"], 2)
            self.assertEqual([row["object"] for row in report["records"]], ["value", "value"])

    def test_relaxed_order_requires_positive_justification(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src" / "sample.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("value.load(std::memory_order_relaxed);\n", encoding="utf-8")
            (root / "include" / "glifistore").mkdir(parents=True)
            policy = root / "policy.json"
            policy.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "rules": [
                            {
                                "id": "sample",
                                "paths": ["src/sample.cpp"],
                                "classification": "telemetry",
                                "allowed_orders": ["relaxed"],
                                "writer": "writer",
                                "readers": "reader",
                                "published_or_protected_data": "counter",
                                "required_happens_before": "none",
                                "invariant": "atomic only",
                            }
                        ],
                    }
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "positive justification"):
                build_report(root, policy)


if __name__ == "__main__":
    unittest.main()
