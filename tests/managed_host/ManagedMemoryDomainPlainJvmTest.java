package io.github.jichuo1.meguviz.memory.android;
public final class ManagedMemoryDomainPlainJvmTest {
    public static void main(String[] args) throws Exception {
        for (int i = 0; i < 2; ++i) {
            ManagedMemoryDomain d = i == 0 ? ManagedMemoryDomain.system(8192) : ManagedMemoryDomain.arena(8192, 4096);
            ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
            ManagedMemoryDomain.Lease lease = owner.borrowWrite(0, 64);
            byte[] data = new byte[64]; lease.read(0, data, 0, data.length);
            for (byte value : data) if (value != 0) throw new AssertionError("zero initialization");
            ManagedMemoryDomain.Allocation targetOwner = d.allocate(64, 8);
            ManagedMemoryDomain.Lease target = targetOwner.borrowWrite(0, 64);
            lease.write(0, new byte[]{9}, 0, 1);
            // Direct buffer view: write through it, read back, read-only view, invalidation.
            lease.withDirectBuffer(0, 64, buffer -> {
                if (!buffer.isDirect()) throw new AssertionError("direct buffer kind");
                if (buffer.get(0) != 9) throw new AssertionError("direct buffer sees staged write");
                buffer.put(1, (byte) 7);
                try { lease.close(); throw new AssertionError("close inside callback admitted"); }
                catch (IllegalStateException expected) { }
            });
            lease.read(0, data, 0, data.length);
            if (data[1] != 7) throw new AssertionError("direct buffer writeback");
            java.nio.ByteBuffer[] escaped = new java.nio.ByteBuffer[1];
            lease.withDirectBuffer(0, 64, buffer -> escaped[0] = buffer);
            if (escaped[0].limit() != 0) throw new AssertionError("buffer not invalidated after callback");
            try { lease.withDirectBuffer(1, 64, buffer -> { }); throw new AssertionError("range overflow admitted"); }
            catch (IndexOutOfBoundsException expected) { }
            owner.close(); targetOwner.close(); d.beginClose();
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
        ManagedMemoryDomain.Lease reader = owner.borrowRead(0, 64);
        reader.withDirectBuffer(0, 64, buffer -> {
            if (!buffer.isReadOnly()) throw new AssertionError("read lease buffer must be read-only");
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
