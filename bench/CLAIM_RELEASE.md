# System claim release microbenchmark

This independent installed-package consumer measures internal Memory changes,
not Memory adoption benefit, real applications, Android, energy or RSS.
The selected baseline remains the current production strategy from d2b6aa3;
rejected sticky/hint trials are separate and must never be pooled here.

Each round creates 1 or 32 SYSTEM domains and exactly 2048 allocations of
64 bytes aligned to 16. Global allocation index modulo domain count selects
the domain, so all domains have equal requested backing and exact budgets:
131072 bytes total, 131072 or 4096 bytes per domain. The preparing thread
allocates owners in global index order, obtains WRITE leases, writes and
reads every byte using observable volatile access, then releases each lease.
Before timing release, snapshots require all 2048 owners, no leases or
reservations, exact per-domain backing and exactly 2048 provider publications.
Payload checksum is independently recomputed by the Python controller.

The planned release permutation is forward, reverse or deterministic Fisher-
Yates using fixed xorshift64* seed 9e3779b97f4a7c15. A byte-defined FNV64 hash
of that permutation is checked independently. Workers take strided positions
in the permutation. Each thread sequence is deterministic; the global
interleaving for four threads is not controlled. Forward single-thread
release is deliberately retained as a best-case counterexample to extra
lookup/metadata cost. There is no work stealing or shared hot-loop owner flag.

Release batches contain 64 API calls. P50/P95 of batch duration divided by
batch size are not individual-call latency distributions. Release-phase wall
time includes the ready-worker dispatch and joins. All owners have no active
lease, so successful SYSTEM release synchronously completes backing cleanup.
After joining, snapshots require zero owners, leases, backing and reservations
before begin_close; every domain then collects, drains and destroys normally.

The complete workflow clock starts before domain creation/allocation/write-
read preparation and ends after the final close/drain/destroy. It also includes
worker creation and synchronization plus post-release resource checks.
Preparation, release phase and close durations are reported separately but
none is removed from this workflow. Argument parsing, permutation generation,
JSON reporting and final benchmark-container destruction are outside it.
A release-only win must not be called an end-to-end win: allocation still
performs overlap checks, backing unlink may remain linear, and metadata or
index construction can increase costs. Backing is requested capacity, not
allocator metadata or RSS; those are reported as unmeasured.

Build with an installed host SDK:

```text
cmake -S bench/claim_release -B build/claim-release-bin -DCMAKE_PREFIX_PATH=<sdk> -DCMAKE_BUILD_TYPE=Release
cmake --build build/claim-release-bin --config Release --parallel 2
python tools/run_claim_release_pairs.py --exe <mgmem_claim_release> --baseline-runtime-dir <baseline-bin-or-lib> --candidate-runtime-dir <candidate-bin-or-lib> --out-dir build/<new-private-directory>
```

Default matrix: 2048 allocations; domains 1/32; forward/reverse/shuffled;
threads 1/4; four measured rounds and one warmup; four balanced AB/BA pairs.
This is exactly 96 independent child processes. The same executable is staged
with each selected library; actual module path/disk hash, complete workload,
resources and all summary statistics are checked. Raw JSON and logs stay in
private build. Path/hash checks are not signed provenance or mapped-page
attestation. There are no retries, sample removal, parameter changes or pooling.
Both earlier regressions and all forward results must be preserved. CPU
frequency, affinity and other host activity are uncontrolled. A failed process
is incomplete and does not establish successful cleanup or leak freedom.
