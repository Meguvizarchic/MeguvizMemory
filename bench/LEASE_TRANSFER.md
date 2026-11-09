# Existing-lease transfer microbenchmark

`LeaseTransferPaired.java` compares `source.read(...scratch)` followed by
`target.write(...scratch)` against `source.copyTo(target, ...)`. Both sides use
existing managed Memory leases and transfer the same 64 KiB per operation.
The Java scratch array is allocated once and reused. The legacy write path's
native staging remains part of that implementation's actual cost.

The fixed workload warms both implementations for 6000 operations each, then
records twelve pairs of 3000 operations per window, with six AB and six BA
orders. No samples are discarded or retried. Destination reset and full byte
validation of both destination and unchanged source occur outside each timed
window. Successful completion requires the domain to reach CLOSED with zero
backing, reservations, live/retiring bytes and owner/lease counts.

Build the existing `tests/managed_host` project in a fresh build directory and
run its CTest contracts first. Its `plain` JNI target has no fault hooks. This
host test DLL embeds the core for test composition; it is not the Android
production shared-library composition.

```text
cmake -S tests/managed_host -B build/lease-transfer -DCMAKE_BUILD_TYPE=Release
cmake --build build/lease-transfer --config Release
ctest --test-dir build/lease-transfer -C Release --output-on-failure
javac --release 8 -cp build/lease-transfer/classes -d build/lease-transfer/classes bench/LeaseTransferPaired.java
```

On Windows with the multi-configuration generator:

```text
java -Djava.library.path=build/lease-transfer/plain/Release -cp build/lease-transfer/classes LeaseTransferPaired
```

On Linux with a single-configuration generator, use
`-Djava.library.path=build/lease-transfer/plain` instead. The program accepts no
arguments and prints a JSON record containing every paired duration and the
median paired candidate/baseline ratio. Preserve the full stdout; do not use
the ratio alone. Run on a worker, never as an application UI flow.

This is an exploratory single-process, cache-resident host JVM workload. CPU
affinity, frequency and other machine work are not controlled, and the program
does not compute a confidence interval or repeat independent JVM processes.
It establishes neither Android/ART timings nor product performance, DRAM
bandwidth, energy, or zero-copy behavior. The ratio compares complete transfer
implementations; their logical bytes match while their physical copy counts
intentionally differ.
