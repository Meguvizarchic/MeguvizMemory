#!/usr/bin/env python3
"""Draft a site inventory by scanning for Memory allocation call sites.

This is a fact extractor, not a classifier: it finds C/C++/Java calls
to the Memory allocation APIs, records file:line and a literal size
when the first argument is an integer constant, and leaves kind,
shape and lifetime as "unknown" for a human to fill in. Feed the
reviewed result to tools/classify_allocation_sites.py.

Usage:
    python tools/draft_inventory.py [--root src consumers ...] \
        --output inventory.draft.json
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".java", ".kt"}
CALL_RE = re.compile(
    r"(mgmem_allocate_v1"
    r"|mgmem_import_external(?:_arena)?(?:_checked)?_v1"
    r"|with_allocation"
    r"|(?:\.|->)allocate"
    r"|(?:\.|->)import_external(?:_arena)?(?:_checked)?)\s*\(")
LITERAL_RE = re.compile(r"\s*(\d[\d_]*)\s*(?:u64|ull|UL|u)?\s*$", re.IGNORECASE)
SKIP_DIRS = {"build", "install", ".git", "artifacts", "__pycache__"}


def _display_path(path: Path, root: Path) -> str:
    """Repo-root relative when under cwd, else relative to the root's parent."""
    absolute = path.resolve()
    cwd = Path.cwd().resolve()
    if absolute.is_relative_to(cwd):
        return absolute.relative_to(cwd).as_posix()
    parent = root.resolve().parent
    if absolute.is_relative_to(parent):
        return absolute.relative_to(parent).as_posix()
    return path.name


def _literal_size(line: str, match) -> int | None:
    """Literal size anchored inside this call's own argument list.

    mgmem_allocate_v1 takes (domain, bytes, ...) so its size is the
    second argument; the member helpers take (bytes, ...) first.
    Anything that is not a bare integer literal in that position, or
    whose argument list continues past this line, yields None instead
    of borrowing a number from a neighbouring call.
    """
    open_index = line.find("(", match.start())
    if open_index < 0:
        return None
    depth, close_index = 0, None
    for index in range(open_index, len(line)):
        if line[index] == "(":
            depth += 1
        elif line[index] == ")":
            depth -= 1
            if depth == 0:
                close_index = index
                break
    if close_index is None:
        return None
    args, depth, start = [], 0, open_index + 1
    for index in range(open_index + 1, close_index):
        if line[index] in "([{":
            depth += 1
        elif line[index] in ")]}":
            depth -= 1
        elif line[index] == "," and depth == 0:
            args.append(line[start:index])
            start = index + 1
    args.append(line[start:close_index])
    position = 1 if match.group(1) == "mgmem_allocate_v1" else 0
    if position >= len(args):
        return None
    literal = LITERAL_RE.match(args[position])
    if not literal:
        return None
    try:
        return int(literal.group(1).replace("_", ""))
    except ValueError:
        return None


UNREADABLE = []  # paths skipped during the last scan_roots call


def _has_symlink_parent(path: Path, root: Path) -> bool:
    current = path.parent
    while True:
        if current.is_symlink():
            return True
        if current == root or current == current.parent:
            return False
        current = current.parent


def scan_file(path: Path, root: Path) -> list:
    rows = []
    try:
        text = path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError:
        UNREADABLE.append(str(path))
        return rows
    relative = _display_path(path, root)
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "*")) or "MGMEM_API" in line:
            continue  # comments and C declarations are not call sites
        for occurrence, match in enumerate(CALL_RE.finditer(line)):
            size = _literal_size(line, match)
            stem = re.sub(r"[^A-Za-z0-9]+", "-", f"{relative}-{number}-{occurrence}").strip("-")
            rows.append({
                "id": f"draft.{stem}",
                "kind": "unknown",
                "shape": "unknown",
                "lifetime": "unknown",
                "size_bytes": {"typical": size} if size else None,
                "location": f"{relative}:{number}",
                "api": match.group(1),
                "needs_review": ["confirm_call_site", "kind", "shape", "lifetime",
                                 "existing_optimization", "calls_per_batch"]
                                + (["size_bytes"] if size else []),
            })
    return rows


def scan_roots(roots) -> dict:
    sites, seen, skipped_links = [], set(), []
    UNREADABLE.clear()
    for root in roots:
        root = Path(root)
        base = root if root.is_dir() else root.parent
        if root.is_file():
            files = [root] if not root.is_symlink() else []
            if root.is_symlink():
                skipped_links.append(str(root))
        else:
            files = []
            for path in root.rglob("*"):
                if path.suffix.lower() not in SUFFIXES:
                    continue
                relative_parts = path.relative_to(root).parts if path.is_relative_to(root) else ()
                if set(relative_parts) & SKIP_DIRS:
                    continue
                if path.is_symlink() or _has_symlink_parent(path, base):
                    skipped_links.append(str(path))
                    continue
                files.append(path)
            files.sort()
        for path in files:
            for row in scan_file(path, base if root.is_dir() else path.parent):
                candidate, suffix = row["id"], 2
                while candidate in seen:
                    candidate = f"{row['id']}--{suffix}"
                    suffix += 1
                row["id"] = candidate
                seen.add(candidate)
                sites.append(row)
    return {
        "schema_version": 1,
        "scope": ("Draft inventory from source scan; review every needs_review "
                  "field before classifying. Facts only come from file:line. "
                  "Line-based scan can still hit definitions or look-alike "
                  "methods; confirm_call_site is part of the review."),
        "draft": True,
        "roots": [str(Path(root)) for root in roots],
        "unreadable_files": list(UNREADABLE),
        "skipped_symlinks": skipped_links,
        "sites": sites,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", nargs="+", default=["src", "consumers", "adapters", "include"])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    roots = [Path(name) for name in args.root]
    missing = [str(r) for r in roots if not r.exists()]
    if missing:
        print(f"draft_inventory: root not found: {missing}; run from the repo root",
              file=sys.stderr)
        return 1
    draft = scan_roots(roots)
    text = json.dumps(draft, indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    print(f"draft_inventory: {len(draft['sites'])} candidate sites", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
