#!/usr/bin/env python3
"""Offline tests for stage 2: trace v2 lifetimes, generator, feedback."""
import json
from pathlib import Path
import tempfile
import unittest

import make_trace
import shadow_estimate as shadow
import snapshot_feedback as feedback


def jsonl(rows):
    return "\n".join(json.dumps(r) for r in rows) + "\n"


class LifetimeTest(unittest.TestCase):
    def test_stream_peak_is_one_allocation(self):
        epochs = shadow.parse_trace(jsonl(make_trace.rows("stream", 4096)))
        report = shadow.estimate(epochs, 4096)
        self.assertEqual(report["lifetime_model"], "explicit_free_events")
        self.assertEqual(report["system"]["backing_peak_bytes"], 64)
        self.assertEqual(report["system"]["staged_backing_peak_bytes"], 262144)
        # Arena retains everything until reset, so against the stream
        # peak it can never look good on backing.
        self.assertEqual(report["verdict"], "system_recommended")

    def test_staged_v2_matches_legacy_fixed(self):
        staged = shadow.estimate(shadow.parse_trace(jsonl(make_trace.rows("staged", 4096))), 4096)
        legacy = shadow.estimate(shadow.parse_trace(jsonl(make_trace.rows("fixed", 4096))), 4096)
        self.assertEqual(staged["system"]["backing_peak_bytes"],
                         legacy["system"]["backing_peak_bytes"])
        self.assertEqual(staged["arena"], legacy["arena"])
        self.assertEqual(legacy["lifetime_model"], "staged_all_live")

    def test_free_errors_are_named_with_lines(self):
        for text in ('{"op":"free","id":1}\n',
                     '{"op":"alloc","id":1,"bytes":64}\n{"op":"alloc","id":1,"bytes":64}\n',
                     '{"op":"alloc","id":1,"bytes":64}\n{"op":"free","id":1}\n{"op":"free","id":1}\n',
                     '{"op":"warp","bytes":64}\n'):
            with self.assertRaises(shadow.TraceError) as caught:
                shadow.parse_trace(text)
            self.assertIn("line", str(caught.exception))

    def test_id_reuse_after_free_and_across_reset(self):
        text = ('{"op":"alloc","id":1,"bytes":64}\n{"op":"free","id":1}\n'
                '{"op":"alloc","id":1,"bytes":64}\n{"op":"reset"}\n'
                '{"op":"alloc","id":1,"bytes":64}\n')
        epochs = shadow.parse_trace(text)
        self.assertEqual(len(epochs), 2)


class GeneratorTest(unittest.TestCase):
    def test_patterns_round_trip_through_parser(self):
        for pattern in make_trace.PATTERNS:
            epochs = shadow.parse_trace(jsonl(make_trace.rows(pattern, 128)))
            report = shadow.estimate(epochs, 4096, prepared_alignment=64)
            self.assertEqual(report["requests"], 128)

    def test_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "t.jsonl"
            self.assertEqual(make_trace.main(
                ["--pattern", "stream", "--count", "8", "--output", str(out)]), 0)
            self.assertEqual(len(out.read_text(encoding="utf-8").splitlines()), 16)


class FeedbackTest(unittest.TestCase):
    SHADOW = {"site": "s", "arena": {"provider_calls_peak": 64,
                                     "backing_peak_bytes": 262144}}

    def test_within_tolerance(self):
        result = feedback.compare(self.SHADOW, {
            "provider_allocation_count": 66, "peak_backing_bytes": 270336})
        self.assertEqual(result["result"], "WITHIN_TOLERANCE")

    def test_drift_named(self):
        result = feedback.compare(self.SHADOW, {
            "provider_allocation_count": 200, "peak_backing_bytes": 262144})
        self.assertEqual(result["result"], "DRIFT")
        self.assertEqual(result["drift"], ["provider"])
        self.assertIn("漂移", result["explanation"])

    def test_missing_fields_rejected_with_hint(self):
        with self.assertRaises(feedback.FeedbackError) as caught:
            feedback.compare(self.SHADOW, {"peak_backing_bytes": 1})
        self.assertIn("provider_allocation_count", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
