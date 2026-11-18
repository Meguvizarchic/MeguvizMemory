# External import marginal cost vs live import count

Independent installed-package consumer. It does not change the root build or
establish a product performance result.

## Method

For each background level (0 / 1024 / 16384 live external imports held in one
SYSTEM domain), the bench presets that many disjoint 64-byte fake external
regions through the real `mgmem_import_external_v1` API (fake storage plus
atomic retain/release callbacks), verifies the import snapshot, then times
one additional import at a time against a rotating pool of 32 disjoint probe
regions. The probe is borrowed and payload-checked, then released outside
the timed import interval; probe release time is recorded separately as a
diagnostic. The background stays live for the whole level, so every timed
import runs the full duplicate-identity / overlap check against all live
claims and succeeds (no early-exit rejection). Differences between levels
isolate the per-claim slope from the constant import cost.

Preset build time (importing the background into an empty domain) is also
recorded. Under a linear marginal cost it must grow quadratically, which
cross-checks the probe slope independently of the probe loop.

Timing is `std::chrono::steady_clock`; on this machine its tick is 100 ns,
so the 0-background median sits on the one-tick floor and the mean is the
resolved baseline there. Defaults: 200 warmup probes, then 5 rounds of 2000
probes per level (10000 samples per level).

```text
tools\host-build.cmd -S bench\import_slope -B build\import-slope-bin -DCMAKE_PREFIX_PATH=D:\DevArea\MeguvizMemory\install\p0-host -DCMAKE_BUILD_TYPE=Release
tools\host-build.cmd --build build\import-slope-bin --config Release --parallel 2
set PATH=D:\DevArea\MeguvizMemory\install\p0-host\bin;%PATH%
build\import-slope-bin\Release\mgmem_import_slope.exe [--probes N] [--rounds N] [--warmup-probes N]
```

## Results (2026-11-18, i7-13790F, install/p0-host mgmem.dll, ABI 1.1)

Three fresh processes, defaults. Raw JSON kept locally under
`build/import-slope-run{1,2,3}.json`.

| background live imports | probe import p50 (ns) | probe import mean (ns) | preset build (ms) |
|---|---|---|---|
| 0 | 100 / 100 / 100 | 107 / 111 / 101 | 0 |
| 1024 | 3000 / 3000 / 2900 | 3006 / 3054 / 3042 | 1.38 / 1.38 / 1.27 |
| 16384 | 46900 / 47600 / 46200 | 51108 / 52777 / 50815 | 417 / 451 / 476 |

Slope of one added import, from p50 differences (ns per live claim):

| segment | run 1 | run 2 | run 3 |
|---|---|---|---|
| 0 -> 1024 | 2.83 | 2.83 | 2.73 |
| 1024 -> 16384 | 2.86 | 2.90 | 2.82 |
| 0 -> 16384 | 2.86 | 2.90 | 2.81 |

Probe release p50 stayed at the 100 ns tick floor at every level (release
drops one known claim node; it does not scale). Round p50s inside a level
varied by at most ~6%.

## Reading

- The marginal import cost is affine: ~0.11 us constant + ~2.85 ns per live
  claim (all live claims in the loaded runtime, source: the linear
  `range_claims` list scan in `claim_range`). The two measured segments
  agree within 3% in every run, so a single slope describes 0..16384.
- Preset builds match the quadratic accumulation of that slope within
  0.81x-1.26x (fresh node allocation and table growth make builds deviate
  from the steady-state probe model), confirming the scaling order by a
  second route.
- Thresholds from the fitted line: one import reaches 1 us at ~310 live
  imports, 10 us at ~3.5k, 100 us at ~35k. Building N live imports from
  empty reaches 10 ms at ~2.6k, 100 ms at ~8.4k, 1 s at ~26k (measured:
  16k took 0.42-0.48 s). The scan also runs under the runtime-global range
  mutex, so at 16k live claims each import holds that mutex for ~47 us.
- Worth optimizing only if a real workload holds thousands of live imports
  (or imports in bulk): below ~300 live imports the marginal cost is
  sub-microsecond. An interval structure keyed by address (plus an
  external-id hash) would remove the linear term; nothing in the measured
  range suggests superlinear per-claim behavior.

Not measured: overlap-rejection cost (early exit depends on hit position),
arena-mode imports, checked imports, concurrent importers contending on the
range mutex, and any Android/product shape. `performance_qualified` is
false; this is a synthetic scaling observation, not a qualification.
