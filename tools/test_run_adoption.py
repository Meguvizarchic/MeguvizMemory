#!/usr/bin/env python3
"""Offline tests for stage 1: sweep, draft generation, one-click chain."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import generate_policy_draft as drafter
import run_adoption as runner
import shadow_estimate as shadow
import validate_allocation_policy as validator

ROOT = Path(__file__).resolve().parents[1]
FIXED = (64, 16)


def arena_shadow(site=None):
    epochs = [[FIXED] * 4096]
    report = shadow.estimate(epochs, 4096)
    report = shadow.attach_sweep(report, epochs)
    if site:
        report["site"] = site
    raw = (json.dumps(report) + "\n").encode()
    return hashlib.sha256(raw).hexdigest(), report


class SweepTest(unittest.TestCase):
    def test_sweep_recommends_smallest_passing_chunk(self):
        epochs = [[FIXED] * 4096]
        report = shadow.attach_sweep(shadow.estimate(epochs, 65536), epochs)
        self.assertEqual(report["recommended_chunk_bytes"], 1024)
        self.assertEqual(len(report["chunk_sweep"]), len(shadow.SWEEP_CHUNKS))

    def test_counterfactual_names_passing_chunk(self):
        # Small input in a huge chunk fails on growth; a small chunk passes.
        epochs = [[(448, 16)] * 64]
        report = shadow.attach_sweep(shadow.estimate(epochs, 262144), epochs)
        if report["verdict"] != "arena_recommended":
            self.assertIsNotNone(report["counterfactual"])
        self.assertIn("同形率", report["explanation"])

    def test_explanation_present_for_every_verdict(self):
        for epochs, chunk in ([[FIXED] * 4096], 4096), ([[(448, 16)]], 65536):
            report = shadow.estimate(epochs, chunk)
            self.assertTrue(report["explanation"])


class DraftTest(unittest.TestCase):
    CLASSIFIED = {"schema_version": 1, "sites": [
        {"id": "arena-site", "classification": "arena_candidate", "reasons": ["r"]},
        {"id": "plain", "classification": "system_candidate", "reasons": ["r"]},
        {"id": "maybe", "classification": "uncertain", "reasons": ["r"]},
    ]}

    def test_arena_entry_filled_from_shadow_and_validates(self):
        shadows = {"arena-site": arena_shadow("arena-site")}
        draft = drafter.generate(self.CLASSIFIED, shadows)
        modes = {e["site_id"]: e["mode"] for e in draft["entries"]}
        self.assertEqual(modes, {"arena-site": "arena", "plain": "system",
                                 "maybe": "shadow"})
        receipt = validator.validate(draft, self.CLASSIFIED, shadows)
        self.assertEqual(receipt["result"], "PASS", receipt["errors"])

    def test_arena_candidate_without_shadow_stays_shadow(self):
        draft = drafter.generate(self.CLASSIFIED, {})
        modes = {e["site_id"]: e["mode"] for e in draft["entries"]}
        self.assertEqual(modes["arena-site"], "shadow")
        receipt = validator.validate(draft, self.CLASSIFIED, {})
        self.assertEqual(receipt["result"], "PASS", receipt["errors"])


class RunAdoptionTest(unittest.TestCase):
    def test_one_click_with_seed_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "run"
            summary = runner.run(ROOT / "ALLOCATION_SITES.json", out,
                                 ROOT / "ALLOCATION_POLICY.json")
            self.assertEqual(summary["result"], "PASS")
            self.assertTrue((out / "receipt.json").exists())
            self.assertTrue((out / "classification.json").exists())

    def test_one_click_generates_draft_and_shadow(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            trace = tmp / "trace.jsonl"
            trace.write_text("\n".join('{"bytes":64,"alignment":16}' for _ in range(4096)),
                             encoding="utf-8")
            inventory = tmp / "sites.json"
            inventory.write_text(json.dumps({"schema_version": 1, "sites": [{
                "id": "batch", "kind": "heap_blocks", "shape": "fixed",
                "lifetime": "short_batch", "size_bytes": {"typical": 64},
                "alignment": 16, "calls_per_batch": 64}]}), encoding="utf-8")
            out = tmp / "run"
            summary = runner.run(inventory, out, None, [("batch", trace)])
            self.assertEqual(summary["result"], "PASS", summary["errors"])
            self.assertEqual(summary["shadows"], {"batch": "arena_recommended"})
            draft = json.loads((out / "policy-draft.json").read_text(encoding="utf-8"))
            self.assertEqual(draft["entries"][0]["mode"], "arena")

    def test_existing_out_dir_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(runner.RunError):
                runner.run(ROOT / "ALLOCATION_SITES.json", Path(tmp))

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "run"
            code = runner.main(["--inventory", str(ROOT / "ALLOCATION_SITES.json"),
                                "--policy", str(ROOT / "ALLOCATION_POLICY.json"),
                                "--out-dir", str(out)])
            self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
