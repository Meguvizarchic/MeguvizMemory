package io.github.jichuo1.meguviz.memory.android;

import java.util.Arrays;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

public final class ManagedMemoryDomainPlainJvmTest {
    private static void checkArrayWrites(ManagedMemoryDomain domain) {
        try (ManagedMemoryDomain.Allocation owner = domain.allocate(2048, 8);
             ManagedMemoryDomain.Lease lease = owner.borrowWrite()) {
            byte[] input = new byte[2048];
            byte[] actual = new byte[2048];
            for (int i = 0; i < input.length; ++i) input[i] = (byte) (i * 7);
            lease.write(0, input, 0, input.length);
            lease.write(32, input, 128, 64);
            System.arraycopy(input, 128, input, 32, 64);
            lease.read(0, actual, 0, actual.length);
            if (!Arrays.equals(actual, input)) throw new AssertionError("array write paths");
        }
    }

    private static void checkConcurrentViews(ManagedMemoryDomain.Lease lease) throws Exception {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread waiter = new Thread(() -> {
            try { lease.withDirectBuffer(0, 1, view -> {
                if (view.remaining() != 1) throw new AssertionError("waiting view range");
            }); }
            catch (Throwable error) { failure.set(error); }
        });
        lease.withDirectBuffer(0, 2, outer -> {
            lease.withDirectBuffer(0, 1, inner -> { });
            waiter.start();
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            while (waiter.getState() != Thread.State.BLOCKED && System.nanoTime() - deadline < 0) Thread.yield();
            if (waiter.getState() != Thread.State.BLOCKED) throw new AssertionError("second callback did not wait");
            try { lease.close(); throw new AssertionError("enclosing callback admitted close"); }
            catch (IllegalStateException expected) { }
            if (lease.closeState() != NativeMemoryLease.CLOSE_OPEN || outer.limit() != 2)
                throw new AssertionError("rejected close changed active lease");
        });
        waiter.join(5000);
        if (waiter.isAlive() || failure.get() != null) throw new AssertionError("waiting callback", failure.get());
    }

    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 2; ++i) {
            ManagedMemoryDomain d = i == 0 ? ManagedMemoryDomain.system(8192) : ManagedMemoryDomain.arena(8192, 4096);
            checkArrayWrites(d);
            ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
            ManagedMemoryDomain.Lease lease = owner.borrowWrite();
            byte[] data = new byte[64]; lease.read(0, data, 0, data.length);
            for (byte value : data) if (value != 0) throw new AssertionError("zero initialization");
            ManagedMemoryDomain.Allocation targetOwner = d.allocate(64, 8);
            ManagedMemoryDomain.Lease target = targetOwner.borrowWrite(0, 64);
            lease.write(0, new byte[]{9}, 0, 1);
            // Direct buffer view: write through it, read back, read-only view, invalidation.
            lease.withDirectBuffer(buffer -> {
                if (!buffer.isDirect() || buffer.remaining() != owner.length())
                    throw new AssertionError("direct buffer kind or entire range");
                if (buffer.get(0) != 9) throw new AssertionError("direct buffer sees staged write");
                buffer.put(1, (byte) 7);
                try { lease.close(); throw new AssertionError("close inside callback admitted"); }
                catch (IllegalStateException expected) { }
            });
            lease.read(0, data, 0, data.length);
            if (data[1] != 7) throw new AssertionError("direct buffer writeback");
            checkConcurrentViews(lease);
            java.nio.ByteBuffer[] escaped = new java.nio.ByteBuffer[1];
            lease.withDirectBuffer(0, 64, buffer -> escaped[0] = buffer);
            if (escaped[0].limit() != 0) throw new AssertionError("buffer not invalidated after callback");
            try { lease.withDirectBuffer(1, 64, buffer -> { }); throw new AssertionError("range overflow admitted"); }
            catch (IndexOutOfBoundsException expected) { }
            owner.close(); targetOwner.close(); d.beginClose();
            if (owner.length() != 0) throw new AssertionError("closed allocation length");
            try { owner.borrowWrite(); throw new AssertionError("closed allocation borrowed"); }
            catch (IllegalStateException expected) { }
            lease.copyTo(target, 0, 0, 64);
            target.read(0, data, 0, data.length);
            if (data[0] != 9) throw new AssertionError("lease transfer lifetime");
            if (data[1] != 7) throw new AssertionError("direct buffer byte transferred");
            for (int index = 2; index < data.length; ++index)
                if (data[index] != 0) throw new AssertionError("complete transfer");
            target.close();
            lease.read(0, data, 0, 1); if (data[0] != 9) throw new AssertionError("lease lifetime");
            lease.close(); long until = System.nanoTime() + 8_000_000_000L;
            while (d.state() != ManagedMemoryDomain.CLOSED && System.nanoTime() - until < 0) Thread.sleep(2);
            if (d.state() != ManagedMemoryDomain.CLOSED) throw new AssertionError("close");
            d.forgetClosed();
        }
        // Read leases expose a read-only direct view.
        ManagedMemoryDomain d = ManagedMemoryDomain.system(8192);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease writer = owner.borrowWrite(0, 64);
        writer.write(0, new byte[]{5}, 0, 1);
        writer.close();
        ManagedMemoryDomain.Lease reader = owner.borrowRead();
        reader.withDirectBuffer(buffer -> {
            if (!buffer.isReadOnly() || buffer.remaining() != owner.length())
                throw new AssertionError("read lease buffer kind or entire range");
            if (buffer.get(0) != 5) throw new AssertionError("read lease direct view data");
        });
        reader.close();
        owner.close();
        d.beginClose();
        long until = System.nanoTime() + 8_000_000_000L;
        while (d.state() != ManagedMemoryDomain.CLOSED && System.nanoTime() - until < 0) Thread.sleep(2);
        if (d.state() != ManagedMemoryDomain.CLOSED) throw new AssertionError("read-view domain close");
        d.forgetClosed();
    }
}
