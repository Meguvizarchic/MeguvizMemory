#!/usr/bin/env python3
"""Compare a shadow prediction with a real domain snapshot.

The snapshot JSON carries mgmem_snapshot_v1 fields (at least
provider_allocation_count and peak_backing_bytes) captured from an
actual domain run. Relative errors above the tolerance are reported
as drift: the trace or chunk choice no longer describes reality and
the site should be re-estimated. This checks model fidelity only; it
does not turn the model into a measurement.

Usage:
    python tools/snapshot_feedback.py --shadow shadow.json \
        --snapshot snapshot.json [--mode arena] [--output feedback.json]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

TOLERANCE = 0.10


class FeedbackError(ValueError):
    pass


def _need_int(payload: dict, name: str, source: str) -> int:
    value = payload.get(name)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise FeedbackError(f"{source} field {name!r} must be a non-negative int")
    return value


def compare(shadow_report, snapshot, mode: str = "arena",
            tolerance: float = TOLERANCE) -> dict:
    if mode not in ("arena", "system"):
        raise FeedbackError("mode must be arena or system")
    if not isinstance(shadow_report, dict) or not isinstance(snapshot, dict):
        raise FeedbackError("shadow report and snapshot must be JSON objects")
    predicted = shadow_report.get(mode)
    if not isinstance(predicted, dict):
        raise FeedbackError(f"shadow report has no {mode!r} prediction block")
    if mode == "arena":
        pred_provider = _need_int(predicted, "provider_calls_peak", "shadow.arena")
    else:
        pred_provider = _need_int(
            predicted, "provider_calls_per_epoch_peak", "shadow.system")
    pred_backing = _need_int(predicted, "backing_peak_bytes", f"shadow.{mode}")
    real_provider = _need_int(snapshot, "provider_allocation_count", "snapshot")
    real_backing = _need_int(snapshot, "peak_backing_bytes", "snapshot")

    def rel_error(pred: int, real: int):
        if real == 0:
            return None if pred == 0 else 1.0
        return abs(pred - real) / real

    provider_error = rel_error(pred_provider, real_provider)
    backing_error = rel_error(pred_backing, real_backing)
    drift = [name for name, err in (("provider", provider_error), ("backing", backing_error))
             if err is not None and err > tolerance]
    if drift:
        explanation = (f"预测漂移：{'、'.join(drift)} 相对误差超过 {tolerance:.0%}，"
                       "轨迹或 chunk 选择已不描述当前真实域，应重新采集并估算。")
    else:
        explanation = "预测与真实快照在容差内，影子模型对该域仍具描述力。"
    return {
        "schema_version": 1,
        "tool": "snapshot_feedback",
        "performance_qualified": False,
        "mode": mode,
        "tolerance": tolerance,
        "site": shadow_report.get("site"),
        "predicted": {"provider_calls": pred_provider, "backing_peak_bytes": pred_backing},
        "observed": {"provider_allocation_count": real_provider,
                     "peak_backing_bytes": real_backing},
        "relative_error": {"provider": provider_error, "backing": backing_error},
        "drift": drift,
        "result": "DRIFT" if drift else "WITHIN_TOLERANCE",
        "explanation": explanation,
    }


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shadow", required=True, type=Path)
    parser.add_argument("--snapshot", required=True, type=Path)
    parser.add_argument("--mode", choices=("arena", "system"), default="arena")
    parser.add_argument("--tolerance", type=float, default=TOLERANCE)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        report = compare(
            json.loads(args.shadow.read_text(encoding="utf-8-sig")),
            json.loads(args.snapshot.read_text(encoding="utf-8-sig")),
            args.mode, args.tolerance)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, FeedbackError) as failure:
        print(f"snapshot_feedback: {failure}", file=sys.stderr)
        return 1
    text = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
