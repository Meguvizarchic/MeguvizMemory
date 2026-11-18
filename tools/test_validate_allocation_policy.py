#!/usr/bin/env python3
"""Offline tests for the Layer 3 policy validator and the full chain."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import classify_allocation_sites as classifier
import shadow_estimate as shadow
import validate_allocation_policy as validator

ROOT = Path(__file__).resolve().parents[1]


def classification(rows):
    return {"schema_version": 1,
            "sites": [{"id": site, "classification": kind} for site, kind in rows]}


def policy(entries):
    return {"schema_version": 1, "entries": entries}


def entry(site, mode, **extra):
    base = {"site_id": site, "mode": mode, "rollback": "system", "rationale": "r"}
    base.update(extra)
    return base


class ValidatorTest(unittest.TestCase):
    def test_seed_policy_passes_against_seed_inventory(self):
        inventory = json.loads((ROOT / "ALLOCATION_SITES.json").read_text(encoding="utf-8"))
        classified = classifier.classify_inventory(inventory)
        seed = json.loads((ROOT / "ALLOCATION_POLICY.json").read_text(encoding="utf-8"))
        receipt = validator.validate(seed, classified, {})
        self.assertEqual(receipt["result"], "PASS", receipt["errors"])
        self.assertEqual(len(receipt["checked"]), 10)
        self.assertEqual(receipt["uncovered_sites_default_system"], [])

    def test_uncovered_site_defaults_to_system_not_error(self):
        receipt = validator.validate(policy([]), classification([("a", "uncertain")]), {})
        self.assertEqual(receipt["result"], "PASS")
        self.assertEqual(receipt["uncovered_sites_default_system"], ["a"])

    def test_arena_requires_shadow_evidence_with_matching_sha(self):
        classified = classification([("s", "arena_candidate")])
        report = shadow.estimate([[(64, 16)] * 4096], 4096)
        self.assertEqual(report["verdict"], "arena_recommended")
        raw = (json.dumps(report) + "\n").encode()
        sha = hashlib.sha256(raw).hexdigest()
        good = entry("s", "arena", budget_bytes=262144, chunk_bytes=4096,
                     shadow_evidence_sha256=sha)
        receipt = validator.validate(policy([good]), classified, {"s": (sha, report)})
        self.assertEqual(receipt["result"], "PASS", receipt["errors"])
        bad_sha = dict(good, shadow_evidence_sha256="0" * 64)
        receipt = validator.validate(policy([bad_sha]), classified, {"s": (sha, report)})
        self.assertEqual(receipt["result"], "FAIL")
        self.assertEqual(receipt["errors"][0]["reason"], "shadow_evidence_sha_mismatch")
        receipt = validator.validate(policy([good]), classified, {})
        self.assertEqual(receipt["errors"][0]["reason"], "arena_missing_shadow_evidence")

    def test_arena_with_non_arena_shadow_verdict_fails(self):
        classified = classification([("s", "arena_candidate")])
        report = shadow.estimate([[(448, 16)]], 65536)
        raw = (json.dumps(report) + "\n").encode()
        sha = hashlib.sha256(raw).hexdigest()
        bad = entry("s", "arena", budget_bytes=65536, chunk_bytes=65536,
                    shadow_evidence_sha256=sha)
        receipt = validator.validate(policy([bad]), classified, {"s": (sha, report)})
        self.assertEqual(receipt["errors"][0]["reason"], "shadow_verdict_not_arena")

    def test_fail_closed_mismatches(self):
        cases = [
            (entry("s", "arena", budget_bytes=4096, chunk_bytes=4096),
             classification([("s", "uncertain")]), "classification_mismatch"),
            (entry("s", "system"), classification([("s", "excluded")]),
             "excluded_site_must_not_adopt"),
            (entry("ghost", "system"), classification([("s", "system_candidate")]),
             "site_not_in_classification"),
            (entry("s", "copyto"), classification([("s", "system_candidate")]),
             "classification_mismatch"),
        ]
        for bad_entry, classified, reason in cases:
            receipt = validator.validate(policy([bad_entry]), classified, {})
            self.assertEqual(receipt["result"], "FAIL")
            self.assertEqual(receipt["errors"][0]["reason"], reason)

    def test_malformed_policy_rejected(self):
        for bad in ({"schema_version": 2, "entries": []},
                    policy([entry("s", "warp")]),
                    policy([entry("s", "system"), entry("s", "system")]),
                    policy([{**entry("s", "system"), "rationale": ""}]),
                    policy([{**entry("s", "system"), "rollback": "arena"}])):
            with self.assertRaises(validator.PolicyError):
                validator.validate(bad, classification([("s", "system_candidate")]), {})

    def test_full_cli_chain(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            classified_path = tmp / "classification.json"
            receipt_path = tmp / "receipt.json"
            self.assertEqual(classifier.main(
                ["--inventory", str(ROOT / "ALLOCATION_SITES.json"),
                 "--output", str(classified_path)]), 0)
            self.assertEqual(validator.main(
                ["--policy", str(ROOT / "ALLOCATION_POLICY.json"),
                 "--classification", str(classified_path),
                 "--output", str(receipt_path)]), 0)
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            self.assertEqual(receipt["result"], "PASS")
            self.assertFalse(receipt["performance_qualified"])


if __name__ == "__main__":
    unittest.main()
