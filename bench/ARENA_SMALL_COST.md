# Prepared Arena small-workload cost diagnostic

This independent installed-SDK consumer separates work that the existing lookup
microbenchmark combines. It does not change the production library or the old
benchmark/controller. It is a cost diagnostic, not an Android, application or
performance qualification.

```text
cmake -S bench/arena_small_cost -B build/arena-small-cost-bin -DCMAKE_PREFIX_PATH=<installed-sdk> -DCMAKE_BUILD_TYPE=Release
cmake --build build/arena-small-cost-bin --config Release --parallel 2
python tools/run_arena_small_cost.py --exe <mgmem_arena_small_cost> --runtime-dir <selected-runtime> --out-dir build/<new-private-directory> --cycles 128 --rounds 8 --warmup 2 --repeats 4
```

Each round creates an Arena, prepares enough 4096-byte / 64-aligned chunks for
one batch, releases the preparation owners and resets. The preparation size is
the maximum integer first-fit requirement over all four possible starting
shapes, independently recomputed by the Python controller. Fixed requests use
64 bytes / alignment16; mixed cycles 32/64/96/128 bytes at alignment8/16/32/64.
Both fixed and mixed batch32 fit one prepared chunk; mixed batch64 needs two.
The exact provider count and peak backing must stay unchanged during timed use.

Each cycle resets the Arena outside the hot interval, then uses exactly one
batch. Defaults are batch32/64 and 128 cycles per round. Request-array setup and
summary recording are outside the hot interval. Reset, preparation and final
close are recorded separately. Compare a separately frozen `--cycles 1` matrix
with the repeated-cycle matrix to inspect amortization; never pool them.

Each raw round also reports `round_wall_ns`, bounded by two additional clock
reads: immediately before domain configuration/creation through successful
domain destruction. `round_wall_ns_per_op_p50/p95` is recalculated from those
raw round durations divided by completed operations. The span includes
preparation, reset, selected hot work, worker creation/dispatch/join where
applicable, validation snapshots, collect/drain and domain destruction. A node
free deferred into domain destruction therefore remains inside this span;
moving work out of hot/close subintervals is not by itself a net saving.

The two new reads are outside all existing hot intervals. Command-line parsing,
module lookup, initial result/slot-buffer allocation, final summarization and
JSON reporting are outside the round span. Local slot/preparation buffers are
destroyed during function return after the ending timestamp; returned result
storage is destroyed later by the caller. Those destructor costs are excluded.
This is a domain-lifecycle wall interval rather than complete process/application cost.
Existing reports without these fields stay bound to their original source
snapshots and must not be retroactively assigned the new metric.

Two modes have deliberately distinct workload IDs and live sets:

- `--mode stream` is `arena_small_cost_stream_v1`: allocate the whole batch,
  then for each owner acquire its WRITE lease, write and read every byte through
  volatile access, retire the owner while that lease lives, and release the
  lease. This preserves the old lookup benchmark's streaming lease lifetime:
  at most one lease is live. Three hot clock reads measure the allocation batch
  and the combined remainder; there are no per-API clock reads.
- `--mode stages` is `arena_small_cost_staged_v1`: allocate the batch, acquire
  all leases, process all payloads, retire all owners, then release all leases.
  Six hot clock reads separate allocation, borrow, payload, owner retirement
  and lease release. Its peak live lease count is the batch size, so these
  phase timings must not be subtracted from `stream` as a causal attribution.

The payload loop performs the same observable writes, checks and checksum
accumulation in both modes. A separate arithmetic modulo-256 byte oracle checks
every cycle and round. Semantic metadata insertion/removal counters describe
successful API work, not observed heap calls, node allocator events, metadata
bytes or RSS. Their interpretation must remain separate from any diagnostic
allocator instrumentation.

`--executor inline` runs the body directly. `--executor worker` creates one
fresh worker per round, waits until it is ready, then times dispatch through
join. The worker executes exactly the same selected mode/cycle workload.
Creation-to-ready time, worker-loop time and outer wall time are all retained.
Outer and loop differences are scheduling/wall-time observations, not isolated
CPU costs; thread creation is not included in dispatch-to-join time. No direct
subtraction of unrelated profiles proves a net benefit.

Every cycle uses two reset clock reads in addition to its hot clocks. The worker
loop also includes slot setup, reset, clock/bookkeeping overhead and accumulation.
Stage intervals include their surrounding check and clock bookkeeping. P50/P95
use nearest-index selection `floor((N-1)*p+0.5)` over raw cycle or round values;
they are not individual API latency distributions. Raw records allow independent
recalculation. Very short intervals can quantize to zero; complete hot intervals
must remain positive.

The controller runs a fresh staged executable/library process for every profile
and repetition, reversing the profile order on alternate repetitions. Module
path and disk hash must match the selected bytes; this is not mapped-image or
signed provenance attestation. Every result and failure is retained, with no
retries, exclusions or speed threshold. Output must be a new directory beneath
ignored `build/`, since it contains private paths. A child failure is incomplete
and may retain process-local owners until exit; it never establishes normal
close success. Successful rounds require zero owners, leases, live, retiring,
reserved and backing bytes after collect/drain, followed by domain destruction.

CPU frequency, affinity and competing activity are uncontrolled. This tool
identifies candidate fixed costs; implementation decisions still need a frozen
baseline/candidate comparison with the production lifecycle and all unfavorable
cases preserved.

## Paired runtime comparison

`tools/run_arena_small_cost_pairs.py` stages the same executable with two selected
libraries and alternates AB/BA for each identical shape/batch/mode/executor profile.
It reuses the independent raw-cycle validator and reports every metric's paired
ratio and absolute difference. A zero baseline interval has an undefined ratio
(`null`), with its difference and original output retained; it is not dropped.
There are no retries, exclusions or speed gates. Stream, stages, cycle counts
and cold batch1 cases remain separate workloads.

```text
python tools/run_arena_small_cost_pairs.py --exe <mgmem_arena_small_cost> --baseline-runtime-dir <baseline> --candidate-runtime-dir <candidate> --out-dir build/<new-pair-directory> --batches 32 64 --cycles 128 --rounds 8 --warmup 2 --pairs 8
```
