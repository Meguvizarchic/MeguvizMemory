#!/usr/bin/env python3
"""Generate an allocation-policy draft from Layers 1 and 2.

Every machine-checkable field (mode, chunk, budget, shadow SHA-256) is
filled by this tool; a human only reviews entries and replaces the
generated rationale. Sites classified as arena candidates become arena
entries only when a matching arena_recommended shadow report is given;
otherwise they stay in shadow mode so the draft still validates.

Usage:
    python tools/generate_policy_draft.py \
        --classification classification.json \
        [--shadow site=shadow.json ...] --output policy.json
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

SCHEMA_VERSION = 1
MODE_BY_CLASSIFICATION = {
    "system_candidate": "system",
    "copyto_candidate": "copyto",
    "import_candidate": "import",
    "excluded": "excluded",
    "uncertain": "shadow",
}


class DraftError(ValueError):
    pass


def generate(classification_payload, shadows: dict) -> dict:
    """shadows maps site_id -> (file_sha256, parsed report)."""
    if not isinstance(classification_payload, dict) or \
            classification_payload.get("schema_version") != SCHEMA_VERSION:
        raise DraftError("classification schema_version must be 1; run "
                         "tools/classify_allocation_sites.py first")
    rows = classification_payload.get("sites")
    if not isinstance(rows, list) or not rows:
        raise DraftError("classification sites must be a non-empty list")
    entries = []
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            raise DraftError("classification row missing id")
        site, classification = row["id"], row.get("classification")
        reasons = ",".join(row.get("reasons") or []) or "no reason recorded"
        base = {"site_id": site, "rollback": "system"}
        if classification == "arena_candidate":
            evidence = shadows.get(site)
            report = evidence[1] if evidence else None
            if isinstance(report, dict) and report.get("verdict") == "arena_recommended":
                chunk = report.get("recommended_chunk_bytes") or \
                    (report.get("config") or {}).get("chunk_bytes")
                peak = None
                for sweep_row in report.get("chunk_sweep") or []:
                    if isinstance(sweep_row, dict) and sweep_row.get("chunk_bytes") == chunk:
                        peak = sweep_row.get("backing_peak_bytes")
                        break
                if peak is None:
                    peak = (report.get("arena") or {}).get("backing_peak_bytes")
                if not isinstance(chunk, int) or not isinstance(peak, int):
                    raise DraftError(f"{site}: shadow report lacks chunk or arena peak")
                base.update({
                    "mode": "arena", "chunk_bytes": chunk,
                    "budget_bytes": max(peak, chunk),
                    "shadow_evidence_sha256": evidence[0],
                    "rationale": ("Auto-generated: Layer 1 arena candidate with an "
                                  "arena_recommended shadow report. Review before adoption."),
                })
            else:
                base.update({
                    "mode": "shadow",
                    "rationale": ("Auto-generated: arena candidate without an "
                                  "arena_recommended shadow report yet; collect a trace first."),
                })
        elif classification in MODE_BY_CLASSIFICATION:
            base.update({
                "mode": MODE_BY_CLASSIFICATION[classification],
                "rationale": f"Auto-generated from classification {classification} ({reasons}).",
            })
        else:
            raise DraftError(f"{site}: unknown classification {classification!r}")
        entries.append(base)
    return {
        "schema_version": SCHEMA_VERSION,
        "scope": ("Auto-generated draft; review every entry before use. "
                  "Switches no runtime path and qualifies no performance."),
        "draft": True,
        "entries": entries,
    }


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--classification", required=True, type=Path)
    parser.add_argument("--shadow", action="append", default=[],
                        help="site_id=path to a shadow_estimate report")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        shadows = {}
        for item in args.shadow:
            site, sep, name = item.partition("=")
            if not sep or not site or not name:
                raise DraftError(f"--shadow expects site=path, got {item!r}")
            raw = Path(name).read_bytes()
            shadows[site] = (hashlib.sha256(raw).hexdigest(),
                             json.loads(raw.decode("utf-8-sig")))
        payload = json.loads(args.classification.read_text(encoding="utf-8-sig"))
        draft = generate(payload, shadows)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, DraftError) as failure:
        print(f"generate_policy_draft: {failure}", file=sys.stderr)
        return 1
    text = json.dumps(draft, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
