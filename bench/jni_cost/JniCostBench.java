package io.github.jichuo1.meguviz.memory.android;

import java.util.Arrays;

/**
 * Host JVM microbenchmarks for three never-isolated JNI/Java costs.
 * Plain JNI build only (no MGMEM_MANAGED_TESTING). No JMH: batched timing,
 * warmup rounds discarded, median of measured rounds reported.
 *
 * Point 1: managed allocate time vs bytes. nativeAllocate unconditionally
 *   borrow+memset(0)+release after mgmem_allocate_v1 (workspace_jni.cpp).
 *   Slope of ns vs bytes tells whether zeroing dominates.
 * Point 2: write vs read same size. nativeCopy write path allocates a
 *   staging buffer and copies twice (GetByteArrayRegion -> staging ->
 *   memcpy); read path is one SetByteArrayRegion. Difference attributes
 *   staging alloc + extra copy.
 * Point 3: zero-byte read/write/copyTo fixed cost (JNI hop + registry
 *   lookup + entry lock + validation, no data touched), plus empty borrow
 *   lifecycle ops/s (borrowWrite(0,1) + close, the minimum legal borrow).
 */
public final class JniCostBench {
    private static long sink = 0;

    public static void main(String[] args) throws Exception {
        System.out.println("java.version=" + System.getProperty("java.version"));
        System.out.println("os=" + System.getProperty("os.name") + " " + System.getProperty("os.arch"));
        benchAllocate();
        benchWriteVsRead();
        benchZeroAndBorrow();
        System.out.println("sink=" + sink);
    }

    // ---------- helpers ----------

    private static double median(double[] v) {
        double[] s = v.clone();
        Arrays.sort(s);
        return s[s.length / 2];
    }

    private static void closeDomain(ManagedMemoryDomain d) throws Exception {
        d.beginClose();
        long until = System.nanoTime() + 30_000_000_000L;
        while (d.state() != ManagedMemoryDomain.CLOSED && System.nanoTime() - until < 0) Thread.sleep(2);
        if (d.state() != ManagedMemoryDomain.CLOSED) throw new AssertionError("domain close timeout");
        d.forgetClosed();
    }

    private static void printRow(String label, long size, int iters, double nsPerOp) {
        double perByte = size > 0 ? nsPerOp / size : 0;
        System.out.printf("%-28s size=%9d iters/round=%7d  %12.1f ns/op  %8.4f ns/B  %10.0f ops/s%n",
                label, size, iters, nsPerOp, perByte, 1e9 / nsPerOp);
    }

    // ---------- Point 1: allocate slope ----------

    private static void benchAllocate() throws Exception {
        System.out.println();
        System.out.println("== Point 1: allocate (system domain) ns/op vs bytes ==");
        long[] sizes = {64, 4096, 65536, 1048576, 16777216};
        int[] iters  = {4000, 4000, 2000, 200, 12};
        double[] med = new double[sizes.length];
        for (int s = 0; s < sizes.length; s++) {
            long bytes = sizes[s];
            int n = iters[s];
            long budget = bytes * n + 64L * 1024 * 1024;
            double[] rounds = new double[7];
            for (int r = 0; r < 10; r++) { // 3 warmup + 7 measured
                ManagedMemoryDomain d = ManagedMemoryDomain.system(budget);
                ManagedMemoryDomain.Allocation[] owners = new ManagedMemoryDomain.Allocation[n];
                long t0 = System.nanoTime();
                for (int i = 0; i < n; i++) owners[i] = d.allocate(bytes, 8);
                long t1 = System.nanoTime();
                for (int i = 0; i < n; i++) owners[i].close();
                closeDomain(d);
                if (r >= 3) rounds[r - 3] = (double) (t1 - t0) / n;
            }
            med[s] = median(rounds);
            printRow("allocate", bytes, n, med[s]);
        }
        // Least-squares fit ns = a + b*bytes over the 5 medians.
        double mx = 0, my = 0;
        for (int i = 0; i < sizes.length; i++) { mx += sizes[i]; my += med[i]; }
        mx /= sizes.length; my /= sizes.length;
        double sxy = 0, sxx = 0;
        for (int i = 0; i < sizes.length; i++) {
            sxy += (sizes[i] - mx) * (med[i] - my);
            sxx += (sizes[i] - mx) * (sizes[i] - mx);
        }
        double b = sxy / sxx, a = my - b * mx;
        System.out.printf("fit: ns = %.1f + %.5f * bytes  (slope = %.2f GiB/s zeroing-equivalent)%n",
                a, b, 1.0 / b / 1.073741824);
        for (int i = 0; i < sizes.length; i++) {
            double frac = b * sizes[i] / med[i];
            System.out.printf("  size=%9d  size-proportional share of measured ns: %.1f%%%n", sizes[i], 100 * frac);
        }
        // Reference only: JVM zeroed allocation and plain fill, same sizes.
        System.out.println("-- reference (not the product path) --");
        for (int s = 0; s < sizes.length; s++) {
            int n = iters[s];
            int bytes = (int) sizes[s];
            byte[] pre = new byte[bytes];
            double[] rounds = new double[7];
            for (int r = 0; r < 10; r++) {
                long t0 = System.nanoTime();
                for (int i = 0; i < n; i++) Arrays.fill(pre, (byte) 0);
                long t1 = System.nanoTime();
                if (r >= 3) rounds[r - 3] = (double) (t1 - t0) / n;
            }
            sink += pre.length;
            printRow("ref Arrays.fill(byte[])", bytes, n, median(rounds));
        }
    }

    // ---------- Point 2: write vs read ----------

    private static void benchWriteVsRead() throws Exception {
        System.out.println();
        System.out.println("== Point 2: write vs read (one WRITE lease, 1 MiB allocation) ==");
        ManagedMemoryDomain d = ManagedMemoryDomain.system(64L * 1024 * 1024);
        ManagedMemoryDomain.Allocation owner = d.allocate(1048576, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowWrite(0, 1048576);
        int[] sizes = {4096, 65536, 262144, 1048576};
        int[] iters = {20000, 5000, 2000, 500};
        byte[] data = new byte[1048576];
        Arrays.fill(data, (byte) 0x5A);
        for (int s = 0; s < sizes.length; s++) {
            int count = sizes[s], n = iters[s];
            double[] rRounds = new double[7], wRounds = new double[7];
            for (int r = 0; r < 10; r++) {
                long t0 = System.nanoTime();
                for (int i = 0; i < n; i++) lease.read(0, data, 0, count);
                long t1 = System.nanoTime();
                for (int i = 0; i < n; i++) lease.write(0, data, 0, count);
                long t2 = System.nanoTime();
                if (r >= 3) { rRounds[r - 3] = (double) (t1 - t0) / n; wRounds[r - 3] = (double) (t2 - t1) / n; }
            }
            double rd = median(rRounds), wr = median(wRounds);
            printRow("read", count, n, rd);
            printRow("write", count, n, wr);
            System.out.printf("  diff write-read = %10.1f ns/op  (%.5f ns/B)  ratio=%.2f%n",
                    wr - rd, (wr - rd) / count, wr / rd);
        }
        sink += data[0];
        lease.close();
        owner.close();
        closeDomain(d);
    }

    // ---------- Point 3: zero-byte fixed cost + empty borrow lifecycle ----------

    private static void benchZeroAndBorrow() throws Exception {
        System.out.println();
        System.out.println("== Point 3: zero-byte fixed cost + empty borrow lifecycle ==");
        ManagedMemoryDomain d = ManagedMemoryDomain.system(64L * 1024 * 1024);
        ManagedMemoryDomain.Allocation owner = d.allocate(4096, 8);
        ManagedMemoryDomain.Allocation ownerB = d.allocate(4096, 8);
        ManagedMemoryDomain.Lease a = owner.borrowWrite(0, 4096);
        // Overlapping WRITE borrows on one owner are rejected (BUSY); use a
        // second allocation for the copyTo destination.
        ManagedMemoryDomain.Lease b = ownerB.borrowWrite(0, 4096);
        byte[] data = new byte[16];
        final int n = 200000;
        // Empty-loop baseline (loop + branch only), reported, not subtracted.
        double[] base = new double[7], zr = new double[7], zw = new double[7], zc = new double[7];
        for (int r = 0; r < 10; r++) {
            long t0 = System.nanoTime();
            for (int i = 0; i < n; i++) sink += i & 1;
            long t1 = System.nanoTime();
            for (int i = 0; i < n; i++) a.read(0, data, 0, 0);
            long t2 = System.nanoTime();
            for (int i = 0; i < n; i++) a.write(0, data, 0, 0);
            long t3 = System.nanoTime();
            for (int i = 0; i < n; i++) a.copyTo(b, 0, 0, 0);
            long t4 = System.nanoTime();
            if (r >= 3) {
                base[r - 3] = (double) (t1 - t0) / n; zr[r - 3] = (double) (t2 - t1) / n;
                zw[r - 3] = (double) (t3 - t2) / n; zc[r - 3] = (double) (t4 - t3) / n;
            }
        }
        printRow("baseline empty loop", 0, n, median(base));
        printRow("read count=0", 0, n, median(zr));
        printRow("write count=0", 0, n, median(zw));
        printRow("copyTo count=0", 0, n, median(zc));
        // 1-byte variants, to separate early-return from data-path fixed cost.
        double[] r1 = new double[7], w1 = new double[7], c1 = new double[7];
        for (int r = 0; r < 10; r++) {
            long t0 = System.nanoTime();
            for (int i = 0; i < n; i++) a.read(0, data, 0, 1);
            long t1 = System.nanoTime();
            for (int i = 0; i < n; i++) a.write(0, data, 0, 1);
            long t2 = System.nanoTime();
            for (int i = 0; i < n; i++) a.copyTo(b, 0, 0, 1);
            long t3 = System.nanoTime();
            if (r >= 3) { r1[r - 3] = (double) (t1 - t0) / n; w1[r - 3] = (double) (t2 - t1) / n; c1[r - 3] = (double) (t3 - t2) / n; }
        }
        printRow("read count=1", 1, n, median(r1));
        printRow("write count=1", 1, n, median(w1));
        printRow("copyTo count=1", 1, n, median(c1));
        a.close();
        b.close();
        // Empty borrow lifecycle: minimum legal borrow is 1 byte.
        final int m = 20000;
        double[] lc = new double[7];
        for (int r = 0; r < 10; r++) {
            long t0 = System.nanoTime();
            for (int i = 0; i < m; i++) {
                ManagedMemoryDomain.Lease l = owner.borrowWrite(0, 1);
                l.close();
            }
            long t1 = System.nanoTime();
            if (r >= 3) lc[r - 3] = (double) (t1 - t0) / m;
        }
        printRow("borrowWrite(0,1)+close", 1, m, median(lc));
        ownerB.close();
        owner.close();
        closeDomain(d);
    }
}
