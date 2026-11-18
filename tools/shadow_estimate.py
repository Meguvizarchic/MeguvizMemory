#!/usr/bin/env python3
"""Layer 2: shadow-estimate System vs Arena from an allocation trace.

The trace is a JSONL stream of allocation requests ({"bytes": N,
"alignment": A}) grouped into epochs by {"op": "reset"} rows. Every
allocation in an epoch is treated as live until the epoch ends, which
matches the staged arena cost model: released arena space is retained
until reset, while System returns each backing independently.

Arena placement replays the core first-fit bump rule from
src/mgmem.cpp: scan existing chunks in order, skip a chunk whose
backing alignment is below the request alignment, place at
round_up(used, alignment) when it fits, otherwise publish a new chunk
of max(chunk_bytes, bytes). --prepared-alignment models a prepared
chunk (its backing alignment is fixed up front, as in the
prepared-single and small-cost prepared profiles).

Only provider counts and logical backing are estimated. Latency is
deliberately not invented: latency_estimate stays null and the verdict
combines provider saving, backing growth and same-shape ratio. This is
a model, not a measurement, and never qualifies performance.

Usage:
    python tools/shadow_estimate.py --trace trace.jsonl --chunk 4096 \
        --output shadow.json
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

SCHEMA_VERSION = 1
WINDOW_ALLOCATIONS = 64
MIN_SAME_SHAPE = 0.90
MIN_PROVIDER_SAVING = 0.50
MAX_BACKING_GROWTH = 0.05
REQUIRED_CONSECUTIVE_WINDOWS = 3

DEFAULT_THRESHOLDS = {
    "min_same_shape": MIN_SAME_SHAPE,
    "min_provider_saving": MIN_PROVIDER_SAVING,
    "max_backing_growth": MAX_BACKING_GROWTH,
    "window_allocations": WINDOW_ALLOCATIONS,
    "required_consecutive_windows": REQUIRED_CONSECUTIVE_WINDOWS,
}
PRESETS = {
    "balanced": {},
    "conservative": {"min_same_shape": 0.95, "min_provider_saving": 0.60,
                     "max_backing_growth": 0.03},
    "exploratory": {"min_same_shape": 0.80, "min_provider_saving": 0.40,
                    "max_backing_growth": 0.10},
}


def resolve_thresholds(preset=None, overrides=None) -> dict:
    merged = dict(DEFAULT_THRESHOLDS)
    if preset is not None:
        if preset not in PRESETS:
            raise TraceError(f"unknown preset {preset!r}; expected one of {sorted(PRESETS)}")
        merged.update(PRESETS[preset])
    if overrides is not None:
        if not isinstance(overrides, dict):
            raise TraceError("threshold overrides must be a JSON object, "
                             f"got {type(overrides).__name__}")
        unknown = set(overrides) - set(DEFAULT_THRESHOLDS)
        if unknown:
            raise TraceError(f"unknown threshold names {sorted(unknown)}; "
                             f"expected subset of {sorted(DEFAULT_THRESHOLDS)}")
        merged.update(overrides)
    for name, limit in (("min_same_shape", 1.0), ("min_provider_saving", 1.0),
                        ("max_backing_growth", 10.0)):
        value = merged[name]
        if not isinstance(value, (int, float)) or isinstance(value, bool) \
                or not 0.0 <= value <= limit:
            raise TraceError(f"threshold {name} must be a number in [0, {limit}]")
    for name in ("window_allocations", "required_consecutive_windows"):
        if not _is_int(merged[name]) or merged[name] <= 0:
            raise TraceError(f"threshold {name} must be a positive int")
    return merged

# Recorded host medians kept as context only; never used as an estimate.
REFERENCE_HOST_RATIOS = {
    "arena_fixed_allocation_p50": [0.53379, 0.32488],
    "arena_mixed_allocation_p50": [1.00641, 0.99436],
    "lease_copyto_64kib": 0.32531,
    "source": "docs/PERFORMANCE_PATHS.md (synthetic host, not a qualification)",
}


class TraceError(ValueError):
    pass


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def parse_trace(text: str) -> list:
    """Return epochs of events.

    An event is ("alloc", id_or_None, bytes, alignment) or ("free", id).
    Legacy rows {"bytes": ...} are allocations held until the epoch
    ends; v2 rows use {"op": "alloc"/"free", "id": N} so lifetimes are
    explicit and the stream lifetime model can be estimated.
    """
    epochs, current, live_ids = [], [], set()
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as failure:
            raise TraceError(f"line {line_number}: invalid JSON: {failure}") from failure
        if not isinstance(row, dict):
            raise TraceError(f"line {line_number}: row must be an object")
        op = row.get("op")
        if op == "reset":
            epochs.append(current)
            current, live_ids = [], set()
            continue
        if op == "free":
            ident = row.get("id")
            if not (_is_int(ident) and ident > 0):
                raise TraceError(f"line {line_number}: free id must be a positive int")
            if ident not in live_ids:
                raise TraceError(
                    f"line {line_number}: free of unknown or already-freed id {ident}")
            live_ids.discard(ident)
            current.append(("free", ident))
            continue
        if op not in (None, "alloc"):
            raise TraceError(f"line {line_number}: unknown op {op!r}; "
                             "expected alloc, free or reset")
        size, alignment = row.get("bytes"), row.get("alignment", 16)
        if not (_is_int(size) and size > 0):
            raise TraceError(f"line {line_number}: bytes must be a positive int")
        if not (_is_int(alignment) and alignment > 0 and alignment & (alignment - 1) == 0):
            raise TraceError(f"line {line_number}: alignment must be a power of two")
        ident = row.get("id")
        if ident is not None:
            if not (_is_int(ident) and ident > 0):
                raise TraceError(f"line {line_number}: alloc id must be a positive int")
            if ident in live_ids:
                raise TraceError(f"line {line_number}: alloc id {ident} already live")
            live_ids.add(ident)
        current.append(("alloc", ident, size, alignment))
    epochs.append(current)
    epochs = [epoch for epoch in epochs
              if any(event[0] == "alloc" for event in epoch)]
    if not epochs:
        raise TraceError("trace contains no allocation requests")
    return epochs


def _normalize_epochs(epochs) -> list:
    """Accept event epochs and legacy [[(bytes, alignment), ...]] epochs."""
    normalized = []
    for epoch in epochs:
        events = []
        for item in epoch:
            if isinstance(item, tuple) and len(item) == 2 and _is_int(item[0]):
                events.append(("alloc", None, item[0], item[1]))
            else:
                events.append(item)
        normalized.append(events)
    return normalized


def _epoch_requests(epoch) -> list:
    return [(event[2], event[3]) for event in epoch if event[0] == "alloc"]


def _system_live_peak(epoch) -> int:
    """Peak live payload honouring explicit frees (stream model)."""
    live, total, peak, synthetic = {}, 0, 0, 0
    for event in epoch:
        if event[0] == "alloc":
            synthetic += 1
            live[event[1] if event[1] is not None else ("row", synthetic)] = event[2]
            total += event[2]
            peak = max(peak, total)
        else:
            total -= live.pop(event[1], 0)
    return peak


def _round_up(value: int, alignment: int) -> int:
    return -(-value // alignment) * alignment


def simulate_arena(requests, chunk_bytes: int, prepared_alignment=None) -> dict:
    """First-fit bump over retained chunks; returns provider/backing totals.

    Mirrors the core search lower bound: a request no smaller and no
    less aligned than the cached one resumes at the cached chunk, and
    publishing a new chunk invalidates the cache.
    """
    chunks = []  # each: [capacity, backing_alignment, used]
    provider_calls = 0
    cache_index = None
    cache_bytes = cache_alignment = 0
    for size, alignment in requests:
        start = 0
        if cache_index is not None and size >= cache_bytes and alignment >= cache_alignment:
            start = cache_index
        placed = False
        for index in range(start, len(chunks)):
            chunk = chunks[index]
            if chunk[1] < alignment:
                continue
            offset = _round_up(chunk[2], alignment)
            if offset + size <= chunk[0]:
                chunk[2] = offset + size
                cache_index, cache_bytes, cache_alignment = index, size, alignment
                placed = True
                break
        if not placed:
            capacity = max(chunk_bytes, size)
            backing_alignment = prepared_alignment or alignment
            chunks.append([capacity, backing_alignment, size])
            provider_calls += 1
            cache_index = None
    return {
        "provider_calls": provider_calls,
        "backing_bytes": sum(chunk[0] for chunk in chunks),
        "used_bytes": sum(chunk[2] for chunk in chunks),
    }


def _window_passes(requests, chunk_bytes, prepared_alignment, thresholds) -> bool:
    # A window judges shape stability and provider saving only. Backing
    # growth is a whole-trace gate: a 64-request window may fill just
    # part of one large chunk, which is not growth.
    if not requests:
        return False
    same_shape = Counter(requests).most_common(1)[0][1] / len(requests)
    arena = simulate_arena(requests, chunk_bytes, prepared_alignment)
    saving = 1.0 - arena["provider_calls"] / len(requests)
    return (same_shape >= thresholds["min_same_shape"]
            and saving >= thresholds["min_provider_saving"])


def estimate(epochs, chunk_bytes: int, prepared_alignment=None,
             budget_bytes=None, thresholds=None, preset=None) -> dict:
    if not (_is_int(chunk_bytes) and chunk_bytes > 0):
        raise TraceError("chunk_bytes must be a positive int")
    thresholds = resolve_thresholds(preset, thresholds)
    epochs = _normalize_epochs(epochs)
    epoch_requests = [_epoch_requests(epoch) for epoch in epochs]
    all_requests = [request for requests in epoch_requests for request in requests]
    total = len(all_requests)
    if total == 0:
        raise TraceError("trace contains no allocation requests")
    requested_bytes = sum(size for size, _ in all_requests)
    same_shape = Counter(all_requests).most_common(1)[0][1] / total

    # Backing peak is per epoch: reset rewinds the arena, so epochs do
    # not accumulate. Provider publications only happen in epoch 1 when
    # later epochs fit the retained chunks; model publications as the
    # maximum per-epoch chunk count instead of their sum.
    # Arena space is retained until reset, so frees do not lower its
    # backing; System is reported under both the staged model (every
    # allocation live until epoch end) and the stream model (explicit
    # frees honoured). Growth is judged against the stream peak.
    epoch_results = [simulate_arena(requests, chunk_bytes, prepared_alignment)
                     for requests in epoch_requests]
    arena_provider = max(row["provider_calls"] for row in epoch_results)
    arena_peak = max(row["backing_bytes"] for row in epoch_results)
    staged_peak = max(sum(size for size, _ in requests) for requests in epoch_requests)
    system_peak = max(_system_live_peak(epoch) for epoch in epochs)
    has_frees = any(event[0] == "free" for epoch in epochs for event in epoch)
    per_epoch_system_provider = max(len(requests) for requests in epoch_requests)
    provider_saving = 1.0 - arena_provider / per_epoch_system_provider
    backing_growth = arena_peak / system_peak - 1.0 if system_peak else 0.0
    peak_index = max(range(len(epochs)),
                     key=lambda i: epoch_results[i]["backing_bytes"])
    padding_bytes = arena_peak - sum(size for size, _ in epoch_requests[peak_index])

    window_size = thresholds["window_allocations"]
    windows = [all_requests[i:i + window_size]
               for i in range(0, total, window_size)]
    window_pass = [_window_passes(w, chunk_bytes, prepared_alignment, thresholds)
                   for w in windows]
    best_run = run = 0
    for passed in window_pass:
        run = run + 1 if passed else 0
        best_run = max(best_run, run)
    required = min(thresholds["required_consecutive_windows"], len(windows))
    windows_ok = best_run >= required

    reasons = []
    if budget_bytes is not None and arena_peak > budget_bytes:
        verdict = "system_recommended"
        reasons.append("arena_peak_exceeds_budget")
    elif (same_shape >= thresholds["min_same_shape"]
          and provider_saving >= thresholds["min_provider_saving"]
          and backing_growth <= thresholds["max_backing_growth"] and windows_ok):
        verdict = "arena_recommended"
        reasons.append("same_shape_provider_saving_within_backing_budget")
    elif backing_growth > thresholds["max_backing_growth"]:
        verdict = "system_recommended"
        reasons.append("arena_backing_growth_exceeds_limit")
    elif provider_saving < thresholds["min_provider_saving"]:
        verdict = "system_recommended"
        reasons.append("provider_saving_too_small")
    else:
        verdict = "uncertain"
        reasons.append("shape_or_window_evidence_insufficient")

    return {
        "schema_version": SCHEMA_VERSION,
        "tool": "shadow_estimate",
        "performance_qualified": False,
        "device_executed": False,
        "latency_estimate": None,
        "reference_host_ratios": REFERENCE_HOST_RATIOS,
        "config": {"chunk_bytes": chunk_bytes,
                   "prepared_alignment": prepared_alignment,
                   "budget_bytes": budget_bytes,
                   "preset": preset,
                   "thresholds": thresholds},
        "requests": total,
        "epochs": len(epochs),
        "requested_bytes": requested_bytes,
        "same_shape_ratio": same_shape,
        "lifetime_model": "explicit_free_events" if has_frees else "staged_all_live",
        "system": {"provider_calls_per_epoch_peak": per_epoch_system_provider,
                   "backing_peak_bytes": system_peak,
                   "staged_backing_peak_bytes": staged_peak},
        "arena": {"provider_calls_peak": arena_provider,
                  "backing_peak_bytes": arena_peak,
                  "padding_peak_bytes": padding_bytes},
        "provider_saving_ratio": provider_saving,
        "backing_growth_ratio": backing_growth,
        "windows": {"count": len(windows), "pass": window_pass,
                    "best_consecutive_pass": best_run, "required": required},
        "verdict": verdict,
        "reasons": reasons,
        "explanation": _explain(verdict, reasons, same_shape, provider_saving,
                                 backing_growth, best_run, required, thresholds)
                       + (" 本轨迹含显式 free，backing 按同时存活峰值（stream 口径）"
                          "对比；全批共存口径见 system.staged_backing_peak_bytes。"
                          if has_frees else ""),
        "counterfactual": None,
    }


def _explain(verdict, reasons, same_shape, saving, growth, best_run, required,
             thresholds) -> str:
    pct = lambda value: f"{value * 100:.1f}%"
    head = (f"同形率 {pct(same_shape)}（门槛 {pct(thresholds['min_same_shape'])}）、"
            f"provider 节省 {pct(saving)}（门槛 {pct(thresholds['min_provider_saving'])}）、"
            f"backing 增幅 {pct(growth)}（上限 {pct(thresholds['max_backing_growth'])}）、"
            f"连续通过窗口 {best_run}/{required}。")
    if verdict == "arena_recommended":
        tail = "满足全部推荐条件，建议进入策略与配对采样评估（本模型不构成性能资格）。"
    elif "arena_backing_growth_exceeds_limit" in reasons:
        tail = "backing 增幅超上限，推荐保持 System；可参考 chunk 扫描是否有更小增幅点。"
    elif "provider_saving_too_small" in reasons:
        tail = "provider 节省不足，arena 省不下足够发布次数，推荐保持 System。"
    elif "arena_peak_exceeds_budget" in reasons:
        tail = "arena 峰值超过给定预算，推荐保持 System 或提高预算后重估。"
    else:
        tail = ("形状或窗口证据不足，不推荐直接采用；注意 provider 节省不等于加速，"
                "需要真实轨迹或实测补证。")
    return head + tail


SWEEP_CHUNKS = tuple(1024 << i for i in range(9))  # 1 KiB .. 256 KiB


def attach_sweep(report: dict, epochs, prepared_alignment=None, budget_bytes=None,
                 thresholds=None, preset=None) -> dict:
    """Add a chunk sweep, a recommended chunk and a counterfactual line."""
    rows = []
    for chunk in SWEEP_CHUNKS:
        row = estimate(epochs, chunk, prepared_alignment, budget_bytes,
                       thresholds=thresholds, preset=preset)
        rows.append({
            "chunk_bytes": chunk,
            "verdict": row["verdict"],
            "provider_calls_peak": row["arena"]["provider_calls_peak"],
            "backing_peak_bytes": row["arena"]["backing_peak_bytes"],
            "backing_growth_ratio": row["backing_growth_ratio"],
            "provider_saving_ratio": row["provider_saving_ratio"],
        })
    report["chunk_sweep"] = rows
    passing = [row for row in rows if row["verdict"] == "arena_recommended"]
    report["recommended_chunk_bytes"] = passing[0]["chunk_bytes"] if passing else None
    if report["verdict"] != "arena_recommended" and passing:
        best = passing[0]
        report["counterfactual"] = (
            f"若改用 chunk={best['chunk_bytes']} B，判定为 arena_recommended"
            f"（provider {best['provider_calls_peak']} 次、backing 增幅 "
            f"{best['backing_growth_ratio'] * 100:.1f}%）。")
    elif report["verdict"] != "arena_recommended" and rows:
        best = min(rows, key=lambda row: row["backing_growth_ratio"])
        report["counterfactual"] = (
            f"扫描内无 chunk 达推荐条件；增幅最小为 chunk={best['chunk_bytes']} B "
            f"时 {best['backing_growth_ratio'] * 100:.1f}%（判定 {best['verdict']}）。")
    return report


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--chunk", type=int, default=4096)
    parser.add_argument("--prepared-alignment", type=int)
    parser.add_argument("--budget", type=int)
    parser.add_argument("--site", help="optional site id recorded in the report")
    parser.add_argument("--sweep", action="store_true",
                        help="also sweep chunk sizes and recommend one")
    parser.add_argument("--preset", choices=sorted(PRESETS))
    parser.add_argument("--thresholds", type=Path,
                        help="JSON object overriding individual thresholds")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        raw = args.trace.read_bytes()
        epochs = parse_trace(raw.decode("utf-8-sig"))
        overrides = None
        if args.thresholds:
            overrides = json.loads(args.thresholds.read_text(encoding="utf-8-sig"))
            if not isinstance(overrides, dict):
                raise TraceError("--thresholds file must contain a JSON object")
        report = estimate(epochs, args.chunk, args.prepared_alignment, args.budget,
                          thresholds=overrides, preset=args.preset)
        if args.sweep:
            report = attach_sweep(report, epochs, args.prepared_alignment, args.budget,
                                  thresholds=overrides, preset=args.preset)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, TraceError) as failure:
        print(f"shadow_estimate: {failure}", file=sys.stderr)
        return 1
    report["trace_sha256"] = hashlib.sha256(raw).hexdigest()
    if args.site:
        report["site"] = args.site
    text = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
