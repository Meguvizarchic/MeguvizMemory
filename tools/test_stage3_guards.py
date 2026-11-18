#!/usr/bin/env python3
"""Offline tests for stage 3: thresholds, draft inventory, review dates."""
from datetime import date
import json
from pathlib import Path
import tempfile
import unittest

import draft_inventory
import shadow_estimate as shadow
import validate_allocation_policy as validator

FIXED = (64, 16)


class ThresholdTest(unittest.TestCase):
    def test_defaults_echoed_in_config(self):
        report = shadow.estimate([[FIXED] * 64], 4096)
        self.assertEqual(report["config"]["thresholds"]["min_same_shape"], 0.90)
        self.assertIsNone(report["config"]["preset"])

    def test_conservative_preset_tightens_growth(self):
        report = shadow.estimate([[FIXED] * 4096], 4096, preset="conservative")
        self.assertEqual(report["config"]["thresholds"]["max_backing_growth"], 0.03)
        self.assertEqual(report["verdict"], "arena_recommended")

    def test_overrides_can_flip_verdict(self):
        strict = shadow.estimate([[FIXED] * 4096], 65536,
                                 thresholds={"max_backing_growth": 0.0})
        self.assertEqual(strict["backing_growth_ratio"], 0.0)
        impossible = shadow.estimate([[FIXED] * 4096], 4096,
                                     thresholds={"min_provider_saving": 0.999})
        self.assertNotEqual(impossible["verdict"], "arena_recommended")

    def test_bad_thresholds_rejected_with_names(self):
        for bad in ({"warp_factor": 1}, {"min_same_shape": True},
                    {"window_allocations": 0}):
            with self.assertRaises(shadow.TraceError):
                shadow.estimate([[FIXED] * 8], 4096, thresholds=bad)
        with self.assertRaises(shadow.TraceError):
            shadow.estimate([[FIXED] * 8], 4096, preset="nope")


class ThresholdCliTest(unittest.TestCase):
    def test_non_object_thresholds_file_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            trace = tmp / "t.jsonl"
            trace.write_text('{"bytes":64,"alignment":16}\n', encoding="utf-8")
            bad = tmp / "th.json"
            bad.write_text("[]", encoding="utf-8")
            self.assertEqual(shadow.main(
                ["--trace", str(trace), "--thresholds", str(bad)]), 1)
            bad.write_text("{broken", encoding="utf-8")
            self.assertEqual(shadow.main(
                ["--trace", str(trace), "--thresholds", str(bad)]), 1)

    def test_run_adoption_rejects_bad_thresholds_without_traces(self):
        import run_adoption as runner
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(shadow.TraceError):
                runner.run(root / "ALLOCATION_SITES.json", Path(tmp) / "run",
                           root / "ALLOCATION_POLICY.json",
                           thresholds={"warp_factor": 1})


class DraftInventoryTest(unittest.TestCase):
    def test_scan_extracts_sites_sizes_and_locations(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.cpp").write_text(
                "auto s = mgmem_allocate_v1(d, 4096, 16, &h);\n"
                "memory.with_allocation(bytes, body);\n"
                "// mgmem_allocate_v1(d, 1, 16, &h); commented out\n"
                "auto t = mgmem_import_external_arena_checked_v1(d, &buf, &h);\n"
                "buf.allocate(n); other(42);\n", encoding="utf-8")
            (root / "B.java").write_text(
                "var d = ManagedMemoryDomain.system(1024);\n"
                "var lease = d.allocate(128);\n", encoding="utf-8")
            (root / "decl.h").write_text(
                "MGMEM_API mgmem_status mgmem_allocate_v1(mgmem_domain *d);\n",
                encoding="utf-8")
            draft = draft_inventory.scan_roots([root])
            apis = [s["api"] for s in draft["sites"]]
            # C import with the real _v1 suffix is found; the domain
            # factory and the header declaration are not call sites.
            self.assertIn("mgmem_import_external_arena_checked_v1", apis)
            self.assertNotIn("ManagedMemoryDomain.system", apis)
            self.assertTrue(all("decl.h" not in s["location"] for s in draft["sites"]))
            by_api = {s["api"]: s for s in draft["sites"]}
            self.assertEqual(by_api["mgmem_allocate_v1"]["size_bytes"]["typical"], 4096)
            # Size stays anchored in its own call: no borrowing 42.
            allocate_rows = [s for s in draft["sites"] if s["api"] == ".allocate"]
            self.assertTrue(all(s["size_bytes"] is None or
                                s["size_bytes"]["typical"] == 128 for s in allocate_rows))
            # Locations keep a directory component and ids are unique.
            self.assertTrue(all("/" in s["location"] for s in draft["sites"]))
            ids = [s["id"] for s in draft["sites"]]
            self.assertEqual(len(ids), len(set(ids)))
            self.assertIn("roots", draft)

    def test_repo_scan_finds_real_sites(self):
        draft = draft_inventory.scan_roots([Path(__file__).resolve().parents[1] / "consumers"])
        self.assertGreater(len(draft["sites"]), 0)


class ReviewDateTest(unittest.TestCase):
    CLASSIFIED = {"schema_version": 1, "sites": [
        {"id": "s", "classification": "system_candidate"}]}

    def _policy(self, **extra):
        entry = {"site_id": "s", "mode": "system", "rollback": "system",
                 "rationale": "r", **extra}
        return {"schema_version": 1, "entries": [entry]}

    def test_overdue_warns_but_passes(self):
        receipt = validator.validate(
            self._policy(owner="me", review_by="2020-01-01"),
            self.CLASSIFIED, {}, today=date(2026, 11, 18))
        self.assertEqual(receipt["result"], "PASS")
        self.assertEqual(receipt["warnings"][0]["reason"], "review_overdue")
        self.assertEqual(receipt["warnings"][0]["owner"], "me")

    def test_future_review_no_warning_and_bad_date_rejected(self):
        receipt = validator.validate(
            self._policy(review_by="2027-01-01"), self.CLASSIFIED, {},
            today=date(2026, 11, 18))
        self.assertEqual(receipt["warnings"], [])
        with self.assertRaises(validator.PolicyError):
            validator.validate(self._policy(review_by="2027-13-99"),
                               self.CLASSIFIED, {})

    def test_missing_or_errored_entries_still_warn(self):
        receipt = validator.validate(self._policy(), self.CLASSIFIED, {},
                                     today=date(2026, 11, 18))
        self.assertEqual(receipt["warnings"][0]["reason"], "review_date_missing")
        overdue_and_wrong = {"schema_version": 1, "entries": [
            {"site_id": "ghost", "mode": "system", "rollback": "system",
             "rationale": "r", "review_by": "2020-01-01"}]}
        receipt = validator.validate(overdue_and_wrong, self.CLASSIFIED, {},
                                     today=date(2026, 11, 18))
        self.assertEqual(receipt["result"], "FAIL")
        self.assertEqual(receipt["warnings"][0]["reason"], "review_overdue")

    def test_seed_policy_has_review_fields(self):
        root = Path(__file__).resolve().parents[1]
        policy = json.loads((root / "ALLOCATION_POLICY.json").read_text(encoding="utf-8"))
        self.assertTrue(all(e.get("owner") and e.get("review_by")
                            for e in policy["entries"]))


class IncompleteMarkerTest(unittest.TestCase):
    def test_failed_run_leaves_marker(self):
        import run_adoption as runner
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "run"
            bad_trace = Path(tmp) / "bad.jsonl"
            bad_trace.write_text('{"op":"free","id":1}\n', encoding="utf-8")
            with self.assertRaises(Exception):
                runner.run(root / "ALLOCATION_SITES.json", out,
                           root / "ALLOCATION_POLICY.json",
                           [("repo.image-workspace", bad_trace)])
            marker = json.loads((out / "INCOMPLETE.json").read_text(encoding="utf-8"))
            self.assertFalse(marker["complete"])


if __name__ == "__main__":
    unittest.main()
