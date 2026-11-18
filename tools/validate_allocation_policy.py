#!/usr/bin/env python3
"""Layer 3: validate an allocation policy against Layers 1 and 2.

A policy entry records, per site, the adopted mode plus its evidence.
The validator is fail-closed:

- a site with no entry defaults to System/dense and is listed as
  uncovered (not an error: absence means "not adopting Memory here");
- mode "arena" requires a Layer 1 arena_candidate classification AND
  a Layer 2 shadow report with verdict arena_recommended whose file
  SHA-256 matches shadow_evidence_sha256;
- mode "excluded" requires a Layer 1 excluded classification;
- modes "copyto"/"import" require the matching Layer 1 candidate;
- an uncertain site may only use mode "shadow" or "system";
- every entry needs a non-empty rationale and rollback "system".

Validation does not switch any runtime path or qualify performance;
the receipt is a freeze input for later paired sampling.

Usage:
    python tools/validate_allocation_policy.py \
        --policy ALLOCATION_POLICY.json \
        --classification classification.json \
        [--shadow site=shadow.json ...] [--output receipt.json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

SCHEMA_VERSION = 1
MODES = {"system", "arena", "copyto", "import", "excluded", "shadow"}
MODE_REQUIRES = {
    "arena": "arena_candidate",
    "copyto": "copyto_candidate",
    "import": "import_candidate",
    "excluded": "excluded",
}


class PolicyError(ValueError):
    pass


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def load_classification(payload) -> dict:
    if not isinstance(payload, dict) or payload.get("schema_version") != SCHEMA_VERSION:
        raise PolicyError("classification schema_version must be 1")
    sites = payload.get("sites")
    if not isinstance(sites, list):
        raise PolicyError("classification sites must be a list")
    result = {}
    for row in sites:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str):
            raise PolicyError("classification row missing id")
        if row["id"] in result:
            raise PolicyError(f"duplicate classification id {row['id']!r}")
        result[row["id"]] = row.get("classification")
    return result


def load_policy(payload) -> list:
    if not isinstance(payload, dict) or payload.get("schema_version") != SCHEMA_VERSION:
        raise PolicyError("policy schema_version must be 1")
    entries = payload.get("entries")
    if not isinstance(entries, list):
        raise PolicyError("policy entries must be a list")
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise PolicyError("each policy entry must be an object")
        site = entry.get("site_id")
        if not isinstance(site, str) or not site:
            raise PolicyError("policy entry site_id must be a non-empty string")
        if site in seen:
            raise PolicyError(f"duplicate policy entry for {site!r}")
        seen.add(site)
        if entry.get("mode") not in MODES:
            raise PolicyError(f"{site}: unknown mode {entry.get('mode')!r}")
        if not isinstance(entry.get("rationale"), str) or not entry["rationale"]:
            raise PolicyError(f"{site}: rationale must be a non-empty string")
        if entry.get("rollback", "system") != "system":
            raise PolicyError(f"{site}: rollback must be system")
        for name in ("budget_bytes", "chunk_bytes"):
            value = entry.get(name)
            if value is not None and not (_is_int(value) and value > 0):
                raise PolicyError(f"{site}: {name} must be a positive int")
    return entries


def validate(policy_payload, classification_payload, shadows: dict) -> dict:
    """shadows maps site_id -> (file_sha256, parsed report)."""
    entries = load_policy(policy_payload)
    classifications = load_classification(classification_payload)
    errors, checked = [], []
    covered = set()
    for entry in entries:
        site, mode = entry["site_id"], entry["mode"]
        covered.add(site)
        classification = classifications.get(site)
        if classification is None:
            errors.append({"site_id": site, "reason": "site_not_in_classification"})
            continue
        required = MODE_REQUIRES.get(mode)
        if required and classification != required:
            errors.append({"site_id": site, "reason": "classification_mismatch",
                           "mode": mode, "classification": classification})
            continue
        if classification == "uncertain" and mode not in ("shadow", "system"):
            errors.append({"site_id": site, "reason": "uncertain_site_needs_shadow_first"})
            continue
        if mode == "system" and classification == "excluded":
            errors.append({"site_id": site, "reason": "excluded_site_must_not_adopt"})
            continue
        if mode == "arena":
            chunk, budget = entry.get("chunk_bytes"), entry.get("budget_bytes")
            if chunk is None or budget is None or budget < chunk:
                errors.append({"site_id": site, "reason": "arena_needs_budget_ge_chunk"})
                continue
            evidence = shadows.get(site)
            expected_sha = entry.get("shadow_evidence_sha256")
            if evidence is None or not isinstance(expected_sha, str):
                errors.append({"site_id": site, "reason": "arena_missing_shadow_evidence"})
                continue
            file_sha, report = evidence
            if file_sha != expected_sha:
                errors.append({"site_id": site, "reason": "shadow_evidence_sha_mismatch"})
                continue
            if not isinstance(report, dict) or report.get("verdict") != "arena_recommended":
                errors.append({"site_id": site, "reason": "shadow_verdict_not_arena"})
                continue
            if report.get("site") not in (None, site):
                errors.append({"site_id": site, "reason": "shadow_site_mismatch"})
                continue
        checked.append({"site_id": site, "mode": mode, "classification": classification})
    uncovered = sorted(set(classifications) - covered)
    return {
        "schema_version": SCHEMA_VERSION,
        "tool": "validate_allocation_policy",
        "performance_qualified": False,
        "device_executed": False,
        "result": "PASS" if not errors else "FAIL",
        "checked": checked,
        "errors": errors,
        "uncovered_sites_default_system": uncovered,
    }


def _load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", required=True, type=Path)
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
                raise PolicyError(f"--shadow expects site=path, got {item!r}")
            raw = Path(name).read_bytes()
            shadows[site] = (hashlib.sha256(raw).hexdigest(),
                             json.loads(raw.decode("utf-8-sig")))
        receipt = validate(_load_json(args.policy), _load_json(args.classification), shadows)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, PolicyError) as failure:
        print(f"validate_allocation_policy: {failure}", file=sys.stderr)
        return 1
    text = json.dumps(receipt, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0 if receipt["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
