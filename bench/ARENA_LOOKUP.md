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

