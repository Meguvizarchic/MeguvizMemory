# Adaptive metadata static evidence (2026-11-09)

This directory initially contains completed static evidence only. Prototype9aca
and final8628 remain separate. No formal performance results or final in-flight
host/Android results are introduced by publish_static.py.

prototype-core.patch changes only the core; final-production.patch includes all
seven production/header/test changes from baselinee92e777 with zero context and
no docs. Apply using git apply --unidiff-zero. source-bindings.json records exact
base and resulting file hashes.

Both count rounds retain all52 Arena cases, four layouts, all cumulative phase
snapshots, full native fields and exact original raw-log hashes. The complete
gzip is a lossless copy of ALREADY SANITIZED canonical JSON, with only local
runtime-module path replaced by its UTF8 hash as explicitly recorded there. It
is not a lossless gzip of private path-bearing raw logs. Original logs stay local.

Diagnostic ZIPs include only14 actual source/header/CMake files per round.
Inventory marks each entry EXACT or REVERSIBLE_PLACEHOLDER_SANITIZED and gives
both original/public byte counts and SHA. Absolute CMake paths, if encountered,
are transformed with reversible placeholders, never accepted by skipping scans.
Map placeholders to your local workspace/tool roots to build; original local
mapping remains private and its source hashes permit exact reconstruction.
Do not assume a sanitized source archive preserves binary identity.

The original count reports retain immutable internal basenames. Public flat
filenames carry prototype/final prefixes; heap-public-file-map.json maps every
original report/full-gzip/index basename to the corresponding public file.

Heap requested bytes are not timing, allocator usable size, RSS or energy.
The unchanged first-cold-batch combined peaks and capacity-hint smaller-phase
over-reservation costs remain in the reports. System count-fixture proposal was
not part of the frozen52-case plan; no observed cases were excluded or retried.

static-index.json inventories this completed static publication. Subsequent
performance/decision/final validation publications belong to the root workflow.
