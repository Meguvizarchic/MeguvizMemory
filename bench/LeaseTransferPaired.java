import io.github.jichuo1.meguviz.memory.android.ManagedMemoryDomain;
import java.util.Arrays;

/** Exploratory host-JVM timing; compile against the matching managed plain JNI build. */
public final class LeaseTransferPaired {
    private static final int BYTES = 64 * 1024;
    private static final int ITERATIONS = 3000, WARMUP = 6000, PAIRS = 12;

    private static long window(boolean direct, ManagedMemoryDomain.Lease source,
                               ManagedMemoryDomain.Lease target, byte[] scratch, int iterations) {
        long started = System.nanoTime();
        if (direct) {
            for (int i = 0; i < iterations; ++i) source.copyTo(target, 0, 0, BYTES);
        } else {
            for (int i = 0; i < iterations; ++i) {
                source.read(0, scratch, 0, BYTES);
                target.write(0, scratch, 0, BYTES);
            }
        }
        return System.nanoTime() - started;
    }

    private static void verify(ManagedMemoryDomain.Lease source, ManagedMemoryDomain.Lease target,
                               byte[] expected, byte[] scratch, byte[] output) {
        target.read(0, output, 0, BYTES);
        if (!Arrays.equals(expected, output)) throw new AssertionError("complete destination mismatch");
        source.read(0, scratch, 0, BYTES);
        if (!Arrays.equals(expected, scratch)) throw new AssertionError("source modified");
    }

    private static double medianRatio(long[] baseline, long[] direct) {
        double[] ratios = new double[PAIRS];
        for (int i = 0; i < PAIRS; ++i) ratios[i] = (double) direct[i] / baseline[i];
        Arrays.sort(ratios);
        return (ratios[PAIRS / 2 - 1] + ratios[PAIRS / 2]) / 2;
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 0) throw new IllegalArgumentException("this frozen exploratory workload takes no arguments");
        ManagedMemoryDomain domain = ManagedMemoryDomain.system(BYTES * 2L);
        long[] baseline = new long[PAIRS], direct = new long[PAIRS];
        byte[] input = new byte[BYTES], scratch = new byte[BYTES], output = new byte[BYTES], zero = new byte[BYTES];
        for (int i = 0; i < BYTES; ++i) input[i] = (byte) (i * 31 + 7);
        try (ManagedMemoryDomain.Allocation a = domain.allocate(BYTES, 64);
             ManagedMemoryDomain.Allocation b = domain.allocate(BYTES, 64);
             ManagedMemoryDomain.Lease source = a.borrowWrite(0, BYTES);
             ManagedMemoryDomain.Lease target = b.borrowWrite(0, BYTES)) {
            source.write(0, input, 0, BYTES);
            // Both implementations warm up with the same fixed workload. No samples are selected or retried.
            window(false, source, target, scratch, WARMUP);
            window(true, source, target, scratch, WARMUP);
            verify(source, target, input, scratch, output);
            for (int pair = 0; pair < PAIRS; ++pair) {
                boolean directFirst = (pair & 1) != 0;
                for (int position = 0; position < 2; ++position) {
                    boolean useDirect = directFirst == (position == 0);
                    // Preparation and complete byte checks are identical and outside both timed windows.
                    target.write(0, zero, 0, BYTES);
                    long elapsed = window(useDirect, source, target, scratch, ITERATIONS);
                    if (elapsed <= 0) throw new AssertionError("non-positive timing");
                    verify(source, target, input, scratch, output);
                    (useDirect ? direct : baseline)[pair] = elapsed;
                }
            }
        } finally { domain.beginClose(); }
        long deadline = System.nanoTime() + 8_000_000_000L;
        while (domain.state() != ManagedMemoryDomain.CLOSED && System.nanoTime() < deadline) Thread.sleep(1);
        long[] finalState = domain.snapshot();
        if (domain.state() != ManagedMemoryDomain.CLOSED || finalState == null || finalState[3] != 0 ||
                finalState[4] != 0 || finalState[6] != 0 || finalState[7] != 0 || finalState[8] != 0 || finalState[9] != 0)
            throw new AssertionError("final resources not zero");
        domain.forgetClosed();
        StringBuilder json = new StringBuilder();
        json.append("{\"schema_version\":1,\"state\":\"PASS_EXPLORATORY_HOST_ONLY\",")
            .append("\"workload\":\"same_existing_native_lease_bytes_transfer\",\"bytes_per_operation\":").append(BYTES)
            .append(",\"iterations_per_window\":").append(ITERATIONS)
            .append(",\"warmup_iterations_each\":").append(WARMUP)
            .append(",\"baseline\":\"read_then_write_reused_java_array\",\"candidate\":\"copyTo\",")
            .append("\"checks\":\"full_destination_bytes_and_unchanged_source_after_every_window\",")
            .append("\"final_domain\":\"CLOSED_ZERO\",\"pairs\":[");
        for (int pair = 0; pair < PAIRS; ++pair) {
            if (pair != 0) json.append(',');
            json.append("{\"index\":").append(pair)
                .append(",\"order\":\"").append((pair & 1) == 0 ? "AB" : "BA")
                .append("\",\"baseline_ns\":").append(baseline[pair])
                .append(",\"copyTo_ns\":").append(direct[pair]).append('}');
        }
        json.append("],\"median_paired_copyTo_over_baseline\":").append(medianRatio(baseline, direct))
            .append(",\"limits\":[\"single_host_JVM_process\",\"cache_resident_64KiB_payload\",")
            .append("\"CPU_frequency_affinity_and_other_work_not_controlled\",\"not_Android_or_product_qualification\"]}");
        System.out.println(json);
    }
}
