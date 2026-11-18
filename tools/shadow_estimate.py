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
    """Return epochs: a list of lists of (bytes, alignment) requests."""
    epochs, current = [], []
    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as failure:
            raise TraceError(f"line {line_number}: invalid JSON: {failure}") from failure
        if not isinstance(row, dict):
            raise TraceError(f"line {line_number}: row must be an object")
        if row.get("op") == "reset":
            epochs.append(current)
            current = []
            continue
        size, alignment = row.get("bytes"), row.get("alignment", 16)
        if not (_is_int(size) and size > 0):
            raise TraceError(f"line {line_number}: bytes must be a positive int")
        if not (_is_int(alignment) and alignment > 0 and alignment & (alignment - 1) == 0):
            raise TraceError(f"line {line_number}: alignment must be a power of two")
        current.append((size, alignment))
    epochs.append(current)
    epochs = [epoch for epoch in epochs if epoch]
    if not epochs:
        raise TraceError("trace contains no allocation requests")
    return epochs


def _round_up(value: int, alignment: int) -> int:
    return -(-value // alignment) * alignment


def simulate_arena(requests, chunk_bytes: int, prepared_alignment=None) -> dict:
    """First-fit bump over retained chunks; returns provider/backing totals."""
    chunks = []  # each: [capacity, backing_alignment, used]
    provider_calls = 0
    for size, alignment in requests:
        placed = False
        for chunk in chunks:
            if chunk[1] < alignment:
                continue
            offset = _round_up(chunk[2], alignment)
            if offset + size <= chunk[0]:
                chunk[2] = offset + size
                placed = True
                break
        if not placed:
            capacity = max(chunk_bytes, size)
            backing_alignment = prepared_alignment or alignment
            chunks.append([capacity, backing_alignment, size])
            provider_calls += 1
    return {
        "provider_calls": provider_calls,
        "backing_bytes": sum(chunk[0] for chunk in chunks),
        "used_bytes": sum(chunk[2] for chunk in chunks),
    }


def _window_passes(requests, chunk_bytes, prepared_alignment) -> bool:
    if not requests:
        return False
    same_shape = Counter(requests).most_common(1)[0][1] / len(requests)
    arena = simulate_arena(requests, chunk_bytes, prepared_alignment)
    system_backing = sum(size for size, _ in requests)
    saving = 1.0 - arena["provider_calls"] / len(requests)
    growth = arena["backing_bytes"] / system_backing - 1.0 if system_backing else 0.0
    return (same_shape >= MIN_SAME_SHAPE and saving >= MIN_PROVIDER_SAVING
            and growth <= MAX_BACKING_GROWTH)


def estimate(epochs, chunk_bytes: int, prepared_alignment=None,
             budget_bytes=None) -> dict:
    if not (_is_int(chunk_bytes) and chunk_bytes > 0):
        raise TraceError("chunk_bytes must be a positive int")
    all_requests = [request for epoch in epochs for request in epoch]
    total = len(all_requests)
    requested_bytes = sum(size for size, _ in all_requests)
    same_shape = Counter(all_requests).most_common(1)[0][1] / total

    # Backing peak is per epoch: reset rewinds the arena, so epochs do
    # not accumulate. Provider publications only happen in epoch 1 when
    # later epochs fit the retained chunks; model publications as the
    # maximum per-epoch chunk count instead of their sum.
    epoch_results = [simulate_arena(epoch, chunk_bytes, prepared_alignment)
                     for epoch in epochs]
    arena_provider = max(row["provider_calls"] for row in epoch_results)
    arena_peak = max(row["backing_bytes"] for row in epoch_results)
    system_peak = max(sum(size for size, _ in epoch) for epoch in epochs)
    per_epoch_system_provider = max(len(epoch) for epoch in epochs)
    provider_saving = 1.0 - arena_provider / per_epoch_system_provider
    backing_growth = arena_peak / system_peak - 1.0 if system_peak else 0.0
    peak_index = max(range(len(epochs)),
                     key=lambda i: epoch_results[i]["backing_bytes"])
    padding_bytes = arena_peak - sum(size for size, _ in epochs[peak_index])

    windows = [all_requests[i:i + WINDOW_ALLOCATIONS]
               for i in range(0, total, WINDOW_ALLOCATIONS)]
    window_pass = [_window_passes(w, chunk_bytes, prepared_alignment) for w in windows]
    best_run = run = 0
    for passed in window_pass:
        run = run + 1 if passed else 0
        best_run = max(best_run, run)
    required = min(REQUIRED_CONSECUTIVE_WINDOWS, len(windows))
    windows_ok = best_run >= required

    reasons = []
    if budget_bytes is not None and arena_peak > budget_bytes:
        verdict = "system_recommended"
        reasons.append("arena_peak_exceeds_budget")
    elif (same_shape >= MIN_SAME_SHAPE and provider_saving >= MIN_PROVIDER_SAVING
          and backing_growth <= MAX_BACKING_GROWTH and windows_ok):
        verdict = "arena_recommended"
        reasons.append("same_shape_provider_saving_within_backing_budget")
    elif backing_growth > MAX_BACKING_GROWTH:
        verdict = "system_recommended"
        reasons.append("arena_backing_growth_exceeds_limit")
    elif provider_saving < MIN_PROVIDER_SAVING:
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
                   "budget_bytes": budget_bytes},
        "requests": total,
        "epochs": len(epochs),
        "requested_bytes": requested_bytes,
        "same_shape_ratio": same_shape,
        "system": {"provider_calls_per_epoch_peak": per_epoch_system_provider,
                   "backing_peak_bytes": system_peak},
        "arena": {"provider_calls_peak": arena_provider,
                  "backing_peak_bytes": arena_peak,
                  "padding_peak_bytes": padding_bytes},
        "provider_saving_ratio": provider_saving,
        "backing_growth_ratio": backing_growth,
        "windows": {"count": len(windows), "pass": window_pass,
                    "best_consecutive_pass": best_run, "required": required},
        "verdict": verdict,
        "reasons": reasons,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--chunk", type=int, default=4096)
    parser.add_argument("--prepared-alignment", type=int)
    parser.add_argument("--budget", type=int)
    parser.add_argument("--site", help="optional site id recorded in the report")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        raw = args.trace.read_bytes()
        epochs = parse_trace(raw.decode("utf-8-sig"))
        report = estimate(epochs, args.chunk, args.prepared_alignment, args.budget)
    except (OSError, UnicodeDecodeError, TraceError) as failure:
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
