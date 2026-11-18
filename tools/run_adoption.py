#!/usr/bin/env python3
"""One-click adoption chain: classify, shadow-estimate, validate.

Runs Layer 1 on the inventory, Layer 2 (with chunk sweep) for every
--trace site, then either validates the given policy or generates a
draft policy and validates that. All artifacts land in a new output
directory; the terminal gets a short human summary, full JSON stays on
disk. Nothing here switches a runtime path or qualifies performance.

Usage:
    python tools/run_adoption.py --inventory ALLOCATION_SITES.json \
        [--policy ALLOCATION_POLICY.json] \
        [--trace site=trace.jsonl ...] --out-dir build/adoption-run
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

import classify_allocation_sites as classifier
import generate_policy_draft as drafter
import shadow_estimate as shadow
import validate_allocation_policy as validator


class RunError(ValueError):
    pass


def _write_json(path: Path, payload) -> bytes:
    raw = (json.dumps(payload, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    path.write_bytes(raw)
    return raw


def run(inventory_path: Path, out_dir: Path, policy_path=None, traces=(),
        chunk: int = 4096, prepared_alignment=None, budget=None,
        thresholds=None, preset=None) -> dict:
    if out_dir.exists():
        raise RunError(f"output directory already exists: {out_dir}; pick a new one")
    shadow.resolve_thresholds(preset, thresholds)  # validate even without traces
    inventory = json.loads(inventory_path.read_text(encoding="utf-8-sig"))
    classified = classifier.classify_inventory(inventory)
    out_dir.mkdir(parents=True)
    try:
        _write_json(out_dir / "classification.json", classified)
        shadows = {}
        for site, trace_path in traces:
            raw = Path(trace_path).read_bytes()
            epochs = shadow.parse_trace(raw.decode("utf-8-sig"))
            report = shadow.estimate(epochs, chunk, prepared_alignment, budget,
                                     thresholds=thresholds, preset=preset)
            report = shadow.attach_sweep(report, epochs, prepared_alignment, budget,
                                         thresholds=thresholds, preset=preset)
            report["trace_sha256"] = hashlib.sha256(raw).hexdigest()
            report["site"] = site
            written = _write_json(out_dir / f"shadow-{site}.json", report)
            shadows[site] = (hashlib.sha256(written).hexdigest(), report)
        if policy_path is not None:
            policy = json.loads(Path(policy_path).read_text(encoding="utf-8-sig"))
            policy_source = str(policy_path)
        else:
            policy = drafter.generate(classified, shadows)
            _write_json(out_dir / "policy-draft.json", policy)
            policy_source = "generated draft"
        receipt = validator.validate(policy, classified, shadows)
        receipt["policy_source"] = policy_source
        _write_json(out_dir / "receipt.json", receipt)
        summary = {
            "tool": "run_adoption",
            "result": receipt["result"],
            "classification_summary": classified["summary"],
            "shadows": {site: report["verdict"] for site, (_, report) in shadows.items()},
            "policy_source": policy_source,
            "checked": len(receipt["checked"]),
            "errors": receipt["errors"],
            "error_summary": receipt.get("error_summary", {}),
            "warnings": receipt.get("warnings", []),
            "uncovered_default_system": receipt["uncovered_sites_default_system"],
            "out_dir": str(out_dir),
        }
        _write_json(out_dir / "summary.json", summary)
        return summary
    except Exception as failure:
        # Mark the partial directory so nobody mistakes it for a result;
        # rerun into a new directory after fixing the cause.
        try:
            _write_json(out_dir / "INCOMPLETE.json",
                        {"complete": False, "error": str(failure),
                         "hint": "partial artifacts only; rerun into a new --out-dir"})
        except OSError:
            pass
        raise


def _parse_traces(items):
    traces = []
    for item in items:
        site, sep, name = item.partition("=")
        if not sep or not site or not name:
            raise RunError(f"--trace expects site=path, got {item!r}")
        traces.append((site, Path(name)))
    return traces


def print_summary(summary: dict) -> None:
    print(f"接入判断完成：{summary['result']}（策略来源：{summary['policy_source']}）")
    parts = "、".join(f"{name} {count}" for name, count in
                      sorted(summary["classification_summary"].items()))
    print(f"分类：{parts}")
    for site, verdict in sorted(summary["shadows"].items()):
        print(f"影子 {site}：{verdict}")
    if summary["uncovered_default_system"]:
        print("未覆盖（默认 System）：" + "、".join(summary["uncovered_default_system"]))
    for reason, count in sorted(summary["error_summary"].items()):
        print(f"错误 {reason}：{count} 处（明细见 receipt.json）")
    for warning in summary.get("warnings", []):
        print(f"警告 {warning['site_id']}：{warning['reason']}"
              + (f"（review_by={warning['review_by']}）" if warning.get("review_by") else ""))
    print(f"产物目录：{summary['out_dir']}")


def main(argv=None) -> int:
    # Summaries contain Chinese; never let the console codepage decide.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--policy", type=Path)
    parser.add_argument("--trace", action="append", default=[], help="site_id=trace.jsonl")
    parser.add_argument("--chunk", type=int, default=4096)
    parser.add_argument("--prepared-alignment", type=int)
    parser.add_argument("--budget", type=int)
    parser.add_argument("--preset", choices=sorted(shadow.PRESETS))
    parser.add_argument("--thresholds", type=Path,
                        help="JSON object overriding individual thresholds")
    parser.add_argument("--out-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        overrides = None
        if args.thresholds:
            overrides = json.loads(args.thresholds.read_text(encoding="utf-8-sig"))
        summary = run(args.inventory, args.out_dir, args.policy,
                      _parse_traces(args.trace), args.chunk,
                      args.prepared_alignment, args.budget,
                      thresholds=overrides, preset=args.preset)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, RunError,
            classifier.InventoryError, shadow.TraceError,
            validator.PolicyError, drafter.DraftError) as failure:
        print(f"run_adoption: {failure}", file=sys.stderr)
        return 1
    print_summary(summary)
    return 0 if summary["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
