#!/usr/bin/env python3
"""Generate sample allocation traces for shadow_estimate trials.

Patterns mirror the recorded benchmark shapes so a new user can get a
first verdict without hand-writing JSONL:
- fixed:  N legacy rows of 64 bytes / alignment 16 (held to epoch end)
- mixed:  N rows cycling 32/64/96/128 bytes at alignment 8/16/32/64
- stream: N v2 alloc/free pairs of 64 bytes (one live at a time)
- staged: N v2 allocs then all frees (whole batch live together)

Usage: python tools/make_trace.py --pattern stream --count 4096 --output t.jsonl
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

PATTERNS = ("fixed", "mixed", "stream", "staged")
MIXED = ((32, 8), (64, 16), (96, 32), (128, 64))


def rows(pattern: str, count: int) -> list:
    if count <= 0:
        raise ValueError("count must be positive")
    if pattern == "fixed":
        return [{"bytes": 64, "alignment": 16}] * count
    if pattern == "mixed":
        return [{"bytes": MIXED[i % 4][0], "alignment": MIXED[i % 4][1]}
                for i in range(count)]
    if pattern == "stream":
        out = []
        for i in range(1, count + 1):
            out.append({"op": "alloc", "id": i, "bytes": 64, "alignment": 16})
            out.append({"op": "free", "id": i})
        return out
    if pattern == "staged":
        allocs = [{"op": "alloc", "id": i, "bytes": 64, "alignment": 16}
                  for i in range(1, count + 1)]
        return allocs + [{"op": "free", "id": i} for i in range(1, count + 1)]
    raise ValueError(f"unknown pattern {pattern!r}; expected one of {PATTERNS}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pattern", required=True, choices=PATTERNS)
    parser.add_argument("--count", type=int, default=4096)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        lines = [json.dumps(row, separators=(",", ":")) for row in rows(args.pattern, args.count)]
    except ValueError as failure:
        print(f"make_trace: {failure}", file=sys.stderr)
        return 1
    text = "\n".join(lines) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
