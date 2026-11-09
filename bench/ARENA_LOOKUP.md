# Arena lookup API microbenchmark

This independent installed-package consumer compares two selected core libraries
using the same executable and request set. It does not change the root build,
link a new allocator, run Android, or establish a product performance result.

Build from an installed Memory SDK:

```text
cmake -S bench/arena_lookup -B build/arena-lookup-bin -DCMAKE_PREFIX_PATH=<installed-sdk> -DCMAKE_BUILD_TYPE=Release
cmake --build build/arena-lookup-bin --config Release --parallel 2
python tools/run_arena_lookup_pairs.py --exe <mgmem_arena_lookup-executable> --baseline-runtime-dir <baseline-bin-or-lib> --candidate-runtime-dir <candidate-bin-or-lib> --out-dir build/arena-lookup-observation
```

The output directory must be new and under private ignored `build/`. It contains
absolute runtime observations and raw command output and must not be published
without review. Each library is copied beside the executable in a separate
directory and executed in a fresh process. The child reports its actual module
path; the controller requires that path and the disk hash to match the staged
library. This is loaded-module path/disk identity evidence, not mapped-page or
signed execution attestation. Windows and Linux are supported.

The default matrix has fixed and mixed requests with 1 and 4 threads sharing
one domain. Fixed requests use 64 bytes / 16-byte alignment. Mixed requests
cycle through 32, 64, 96, and 128 bytes with 8, 16, 32, and 64-byte alignment.
Each thread performs 4096 allocations in batches of 64, making many 4096-byte
Arena chunks. A batch first allocates all owners, then borrows each allocation
for writing, writes and reads every requested byte through volatile accesses to keep the
memory validation observable, releases its owner while
the lease is still alive, and finally releases the lease. Worker checksums
are independently recomputed by the Python controller. Every measured round
uses a new domain and requires final collection, drain and zero resources.
Warmup rounds use the same work but are excluded from summaries.

The allocation sample is **wall-clock batch allocation duration divided by
batch size**. It includes public API calls, metadata, any provider allocation,
mutex waiting and timing-loop overhead; it does not isolate CPU instructions
spent searching the Arena list. The reported P50/P95 uses nearest-rank-style
rounding at `floor((N-1)*p+0.5)` across those batch samples. Workflow samples use
round wall time / completed operations, including worker dispatch and join;
their P50/P95 is across measured rounds. Workflow throughput is completed
operations divided by summed workflow wall time. Close time is reported
separately and excluded from those workflow metrics. RSS, metadata cost,
energy and hardware events are not measured.

The controller alternates AB and BA for an even number of pairs (default 4).
All process observations and checks are kept; there are no automatic retries,
outlier exclusions, sample pooling with historical runs or confidence claims.
The executable and workload are identical for each side. Thread scheduling
may vary the order of mixed requests in a shared domain and thus Arena
packing, so per-round backing peaks are reported on both sides. Treat
capacity changes as a material result, not an invisible speedup.
Different-routine output values, checksums, incomplete work, wrong loaded
library or nonzero final resources stop collection.

Ratios are candidate / baseline, so values below 1 indicate a lower observed
duration in this synthetic workload. The controller also accepts identical
libraries for A/A measurement of its own noise. These observations do not
establish Android runtime behavior, end-to-end DEX/image benefit, cross-core
scaling, thermal stability or a release qualification. A child failure or
timeout is incomplete; a failed child may leave uncollected process-local
resources until process exit and is never counted as normal close success.


## Explicit phase-shift counterexample

`--shape phase` selects the separate `arena_lookup_phase_shift_v1` workload.
The host controller accepts `--shapes fixed mixed phase`; its default remains
exactly `fixed mixed`. Do not pool phase samples with the original workload.
The original retained-shape trial compared the production overwrite-cache
strategy from `d2b6aa3` with the retained-shape candidate. Later claim-reference
trials use this same workload as a separate countercheck, with the exact
baseline and candidate source hashes recorded in each frozen plan. The
pre-cursor `50bd9cd` library is not the baseline for either comparison.

Every phase round prepares its arena before timing: allocate and release
`ceil((total_B_operations * 64 + 64 * 32) / 4096)` whole 4096-byte chunks,
reset the arena, then allocate and release 64 A requests of 32 bytes aligned
to 8. This creates an A-heavy allocation history; whether a library actually records an A hint is policy-dependent. Timed work consists only of B requests
of 64 bytes aligned to 16, using the same full borrow/write/read/owner-release/
lease-release path and byte oracle as fixed requests. All worker threads share
that prepared arena. Preparation and A requests are excluded from the B-phase
workflow and allocation timings, and are identical on both sides.

The native report records the provider publication count immediately before
B and after B. Both counts must equal the exact prepared-chunk count, and
peak backing must equal prepared chunks times 4096. The controller checks
these fields, so a new provider publication invalidating the cache cannot
silently weaken the counterexample. This is a counter of successfully
published backing, not independent observation of every allocator callback.
Closing still requires zero owners, leases, reservations and backing.

This case deliberately exposes a possible cost of retaining an old shape:
A can remain selected throughout a long B phase, causing B to repeatedly
scan earlier full blocks while overwrite-cache can learn B. It is not an
end-to-end preparation-plus-use speed comparison. A correct implementation
can be slower, and such observations must remain in the result.

A proposed new host matrix is 4096 B requests per thread, fixed/mixed/phase,
1 and 4 threads, 2 warmup rounds, 6 measured rounds and 4 balanced pairs
(48 processes), followed by a separate tiny scope with 128 requests,
fixed/mixed, one thread, 16 warmup rounds, 64 measured rounds and 4 balanced
pairs (16 processes). Freeze final executable/library identities before
running; retain all pairs and any capacity differences. These are new scopes,
not additions to or replacements for previous observations.
