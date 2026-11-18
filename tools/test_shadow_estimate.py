#!/usr/bin/env python3
"""Offline tests for the Layer 2 shadow estimator.

Oracle cases come from recorded benchmark contracts, not from this
model: bench/ARENA_LOOKUP.md states 128 fixed 64-byte requests use two
4096-byte chunks and the prepared-single profile fits 32 mixed
requests in one prepared chunk; bench/ARENA_SMALL_COST.md states the
mixed batch64 needs two prepared chunks.
"""
import json
from pathlib import Path
import tempfile
import unittest

import shadow_estimate as shadow

FIXED = (64, 16)
MIXED_CYCLE = [(32, 8), (64, 16), (96, 32), (128, 64)]


def mixed(count):
    return [MIXED_CYCLE[i % 4] for i in range(count)]


class ShadowTest(unittest.TestCase):
    def test_oracle_fixed_128_uses_two_chunks(self):
        result = shadow.simulate_arena([FIXED] * 128, 4096)
        self.assertEqual(result["provider_calls"], 2)
        self.assertEqual(result["backing_bytes"], 8192)

    def test_oracle_prepared_mixed_32_one_chunk_64_two_chunks(self):
        one = shadow.simulate_arena(mixed(32), 4096, prepared_alignment=64)
        two = shadow.simulate_arena(mixed(64), 4096, prepared_alignment=64)
        self.assertEqual(one["provider_calls"], 1)
        self.assertEqual(two["provider_calls"], 2)

    def test_unprepared_chunk_alignment_comes_from_first_request(self):
        # First request needs only 8-byte alignment, so the 64-byte
        # aligned request cannot reuse that chunk in the core rule.
        result = shadow.simulate_arena([(32, 8), (128, 64)], 4096)
        self.assertEqual(result["provider_calls"], 2)

    def test_same_shape_large_batch_recommends_arena(self):
        report = shadow.estimate([[FIXED] * 4096], 4096)
        self.assertEqual(report["verdict"], "arena_recommended")
        self.assertEqual(report["arena"]["provider_calls_peak"], 64)
        self.assertAlmostEqual(report["provider_saving_ratio"], 1 - 64 / 4096)
        self.assertEqual(report["backing_growth_ratio"], 0.0)
        self.assertIsNone(report["latency_estimate"])

    def test_mixed_shape_not_recommended(self):
        report = shadow.estimate([mixed(4096)], 4096, prepared_alignment=64)
        self.assertNotEqual(report["verdict"], "arena_recommended")
        self.assertLess(report["same_shape_ratio"], 0.90)

    def test_small_input_chunk_overhead_recommends_system(self):
        # One 448-byte request in a 64 KiB chunk mirrors the recorded
        # small-input regression in docs/BENCHMARK_RESULTS.md.
        report = shadow.estimate([[(448, 16)]], 65536)
        self.assertEqual(report["verdict"], "system_recommended")
        self.assertGreater(report["backing_growth_ratio"], 100.0)

    def test_budget_breach_recommends_system(self):
        report = shadow.estimate([[FIXED] * 4096], 4096, budget_bytes=131072)
        self.assertEqual(report["verdict"], "system_recommended")
        self.assertIn("arena_peak_exceeds_budget", report["reasons"])

    def test_epochs_do_not_accumulate_backing(self):
        report = shadow.estimate([[FIXED] * 64, [FIXED] * 64], 4096)
        self.assertEqual(report["epochs"], 2)
        self.assertEqual(report["arena"]["backing_peak_bytes"], 4096)
        self.assertEqual(report["arena"]["provider_calls_peak"], 1)

    def test_trace_parsing_and_cli(self):
        rows = [{"bytes": 64, "alignment": 16}] * 64 + [{"op": "reset"}]
        with tempfile.TemporaryDirectory() as tmp:
            trace = Path(tmp) / "trace.jsonl"
            trace.write_text("\n".join(json.dumps(r) for r in rows), encoding="utf-8")
            out = Path(tmp) / "shadow.json"
            code = shadow.main(["--trace", str(trace), "--chunk", "4096",
                                "--site", "demo", "--output", str(out)])
            self.assertEqual(code, 0)
            report = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(report["site"], "demo")
            self.assertEqual(report["requests"], 64)
            self.assertEqual(len(report["trace_sha256"]), 64)

    def test_invalid_trace_rejected(self):
        for bad in ('{"bytes": 0}\n', '{"bytes": 64, "alignment": 3}\n',
                    '{"bytes": true}\n', 'not json\n', '\n'):
            with self.assertRaises(shadow.TraceError):
                shadow.parse_trace(bad)


if __name__ == "__main__":
    unittest.main()
