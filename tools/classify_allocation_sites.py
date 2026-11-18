#!/usr/bin/env python3
"""Layer 1: statically classify allocation sites for Memory adoption.

Input is a site inventory JSON describing each candidate call site with
source-level facts only (kind, size shape, lifetime, existing pooling).
Output is a three-state classification per site: a concrete candidate
(arena / system / copyto / import), excluded, or uncertain. Uncertain
sites must go through Layer 2 shadow estimation; this tool never
recommends a runtime switch by itself and performs no device work.

Usage:
    python tools/classify_allocation_sites.py --inventory sites.json \
        --output classification.json
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

SCHEMA_VERSION = 1
LARGE_SINGLE_BYTES = 256 * 1024
PAGE_BYTES = 4096
MAX_SUGGESTED_CHUNK = 256 * 1024
MIN_BATCH_FOR_ARENA = 8

KINDS = {
    "heap_blocks", "single_buffer", "lease_transfer",
    "stack", "gpu", "existing_pool", "unknown",
}
SHAPES = {"fixed", "mixed", "unknown"}
LIFETIMES = {"short_batch", "long", "escape_async", "unknown"}


class InventoryError(ValueError):
    pass


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _optional_bool(site: dict, name: str, default: bool) -> bool:
    value = site.get(name, default)
    if not isinstance(value, bool):
        raise InventoryError(f"site {site.get('id')!r}: {name} must be bool")
    return value


def validate_site(site) -> dict:
    if not isinstance(site, dict):
        raise InventoryError("each site must be an object")
    site_id = site.get("id")
    if not isinstance(site_id, str) or not site_id:
        raise InventoryError("site id must be a non-empty string")
    kind = site.get("kind")
    if kind not in KINDS:
        raise InventoryError(f"site {site_id!r}: unknown kind {kind!r}")
    shape = site.get("shape", "unknown")
    if shape not in SHAPES:
        raise InventoryError(f"site {site_id!r}: unknown shape {shape!r}")
    lifetime = site.get("lifetime", "unknown")
    if lifetime not in LIFETIMES:
        raise InventoryError(f"site {site_id!r}: unknown lifetime {lifetime!r}")
    size = site.get("size_bytes")
    typical = None
    if size is not None:
        if not isinstance(size, dict):
            raise InventoryError(f"site {site_id!r}: size_bytes must be an object or null")
        for key in ("min", "max", "typical"):
            if key in size and not (_is_int(size[key]) and size[key] > 0):
                raise InventoryError(f"site {site_id!r}: size_bytes.{key} must be a positive int")
        if "min" in size and "max" in size and size["min"] > size["max"]:
            raise InventoryError(f"site {site_id!r}: size_bytes.min exceeds max")
        typical = size.get("typical")
    alignment = site.get("alignment")
    if alignment is not None and not (
            _is_int(alignment) and alignment > 0 and alignment & (alignment - 1) == 0):
        raise InventoryError(f"site {site_id!r}: alignment must be a power of two")
    calls = site.get("calls_per_batch")
    if calls is not None and not (_is_int(calls) and calls > 0):
        raise InventoryError(f"site {site_id!r}: calls_per_batch must be a positive int")
    return {
        "id": site_id,
        "kind": kind,
        "shape": shape,
        "lifetime": lifetime,
        "typical_bytes": typical,
        "alignment": alignment,
        "calls_per_batch": calls,
        "heap_allocation": _optional_bool(site, "heap_allocation", True),
        "existing_optimization": _optional_bool(site, "existing_optimization", False),
        "needs_stable_address": _optional_bool(site, "needs_stable_address", False),
        "external_owner_borrowable": _optional_bool(site, "external_owner_borrowable", False),
        "both_endpoints_leases": _optional_bool(site, "both_endpoints_leases", False),
    }


def validate_inventory(payload) -> list:
    if not isinstance(payload, dict):
        raise InventoryError("inventory must be an object")
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise InventoryError("inventory schema_version must be 1")
    sites = payload.get("sites")
    if not isinstance(sites, list) or not sites:
        hint = ""
        if isinstance(payload.get("entries"), list):
            hint = (" This file has 'entries' and looks like an allocation policy "
                    "(Layer 3); pass a site inventory instead, e.g. ALLOCATION_SITES.json.")
        raise InventoryError(
            "inventory sites must be a non-empty list; expected "
            '{"schema_version": 1, "sites": [{"id": ..., "kind": ...}]}.' + hint)
    if sites and isinstance(sites[0], dict) and "classification" in sites[0] \
            and "kind" not in sites[0]:
        raise InventoryError(
            "these rows carry 'classification' and look like classifier output, "
            "not an inventory; pass the original site inventory (rows need 'kind')")
    validated = [validate_site(site) for site in sites]
    ids = [site["id"] for site in validated]
    if len(ids) != len(set(ids)):
        raise InventoryError("duplicate site id in inventory")
    return validated


def _suggest_chunk(typical: int, alignment, calls: int) -> int:
    step = alignment or 16
    aligned = -(-typical // step) * step
    target = aligned * calls
    chunk = -(-target // PAGE_BYTES) * PAGE_BYTES
    return max(PAGE_BYTES, min(chunk, MAX_SUGGESTED_CHUNK))


def _result(site, classification, mode, reasons, chunk=None, shadow=False):
    return {
        "id": site["id"],
        "classification": classification,
        "recommended_mode": mode,
        "reasons": reasons,
        "suggested_chunk_bytes": chunk,
        "requires_shadow": shadow,
    }


def classify_site(site: dict) -> dict:
    kind = site["kind"]
    # Already pooled or no heap allocation: no Memory increment exists.
    # Excluding these first prevents double-counting existing wins
    # (e.g. BIL P1-07 scratch / P2-05 holder packing).
    if site["existing_optimization"] or kind == "existing_pool":
        return _result(site, "excluded", "none", ["already_optimized"])
    if kind == "gpu":
        return _result(site, "excluded", "none", ["gpu_owner_out_of_scope"])
    if kind == "stack" or not site["heap_allocation"]:
        return _result(site, "excluded", "none", ["no_heap_allocation"])
    if kind == "lease_transfer":
        if site["both_endpoints_leases"]:
            return _result(site, "copyto_candidate", "copyto",
                           ["both_endpoints_already_leases"])
        return _result(site, "uncertain", "shadow",
                       ["endpoints_not_both_leases_migration_cost_unknown"], shadow=True)
    if kind == "single_buffer":
        if site["external_owner_borrowable"]:
            return _result(site, "import_candidate", "import",
                           ["stable_external_owner_can_be_borrowed"])
        typical = site["typical_bytes"]
        if site["needs_stable_address"] or site["lifetime"] == "long" or (
                typical is not None and typical >= LARGE_SINGLE_BYTES):
            return _result(site, "system_candidate", "system",
                           ["single_dense_buffer_arena_saves_no_provider_call"])
        return _result(site, "uncertain", "shadow",
                       ["single_buffer_distribution_unknown"], shadow=True)
    if kind == "heap_blocks":
        # A long-lived or escaping owner pins its whole arena chunk;
        # System keeps per-allocation return instead.
        if site["lifetime"] in ("long", "escape_async") or site["needs_stable_address"]:
            return _result(site, "system_candidate", "system",
                           ["lifetime_would_pin_arena_chunk"])
        typical = site["typical_bytes"]
        if typical is not None and typical >= LARGE_SINGLE_BYTES:
            return _result(site, "system_candidate", "system",
                           ["blocks_too_large_for_chunk_sharing"])
        if site["shape"] == "mixed":
            return _result(site, "uncertain", "shadow",
                           ["mixed_shape_no_stable_host_gain"], shadow=True)
        if site["shape"] == "unknown" or typical is None or site["calls_per_batch"] is None:
            return _result(site, "uncertain", "shadow",
                           ["missing_size_or_frequency_distribution"], shadow=True)
        if site["calls_per_batch"] < MIN_BATCH_FOR_ARENA:
            return _result(site, "system_candidate", "system",
                           ["batch_too_small_to_amortize_domain"])
        if site["lifetime"] == "unknown":
            return _result(site, "uncertain", "shadow",
                           ["lifetime_unknown"], shadow=True)
        chunk = _suggest_chunk(typical, site["alignment"], site["calls_per_batch"])
        return _result(site, "arena_candidate", "arena",
                       ["same_shape_short_lived_batch"], chunk=chunk)
    return _result(site, "uncertain", "shadow", ["kind_unknown"], shadow=True)


def classify_inventory(payload) -> dict:
    sites = validate_inventory(payload)
    results = [classify_site(site) for site in sites]
    summary = {}
    for row in results:
        summary[row["classification"]] = summary.get(row["classification"], 0) + 1
    return {
        "schema_version": SCHEMA_VERSION,
        "tool": "classify_allocation_sites",
        "performance_qualified": False,
        "device_executed": False,
        "sites": results,
        "summary": summary,
    }


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        payload = json.loads(args.inventory.read_text(encoding="utf-8-sig"))
        report = classify_inventory(payload)
    except (OSError, json.JSONDecodeError, InventoryError) as failure:
        print(f"classify_allocation_sites: {failure}", file=sys.stderr)
        return 1
    text = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
