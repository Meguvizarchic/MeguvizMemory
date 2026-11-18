#!/usr/bin/env python3
"""Offline tests for the Layer 1 allocation-site classifier."""
import json
from pathlib import Path
import unittest

import classify_allocation_sites as classifier

ROOT = Path(__file__).resolve().parents[1]


def classify(site):
    report = classifier.classify_inventory({"schema_version": 1, "sites": [site]})
    return report["sites"][0]


class ClassifierTest(unittest.TestCase):
    def test_excluded_wins_over_other_signals(self):
        row = classify({"id": "pool", "kind": "heap_blocks", "shape": "fixed",
                        "lifetime": "short_batch", "existing_optimization": True,
                        "size_bytes": {"typical": 64}, "calls_per_batch": 64})
        self.assertEqual(row["classification"], "excluded")

    def test_same_shape_batch_is_arena_candidate(self):
        row = classify({"id": "batch", "kind": "heap_blocks", "shape": "fixed",
                        "lifetime": "short_batch", "size_bytes": {"typical": 64},
                        "alignment": 16, "calls_per_batch": 64})
        self.assertEqual(row["classification"], "arena_candidate")
        self.assertEqual(row["suggested_chunk_bytes"], 4096)
        self.assertFalse(row["requires_shadow"])

    def test_mixed_shape_is_uncertain(self):
        row = classify({"id": "mixed", "kind": "heap_blocks", "shape": "mixed",
                        "lifetime": "short_batch", "size_bytes": {"typical": 64},
                        "calls_per_batch": 64})
        self.assertEqual(row["classification"], "uncertain")
        self.assertTrue(row["requires_shadow"])

    def test_long_lifetime_pins_chunk_so_system(self):
        row = classify({"id": "long", "kind": "heap_blocks", "shape": "fixed",
                        "lifetime": "escape_async", "size_bytes": {"typical": 64},
                        "calls_per_batch": 64})
        self.assertEqual(row["classification"], "system_candidate")

    def test_single_large_buffer_is_system(self):
        row = classify({"id": "dex", "kind": "single_buffer",
                        "size_bytes": {"typical": 10540184}, "lifetime": "long"})
        self.assertEqual(row["classification"], "system_candidate")

    def test_borrowable_single_buffer_is_import(self):
        row = classify({"id": "imp", "kind": "single_buffer",
                        "external_owner_borrowable": True, "lifetime": "long"})
        self.assertEqual(row["classification"], "import_candidate")

    def test_seed_inventory_matches_manual_decisions(self):
        payload = json.loads((ROOT / "ALLOCATION_SITES.json").read_text(encoding="utf-8"))
        report = classifier.classify_inventory(payload)
        got = {row["id"]: row["classification"] for row in report["sites"]}
        self.assertEqual(got, {
            "bil.frame-scratch": "excluded",
            "bil.frame-buffer-ledger": "excluded",
            "bil.frame-guard-geometry": "excluded",
            "bil.frame-engine-staging": "excluded",
            "bil.frame-engine-local-containers": "system_candidate",
            "bil.danmaku-flat": "uncertain",
            "bil.dexkit-session": "system_candidate",
            "repo.dex-import-consumer": "import_candidate",
            "repo.image-workspace": "uncertain",
            "repo.lease-transfer": "copyto_candidate",
        })
        self.assertFalse(report["performance_qualified"])

    def test_invalid_inventory_rejected(self):
        for bad in (
                {"schema_version": 2, "sites": [{"id": "x", "kind": "stack"}]},
                {"schema_version": 1, "sites": []},
                {"schema_version": 1, "sites": [{"id": "x", "kind": "nope"}]},
                {"schema_version": 1, "sites": [{"id": "x", "kind": "stack"},
                                                  {"id": "x", "kind": "gpu"}]},
                {"schema_version": 1, "sites": [{"id": "x", "kind": "heap_blocks",
                                                  "alignment": 3}]},
                {"schema_version": 1, "sites": [{"id": "x", "kind": "heap_blocks",
                                                  "heap_allocation": 1}]},
        ):
            with self.assertRaises(classifier.InventoryError):
                classifier.classify_inventory(bad)

    def test_cli_writes_output(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out.json"
            code = classifier.main(["--inventory", str(ROOT / "ALLOCATION_SITES.json"),
                                    "--output", str(out)])
            self.assertEqual(code, 0)
            self.assertEqual(json.loads(out.read_text(encoding="utf-8"))["summary"]["excluded"], 4)
        self.assertEqual(classifier.main(["--inventory", str(ROOT / "missing.json")]), 1)


if __name__ == "__main__":
    unittest.main()
