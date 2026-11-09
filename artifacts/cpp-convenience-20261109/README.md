# C++ convenience round, 2026-11-09

Selected header: 2e97483c... (full-chain). Runtime core remains 8628ce7f... .
Use decision.json and selected.patch for the final selection.

Root static files describe the first, unselected 4942 candidate. inlined/ holds
the unselected d062 candidate. full-chain/ holds the selected implementation.
All three static indexes remain immutable. performance/ retains three separate
192-process timing and 24-process count rounds, complete unfavorable metrics,
exact diagnostic sources and generated-code evidence. They are not pooled.

The first two candidates had stable double-digit P50 overhead. The selected
candidate removes that observed fixed overhead; owner P50 paired ratios are
0.971754 to 1.021125. Some P95 results still regress and are retained explicitly.
This is a scoped Windows Release synthetic comparison, not universal timing,
Android runtime, RSS, energy or product performance qualification.

All function-body assembly/source ZIP contents are verified individually.
Complete dumpbin text and raw stdout/stderr remain local; published native gzip
is lossless relative to the explicitly path-redacted canonical JSON.
