package io.github.jichuo1.meguviz.memory.android;

import java.util.concurrent.atomic.AtomicReference;

/** Isolated host-JVM checks; injected failures deliberately keep unknown owners. */
public final class ManagedMemoryDomainJvmTest {
    private static native void nativeArmGate(int kind);
    private static native boolean nativeWaitGate(int kind);
    private static native boolean nativeReleaseGate(int kind);
    private static native void nativeInject(int kind);
    private static native void nativeFailBorrowRelease();
    private static native void nativeCheckedBorrowRelease();
    private static native void nativeBorrowInject(int kind);
    private static native void nativeArmBorrowGate(int kind);
    private static native boolean nativeWaitBorrowGate(int kind);
    private static native boolean nativeReleaseBorrowGate(int kind);
    private static native int nativeRawState(long token);

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static void rejected(Runnable operation, String message) {
        try { operation.run(); } catch (IllegalStateException expected) { return; }
        throw new AssertionError(message);
    }
    private static void rejectedStatus(Runnable operation, int status, String message) {
        try { operation.run(); }
        catch (ManagedMemoryDomain.ManagedException expected) {
            check(expected.status() == status, message + " status"); return;
        }
        throw new AssertionError(message);
    }
    private static void rejectedChecked(Runnable operation, String message) {
        try { operation.run(); }
        catch (IllegalStateException expected) {
            check(expected.getCause() instanceof java.io.IOException, message + " cause"); return;
        }
        throw new AssertionError(message);
    }
    private static void awaitState(ManagedMemoryDomain domain, int wanted) throws Exception {
        long deadline = System.nanoTime() + 8_000_000_000L;
        while (domain.state() != wanted && System.nanoTime() - deadline < 0) Thread.sleep(2);
        check(domain.state() == wanted, "domain state timeout: " + domain.state());
    }
    private static void join(Thread thread) throws Exception {
        thread.join(5000); check(!thread.isAlive(), "test thread stuck");
    }
    private static void arm(int id) { nativeArmGate(id); }
    private static void entered(int id) { check(nativeWaitGate(id), "gate was not entered"); }
    private static void release(int id) { check(nativeReleaseGate(id), "gate timed out"); }
    private static long[] counts() {
        long[] result = ManagedMemoryDomain.runtimeCounts();
        check(result != null && result.length == 7, "runtime counts unavailable");
        return result;
    }
    private static ManagedMemoryDomain closedDomain() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        d.beginClose(); awaitState(d, ManagedMemoryDomain.CLOSED); return d;
    }
    private static void finish(ManagedMemoryDomain d) throws Exception {
        awaitState(d, ManagedMemoryDomain.CLOSED);
        long[] end = d.snapshot();
        check(end != null && end.length == 10 && end[0] == ManagedMemoryDomain.CLOSED && end[1] == 2 &&
                end[3] == 0 && end[4] == 0 && end[6] == 0 && end[7] == 0 && end[8] == 0 && end[9] == 0,
                "normal final domain accounting");
        d.forgetClosed();
        check(d.state() == ManagedMemoryDomain.CLOSED && d.failureStatus() == 0, "forgotten close state");
    }
    private static void normal(boolean arena) throws Exception {
        long[] before = counts();
        ManagedMemoryDomain d = arena ? ManagedMemoryDomain.arena(8192, 4096) : ManagedMemoryDomain.system(8192);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        check(owner.length() == 64, "allocation length");
        ManagedMemoryDomain.Lease lease = owner.borrowWrite(0, 64);
        byte[] value = new byte[64]; lease.read(0, value, 0, value.length);
        for (byte b : value) check(b == 0, "native allocation not initialized");
        byte[] patch = {7, 8, 9}; lease.write(3, patch, 0, patch.length);
        if (arena) {
            d.setBudget(0);
            rejectedStatus(() -> d.allocate(1, 8), 4, "zero budget admitted allocation");
            lease.read(3, value, 0, patch.length);
            check(value[0] == 7 && value[1] == 8 && value[2] == 9, "budget revoked existing lease");
        }
        owner.close(); d.beginClose();
        check(d.state() == ManagedMemoryDomain.CLOSING, "live lease did not defer close");
        long[] retiring = d.snapshot();
        check(retiring != null && retiring[7] == 64 && retiring[8] == 1 && retiring[9] == 1,
                "retiring allocation/lease accounting");
        lease.read(3, value, 0, patch.length);
        check(value[0] == 7 && value[1] == 8 && value[2] == 9, "lease invalid after close");
        rejected(() -> d.allocate(1, 8), "close admitted new owner");
        lease.close(); finish(d);
        long[] after = counts();
        check(after[0] == before[0] && after[1] == before[1] && after[2] == before[2], "normal registry leak");
    }
    private static void workerRetry() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(32, 8);
        nativeInject(1);
        rejected(d::beginClose, "injected worker admission succeeded");
        check(d.state() == ManagedMemoryDomain.CLOSING, "worker failure revoked closing state");
        rejected(() -> d.allocate(1, 8), "failed worker reopened admission");
        owner.close(); d.beginClose(); finish(d);
    }
    private static void raceAllocate() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        AtomicReference<Throwable> outcome = new AtomicReference<>();
        arm(1);
        Thread thread = new Thread(() -> { try { d.allocate(64, 8); outcome.set(new AssertionError("admitted after close")); }
            catch (IllegalStateException expected) { /* closed or rolled back */ }
            catch (Throwable e) { outcome.set(e); } });
        thread.start(); entered(1); d.beginClose(); release(1); join(thread);
        check(outcome.get() == null, "allocate/close race failed"); finish(d);
    }
    private static void raceBorrow() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        AtomicReference<ManagedMemoryDomain.Lease> lease = new AtomicReference<>();
        AtomicReference<Throwable> outcome = new AtomicReference<>();
        arm(2);
        Thread thread = new Thread(() -> { try { lease.set(owner.borrowRead(0, 64)); }
            catch (IllegalStateException expected) { /* core may observe closing */ }
            catch (Throwable e) { outcome.set(e); } });
        thread.start(); entered(2); d.beginClose(); release(2); join(thread);
        check(outcome.get() == null, "borrow/close race failed");
        owner.close(); if (lease.get() != null) lease.get().close(); finish(d);
    }
    private static void racePublished() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        AtomicReference<ManagedMemoryDomain.Allocation> result = new AtomicReference<>();
        AtomicReference<Throwable> outcome = new AtomicReference<>();
        arm(8);
        Thread thread = new Thread(() -> { try { result.set(d.allocate(64, 8)); }
            catch (Throwable e) { outcome.set(e); } });
        thread.start(); entered(8); d.beginClose(); release(8); join(thread);
        check(outcome.get() == null && result.get() != null,
                "previously admitted allocation did not complete");
        rejected(() -> result.get().borrowRead(0, 1), "closing admitted a new lease");
        result.get().close(); finish(d);
    }
    private static void racePin() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64); owner.close(); d.beginClose();
        AtomicReference<Throwable> outcome = new AtomicReference<>();
        arm(3);
        Thread thread = new Thread(() -> { try { lease.close(); } catch (Throwable e) { outcome.set(e); } });
        thread.start(); entered(3);
        check(d.state() == ManagedMemoryDomain.CLOSING && counts()[2] == 1,
                "domain closed before lifetime callback returned its pin");
        release(3); join(thread); check(outcome.get() == null, "lease release failed"); finish(d);
    }
    private static void forgetRace(int gate) throws Exception {
        ManagedMemoryDomain d;
        if (gate == 4 || gate == 5) {
            d = ManagedMemoryDomain.system(4096); d.beginClose();
            long token = domainToken(d), deadline = System.nanoTime() + 5_000_000_000L;
            while (nativeRawState(token) != ManagedMemoryDomain.CLOSED && System.nanoTime() - deadline < 0) Thread.sleep(2);
            check(nativeRawState(token) == ManagedMemoryDomain.CLOSED, "native close timeout");
        } else d = closedDomain();
        AtomicReference<Throwable> outcome = new AtomicReference<>();
        AtomicReference<Integer> value = new AtomicReference<>();
        arm(gate);
        Thread thread = new Thread(() -> { try {
            if (gate == 4) value.set(d.state());
            else if (gate == 7) value.set(d.failureStatus());
            else d.beginClose();
        } catch (Throwable e) { outcome.set(e); } });
        thread.start(); entered(gate);
        if (gate == 5) rejected(d::forgetClosed, "forgot while beginClose was reporting");
        else d.forgetClosed();
        release(gate); join(thread);
        if (gate == 5) d.forgetClosed();
        check(outcome.get() == null, "observer raced with forgotten token");
        if (gate == 4) check(value.get() == ManagedMemoryDomain.CLOSED, "state falsely quarantined");
        if (gate == 7) check(value.get() == 0, "failure falsely reported");
    }
    private static void dropRetry(boolean pendingException) throws Exception {
        ManagedMemoryDomain d = closedDomain();
        nativeInject(pendingException ? 6 : 5);
        rejected(d::forgetClosed, "injected metadata drop succeeded");
        check(d.state() == ManagedMemoryDomain.CLOSED && d.failureStatus() == 0, "forget lost known closed state");
        check(counts()[0] == (pendingException ? 0 : 1), "unexpected token count after drop");
        d.forgetClosed(); check(counts()[0] == 0, "metadata drop was not retryable");
    }
    private static void unknownCreate() {
        nativeInject(2);
        rejected(() -> ManagedMemoryDomain.system(4096), "injected create exception missing");
        long[] c = counts(); check(c[0] == 1 && c[4] == 1, "unclaimed domain not observable");
    }
    private static void unknownAllocate() {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        nativeInject(3);
        rejected(() -> d.allocate(64, 8), "injected allocate exception missing");
        long[] c = counts(); check(d.state() == ManagedMemoryDomain.QUARANTINED && c[1] == 1 && c[5] == 1,
                "unknown owner not quarantined");
        check(d.snapshot() == null, "unknown snapshot was reported as valid");
    }
    private static void unknownBorrow(boolean unknownStatus) {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        nativeInject(unknownStatus ? 8 : 4);
        rejected(() -> owner.borrowRead(0, 64), "injected borrow failure missing");
        long[] c = counts();
        check(d.state() == ManagedMemoryDomain.QUARANTINED && c[2] == 1 && c[5] == 1,
                "unknown borrow did not retain pin");
    }
    private static void destroyUnknown() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        nativeInject(7); d.beginClose(); awaitState(d, ManagedMemoryDomain.QUARANTINED);
        check(d.failureStatus() != 0 && d.snapshot() == null, "unknown destroy appeared successful");
        rejected(d::forgetClosed, "unknown destroy record removed");
    }
    private static void leaseReleaseFailure(boolean quarantineJniFails) throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose();
        nativeFailBorrowRelease();
        if (quarantineJniFails) nativeInject(9);
        rejected(lease::close, "injected native lease close error missing");
        check(lease.closeState() == NativeMemoryLease.CLOSE_FAILED &&
                d.state() == ManagedMemoryDomain.QUARANTINED &&
                ManagedMemoryDomain.javaQuarantineSignals() == 1,
                "managed lease failure not observable");
        long[] c = counts();
        check(c[2] == 1, "uncertain adapter pin released");
        check(c[5] == 1, "guard did not publish native quarantine");
        lease.close(); // Never retries an uncertain release.
        check(counts()[2] == 1, "repeat close dropped uncertain pin");
    }
    private static void javaOnlyQuarantine() {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose(); nativeFailBorrowRelease(); nativeInject(21);
        rejected(lease::close, "injected double quarantine failure missing");
        long[] c = counts();
        check(d.state() == ManagedMemoryDomain.QUARANTINED && c[2] == 1 && c[5] == 0 && c[6] == 1 &&
                ManagedMemoryDomain.javaQuarantineSignals() == 1, "Java-only unknown was not retained/visible");
        rejected(d::forgetClosed, "Java-only unknown was forgotten");
    }
    private static void checkedCreate(boolean duringClaim) {
        nativeInject(duringClaim ? 11 : 10);
        rejectedChecked(() -> ManagedMemoryDomain.system(4096), "checked create/claim exception missing");
        long[] c = counts();
        check(c[0] == 1 && (duringClaim ? c[5] == 1 : c[4] == 1) &&
                ManagedMemoryDomain.javaQuarantineSignals() == 1, "create/claim unknown not observable");
    }
    private static void checkedAllocate(boolean quarantineAlsoThrows) {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        if (quarantineAlsoThrows) nativeInject(15);
        nativeInject(12);
        rejectedChecked(() -> d.allocate(64, 8), "checked allocate exception missing");
        long[] c = counts();
        check(d.state() == ManagedMemoryDomain.QUARANTINED && d.failureStatus() == -1 &&
                c[1] == 1 && ManagedMemoryDomain.javaQuarantineSignals() == 1,
                "checked allocation unknown not quarantined");
        if (quarantineAlsoThrows) check(c[5] == 0, "native quarantine unexpectedly recorded");
        rejected(d::beginClose, "Java-only quarantine allowed close");
        rejected(d::forgetClosed, "Java-only quarantine reported closed");
    }
    private static void checkedBorrow() {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        nativeInject(13);
        rejectedChecked(() -> owner.borrowRead(0, 64), "checked borrow exception missing");
        check(d.state() == ManagedMemoryDomain.QUARANTINED && counts()[2] == 1,
                "checked borrow did not retain unknown pin");
    }
    private static void checkedRelease() {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        nativeInject(14);
        rejectedChecked(owner::close, "checked owner release exception missing");
        check(d.state() == ManagedMemoryDomain.QUARANTINED && ManagedMemoryDomain.javaQuarantineSignals() == 1,
                "checked release unknown not observable");
        owner.close(); // No second native release of an uncertain owner.
    }
    private static void checkedLeaseClose() {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose(); nativeCheckedBorrowRelease();
        rejectedChecked(lease::close, "checked lease close exception missing");
        check(lease.closeState() == NativeMemoryLease.CLOSE_FAILED &&
                lease.closeFailureStatus() == NativeMemoryLease.CLOSE_FAILURE_UNKNOWN &&
                d.state() == ManagedMemoryDomain.QUARANTINED && ManagedMemoryDomain.javaQuarantineSignals() == 1,
                "checked lease close unknown not observable");
        lease.close(); // A repeat does not retry the consumed native lease.
    }
    private static void guardAdmissionRetry() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose(); nativeInject(16);
        AtomicReference<Throwable> error = new AtomicReference<>();
        arm(14);
        Thread first = new Thread(() -> {
            try { rejectedStatus(lease::close, ManagedMemoryDomain.STATUS_CLOSE_GUARD_LIMIT,
                    "guard limit was not reported"); }
            catch (Throwable failure) { error.set(failure); }
        });
        first.start();
        entered(14);
        synchronized (d) {
            // Hold the Java report open after native admission has failed.
            release(14);
            long deadline = System.nanoTime() + 5_000_000_000L;
            while (first.getState() != Thread.State.BLOCKED && System.nanoTime() - deadline < 0) Thread.yield();
            check(first.getState() == Thread.State.BLOCKED && d.pendingManagedOperations() == 1,
                    "failed close did not reach its reporting tail");
            check(lease.closeState() == NativeMemoryLease.CLOSE_RELEASING, "retry reopened before reporting finished");
            lease.close();
            check(counts()[6] == 0, "concurrent close admitted a second guard");
        }
        join(first);
        if (error.get() != null) throw new AssertionError("first close", error.get());
        check(lease.closeState() == NativeMemoryLease.CLOSE_OPEN &&
                d.state() == ManagedMemoryDomain.CLOSING && counts()[2] == 1 && counts()[6] == 0,
                "known pre-admission failure consumed lease");
        lease.close(); finish(d);
    }
    private static void guardEnterUnknown() {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose(); nativeInject(17);
        rejectedChecked(lease::close, "unknown guard admission did not throw");
        check(lease.closeState() == NativeMemoryLease.CLOSE_FAILED &&
                d.state() == ManagedMemoryDomain.QUARANTINED && counts()[2] == 1 && counts()[6] == 1,
                "unknown guard admission was not retained");
    }
    private static void guardExit(boolean fail) throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose();
        AtomicReference<Throwable> result = new AtomicReference<>();
        if (fail) nativeInject(18); else arm(9);
        Thread thread = new Thread(() -> { try { lease.close(); } catch (Throwable error) { result.set(error); } });
        thread.start();
        if (!fail) {
            entered(9);
            check(d.state() == ManagedMemoryDomain.CLOSING && counts()[2] == 0 && counts()[6] == 1,
                    "domain closed before guard registry cleanup");
            release(9);
        }
        join(thread);
        if (fail) {
            check(result.get() instanceof IllegalStateException && d.state() == ManagedMemoryDomain.QUARANTINED &&
                    lease.closeState() == NativeMemoryLease.CLOSE_FAILED && counts()[6] == 1,
                    "failed guard cleanup was not isolated");
        } else {
            check(result.get() == null, "guard cleanup unexpectedly failed"); finish(d);
        }
    }
    private static void guardUnderflow() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose(); nativeInject(19);
        rejected(lease::close, "injected guard underflow missing");
        check(d.state() == ManagedMemoryDomain.QUARANTINED, "guard underflow not isolated");
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (ManagedMemoryDomain.activeCleanupWorkers() != 0 && System.nanoTime() - deadline < 0) Thread.sleep(2);
        check(ManagedMemoryDomain.activeCleanupWorkers() == 0, "quarantined worker not notified");
    }
    private static long domainToken(ManagedMemoryDomain domain) throws Exception {
        java.lang.reflect.Field field = ManagedMemoryDomain.class.getDeclaredField("token");
        field.setAccessible(true); return field.getLong(domain);
    }
    private static void javaCloseTail(boolean failure) throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose();
        long token = domainToken(d);
        if (failure) nativeInject(20);
        arm(10);
        AtomicReference<Throwable> result = new AtomicReference<>();
        Thread thread = new Thread(() -> { try { lease.close(); } catch (Throwable error) { result.set(error); } });
        thread.start(); entered(10);
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (nativeRawState(token) != ManagedMemoryDomain.CLOSED && System.nanoTime() - deadline < 0) Thread.sleep(2);
        check(nativeRawState(token) == ManagedMemoryDomain.CLOSED, "core did not reach closed state");
        check(d.state() == ManagedMemoryDomain.CLOSING && d.pendingManagedOperations() == 1 && d.snapshot() == null,
                "Java close in flight was reported fully complete");
        rejected(d::forgetClosed, "forgot while Java close was reporting");
        release(10); join(thread);
        if (failure) {
            check(result.get() instanceof IllegalStateException && result.get().getCause() instanceof java.io.IOException &&
                    d.state() == ManagedMemoryDomain.QUARANTINED && d.pendingManagedOperations() == 0,
                    "late checked exception not retained");
        } else {
            check(result.get() == null && d.pendingManagedOperations() == 0, "Java close tail failed");
            finish(d);
        }
    }
    private static void observedClosedBeatsStaleState() throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        nativeArmBorrowGate(5);
        AtomicReference<Integer> old = new AtomicReference<>();
        Thread reader = new Thread(() -> old.set(d.state()));
        reader.start(); check(nativeWaitBorrowGate(5), "state tail not entered");
        d.beginClose(); awaitState(d, ManagedMemoryDomain.CLOSED);
        check(nativeReleaseBorrowGate(5), "state gate timed out"); join(reader);
        check(old.get() == ManagedMemoryDomain.CLOSED, "stale state overrode confirmed close");
        d.forgetClosed();
    }
    private static void observedClosedRejectsMutation() throws Exception {
        ManagedMemoryDomain d = closedDomain();
        rejectedStatus(() -> d.allocate(1, 8), 7, "allocation after observed close");
        rejectedStatus(() -> d.setBudget(128), 7, "budget after observed close");
        d.beginClose();
        check(d.pendingManagedOperations() == 0 && d.state() == ManagedMemoryDomain.CLOSED,
              "observed close regressed during rejected operations");
        d.forgetClosed();
    }
    private static void javaMutationTail(boolean allocation, boolean failure) throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = allocation ? null : d.allocate(64, 8);
        long token = domainToken(d);
        int gate = allocation ? 4 : 3;
        if (failure) nativeBorrowInject(allocation ? 5 : 4);
        nativeArmBorrowGate(gate);
        AtomicReference<Throwable> result = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                if (allocation) d.allocate(8192, 8); else owner.close();
            } catch (Throwable error) { result.set(error); }
        });
        worker.start(); check(nativeWaitBorrowGate(gate), "mutation tail not entered");
        d.beginClose();
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (nativeRawState(token) != ManagedMemoryDomain.CLOSED && System.nanoTime() - deadline < 0) Thread.sleep(2);
        check(nativeRawState(token) == ManagedMemoryDomain.CLOSED, "native domain did not close");
        check(d.state() == ManagedMemoryDomain.CLOSING && d.pendingManagedOperations() == 1,
                "in-flight Java mutation reported closed");
        rejected(d::forgetClosed, "forgot during mutation report");
        check(nativeReleaseBorrowGate(gate), "mutation gate timed out"); join(worker);
        if (failure) {
            check(result.get() instanceof IllegalStateException && result.get().getCause() instanceof java.io.IOException &&
                  d.state() == ManagedMemoryDomain.QUARANTINED, "late mutation error lost");
        } else {
            check(allocation ? result.get() instanceof ManagedMemoryDomain.ManagedException : result.get() == null,
                  "unexpected mutation outcome");
            finish(d);
        }
    }
    private static void closeAdapterTail(boolean failure) throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        ManagedMemoryDomain.Lease lease = owner.borrowRead(0, 64);
        owner.close(); d.beginClose();
        if (failure) nativeBorrowInject(3);
        nativeArmBorrowGate(1);
        AtomicReference<Throwable> result = new AtomicReference<>();
        Thread worker = new Thread(() -> { try { lease.close(); } catch (Throwable error) { result.set(error); } });
        worker.start(); check(nativeWaitBorrowGate(1), "adapter close tail not entered");
        check(counts()[2] == 0 && counts()[6] == 1 && d.state() == ManagedMemoryDomain.CLOSING,
                "domain closed before adapter close returned");
        lease.close(); // A concurrent repeat must not drop the first call's guard.
        check(lease.closeState() == NativeMemoryLease.CLOSE_RELEASING && counts()[6] == 1,
                "repeated close changed in-flight state");
        check(nativeReleaseBorrowGate(1), "adapter close tail gate timed out"); join(worker);
        if (failure) {
            check(result.get() instanceof IllegalStateException && result.get().getCause() instanceof java.io.IOException &&
                    d.state() == ManagedMemoryDomain.QUARANTINED && lease.closeState() == NativeMemoryLease.CLOSE_FAILED,
                    "adapter tail exception was not quarantined");
        } else {
            check(result.get() == null, "adapter close tail failed"); finish(d);
        }
    }
    private static void cancelAdapterTail(boolean failure) throws Exception {
        ManagedMemoryDomain d = ManagedMemoryDomain.system(4096);
        ManagedMemoryDomain.Allocation owner = d.allocate(64, 8);
        nativeBorrowInject(1); // Inject a Java exception before token claim.
        if (failure) nativeBorrowInject(2); // Fail after real cancellation.
        nativeArmBorrowGate(2);
        AtomicReference<Throwable> result = new AtomicReference<>();
        Thread worker = new Thread(() -> { try { owner.borrowRead(0, 64); }
            catch (Throwable error) { result.set(error); } });
        worker.start(); check(nativeWaitBorrowGate(2), "adapter cancel tail not entered");
        owner.close(); d.beginClose();
        check(counts()[2] == 0 && counts()[6] == 1 && d.state() == ManagedMemoryDomain.CLOSING,
                "domain closed before adapter cancellation returned");
        check(nativeReleaseBorrowGate(2), "adapter cancellation gate timed out"); join(worker);
        check(result.get() instanceof IllegalStateException, "injected claim exception missing");
        if (failure) check(d.state() == ManagedMemoryDomain.QUARANTINED, "failed cancellation not isolated");
        else finish(d);
    }
    private static void workerCap() throws Exception {
        final int maximum = ManagedMemoryDomain.MAX_CLEANUP_WORKERS;
        ManagedMemoryDomain[] domains = new ManagedMemoryDomain[maximum + 1];
        ManagedMemoryDomain.Allocation[] owners = new ManagedMemoryDomain.Allocation[maximum + 1];
        for (int i = 0; i < domains.length; ++i) {
            domains[i] = ManagedMemoryDomain.system(4096); owners[i] = domains[i].allocate(16, 8);
        }
        for (int i = 0; i < maximum; ++i) domains[i].beginClose();
        check(ManagedMemoryDomain.activeCleanupWorkers() == maximum, "worker cap count");
        rejectedStatus(domains[maximum]::beginClose, ManagedMemoryDomain.STATUS_WORKER_LIMIT,
                "worker cap admitted extra worker");
        check(domains[maximum].state() == ManagedMemoryDomain.CLOSING, "rejected close reopened admission");
        owners[0].close(); awaitState(domains[0], ManagedMemoryDomain.CLOSED);
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (ManagedMemoryDomain.activeCleanupWorkers() == maximum && System.nanoTime() - deadline < 0) Thread.sleep(2);
        domains[maximum].beginClose();
        for (int i = 1; i < owners.length; ++i) owners[i].close();
        for (ManagedMemoryDomain d : domains) finish(d);
        check(counts()[0] == 0 && counts()[1] == 0 && counts()[2] == 0, "worker cap cleanup");
    }
    public static void main(String[] args) throws Exception {
        check(args.length == 1, "one test case required");
        // Load the real test JNI composition before invoking test-only hooks.
        check(ManagedMemoryDomain.activeCleanupWorkers() == 0, "initial worker count");
        switch (args[0]) {
        case "normal": normal(false); break;
        case "arena": normal(true); break;
        case "worker-retry": workerRetry(); break;
        case "race-allocate": raceAllocate(); break;
        case "race-published": racePublished(); break;
        case "race-borrow": raceBorrow(); break;
        case "race-pin": racePin(); break;
        case "forget-state": forgetRace(4); break;
        case "forget-failure": forgetRace(7); break;
        case "forget-close": forgetRace(5); break;
        case "drop-retry": dropRetry(false); break;
        case "drop-exception": dropRetry(true); break;
        case "unknown-create": unknownCreate(); break;
        case "unknown-allocate": unknownAllocate(); break;
        case "unknown-borrow": unknownBorrow(false); break;
        case "unknown-registration": unknownBorrow(true); break;
        case "lease-release-failure": leaseReleaseFailure(false); break;
        case "quarantine-jni-failure": leaseReleaseFailure(true); break;
        case "java-only-quarantine": javaOnlyQuarantine(); break;
        case "checked-create": checkedCreate(false); break;
        case "checked-claim": checkedCreate(true); break;
        case "checked-allocate": checkedAllocate(false); break;
        case "checked-borrow": checkedBorrow(); break;
        case "checked-release": checkedRelease(); break;
        case "checked-quarantine": checkedAllocate(true); break;
        case "checked-lease-close": checkedLeaseClose(); break;
        case "guard-admission-retry": guardAdmissionRetry(); break;
        case "guard-enter-unknown": guardEnterUnknown(); break;
        case "guard-exit-delay": guardExit(false); break;
        case "guard-exit-failure": guardExit(true); break;
        case "guard-underflow": guardUnderflow(); break;
        case "observed-stale-state": observedClosedBeatsStaleState(); break;
        case "observed-closed": observedClosedRejectsMutation(); break;
        case "owner-tail": javaMutationTail(false, false); break;
        case "owner-tail-error": javaMutationTail(false, true); break;
        case "allocate-tail": javaMutationTail(true, false); break;
        case "allocate-tail-error": javaMutationTail(true, true); break;
        case "java-close-tail": javaCloseTail(false); break;
        case "java-close-tail-error": javaCloseTail(true); break;
        case "adapter-close-tail": closeAdapterTail(false); break;
        case "adapter-close-tail-error": closeAdapterTail(true); break;
        case "adapter-cancel-tail": cancelAdapterTail(false); break;
        case "adapter-cancel-tail-error": cancelAdapterTail(true); break;
        case "destroy-unknown": destroyUnknown(); break;
        case "worker-cap": workerCap(); break;
        case "unforgotten-exit": closedDomain(); break;
        default: throw new AssertionError("unknown test case");
        }
        System.out.println("PASS managed host case " + args[0]);
    }
}
