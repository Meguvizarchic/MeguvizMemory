# Arena reset cost vs retained chunk count

Independent installed-package consumer. It does not change the root build or
establish a product performance result.

## Method

For each chunk level (1 / 64 / 1024 chunks of 4096 bytes) one ARENA domain
is created; every round prepares the level by allocating one chunk-sized
block per chunk, payload-checking the first and last blocks, and releasing
all owners, then times only `mgmem_arena_reset_v1`. Preparation
(allocate + release) is timed separately as context and never enters the
reset samples. Snapshots prove `provider_allocation_count` stays at the
level all run, i.e. reset rewinds retained chunks for reuse instead of
returning them.

One reset at 1-64 chunks is at or below the 100 ns `steady_clock` tick on
this machine, so each round additionally batches 1000 consecutive resets
and reports the amortized per-reset cost. A reset on an already rewound
arena walks the same backing list and stores the same zero offsets, so the
batched value measures the same per-chunk walk; at 1024 chunks, where the
single-shot sample resolves, the two agree within ~1.5% (see table).
Slopes use the batched values. Defaults: 5 warmup rounds, 30 measured
rounds per level. One allocation per chunk by construction; reset walks
backings only (its precondition is zero live allocations), so the result
is a per-chunk, not per-allocation, cost.

```text
tools\host-build.cmd -S bench\arena_reset_slope -B build\arena-reset-slope-bin -DCMAKE_PREFIX_PATH=D:\DevArea\MeguvizMemory\install\p0-host -DCMAKE_BUILD_TYPE=Release
tools\host-build.cmd --build build\arena-reset-slope-bin --config Release --parallel 2
set PATH=D:\DevArea\MeguvizMemory\install\p0-host\bin;%PATH%
build\arena-reset-slope-bin\Release\mgmem_arena_reset_slope.exe [--rounds N] [--warmup-rounds N] [--batch-resets N]
```

## Results (2026-11-18, i7-13790F, install/p0-host mgmem.dll, ABI 1.1)

Three fresh processes, defaults. Raw JSON kept locally under
`build/arena-reset-slope-run{1,2,3}.json`. Values are per-reset ns.

| chunks (4096 B each) | batched p50 | single-shot p50 | prepare alloc+release p50 |
|---|---|---|---|
| 1 | 13.0 / 13.1 / 13.5 | 0 (tick floor) | 100 (tick floor) |
| 64 | 81.4 / 81.2 / 79.9 | 100 (one tick) | 3700 / 3700 / 3900 |
| 1024 | 3255 / 3347 / 3466 | 3300 / 3400 / 3500 | 75200 / 75900 / 82900 |

Slope from batched p50 differences (ns per chunk):

| segment | run 1 | run 2 | run 3 |
|---|---|---|---|
| 1 -> 64 | 1.09 | 1.08 | 1.05 |
| 64 -> 1024 | 3.31 | 3.40 | 3.53 |
| 1 -> 1024 | 3.17 | 3.26 | 3.37 |

## Reading

- Reset is affine with a ~13 ns fixed part (API, lock, quiet check,
  generation bump, observed directly at 1 chunk) plus a per-chunk walk.
  The incremental per-chunk cost is ~1.1 ns while the backing list is
  cache-resident (up to 64 chunks) and ~3.3-3.5 ns on the 64 -> 1024
  segment, so use `reset ~= 13 ns + 3.4 ns/chunk` as the conservative model
  beyond small arenas. No superlinear behavior was observed.
- Absolute scale: 1024 chunks (4 MiB retained) reset in ~3.3 us, about 4%
  of one allocate+release cycle of the same chunks (75-83 us).
  Extrapolating the conservative slope: 16k chunks (64 MiB) ~54 us,
  256k chunks (1 GiB) ~0.87 ms. The 1 GiB figure is an extrapolation; a
  backing list that outgrows cache could raise the per-chunk cost and was
  not measured.
- Not worth optimizing: at any plausible chunk count reset is a small
  fraction of the allocation cycle it rewinds. The threshold where a
  single reset alone would reach 1 ms is roughly 300k chunks (~1.2 GiB
  retained in one domain) under the conservative slope.

Not measured: reset after trim/reallocation churn (backing nodes here are
allocated sequentially and stay cache-local), mixed external backings in
the same list, contention on the domain mutex, and any Android/product
shape. `performance_qualified` is false; this is a synthetic scaling
observation, not a qualification.
