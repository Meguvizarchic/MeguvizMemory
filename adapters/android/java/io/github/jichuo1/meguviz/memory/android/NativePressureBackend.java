package io.github.jichuo1.meguviz.memory.android;

import java.util.concurrent.atomic.AtomicBoolean;
import android.os.Looper;

/** Narrow JNI backend: registered IDs are not native pointers. Native close
 * schedules final controller cleanup; it never joins a worker on the caller. */
public final class NativePressureBackend implements MemoryPressureAdapter.Backend, AutoCloseable {
    static {
        // See NativeMemoryLease: pure-Java unit tests may skip the load.
        if (!Boolean.getBoolean("io.github.jichuo1.meguviz.memory.skipNativeLoad"))
            System.loadLibrary("mgmem_pressure_jni");
    }
    private final long registration;
    private final AtomicBoolean closing = new AtomicBoolean();
    private static final int CLOSE_OPEN = 0, CLOSE_ADMITTING = 1, CLOSE_ACCEPTED = 2, CLOSE_UNKNOWN = 3;
    private final Object closeAdmission = new Object();
    private int closePhase = CLOSE_OPEN;
    private final Object completion = new Object();
    private final long[] completedSnapshot = new long[16];
    private boolean completed;
    private int completionStatus;
    private NativePressureBackend(long normal,long moderate,long critical,long chunk) {
        this.registration = nativeCreate(normal,moderate,critical,chunk,this,completedSnapshot);
        if (registration <= 0) throw new IllegalStateException("native registration failed");
    }
    private NativePressureBackend(long registration) {
        if (registration <= 0) throw new IllegalStateException("native registration failed");
        this.registration = registration;
        bind();
    }
    private void bind() {
        if (registration <= 0) throw new IllegalStateException("native registration failed");
        int status=nativeBind(registration,this,completedSnapshot);
        if (status!=0) throw new IllegalStateException("native Java binding failed: "+status);
    }
    public static NativePressureBackend createArena(long normal, long moderate, long critical, long chunk) {
        if (normal <= 0 || moderate < 0 || critical < 0 || critical > moderate || moderate > normal || chunk <= 0)
            throw new IllegalArgumentException("invalid budgets/chunk");
        return new NativePressureBackend(normal,moderate,critical,chunk);
    }
    /** Token returned by mgmem_android_pressure_register_v1 in this same JNI
     * runtime; application native owner lifetime is pinned until cleanup ends. */
    public static NativePressureBackend fromNativeRegistration(long registration) {
        if (!nativeExists(registration)) throw new IllegalArgumentException("stale registration");
        return new NativePressureBackend(registration);
    }
    @Override public int notifyPressure(int pressure) {
        if (closing.get()) return 7;
        return nativeNotify(registration,pressure);
    }
    @Override public long[] snapshot() {
        if (closing.get()) nativePollCompletion(registration);
        synchronized (completion) { if (completed) return completedSnapshot.clone(); }
        return nativeSnapshot(registration);
    }
    public static String logicalUnavailableReason(long[] values,int index) {
        if (values==null) return "NATIVE_OBSERVATION_BUSY_OR_UNAVAILABLE";
        if (index<0 || index>=values.length) throw new IllegalArgumentException("unknown logical column");
        if (values[index]>=0) return null;
        // Controller/domain states are bounded integers, so both -1 with a
        // completed error uniquely mark an unavailable final snapshot rather
        // than an individual unsigned counter exceeding Java's range.
        if (values.length>=16 && values[0]==-1 && values[1]==-1 && values[15]==1 && values[13]!=0)
            return "NATIVE_SNAPSHOT_UNAVAILABLE_AFTER_CLEANUP_ERROR";
        return "UNSIGNED_NATIVE_COUNTER_EXCEEDS_JAVA_LONG";
    }
    /** A normal return confirms cleanup admission, not cleanup completion.
     * Concurrent or same-thread reentry while admission is in progress throws
     * a retryable exception instead of reporting unconfirmed success. If JNI
     * throws, admission is unknown and must not be blindly attempted again. */
    @Override public void beginClose() {
        synchronized (closeAdmission) {
            if (closePhase == CLOSE_ACCEPTED) return;
            if (closePhase == CLOSE_ADMITTING) throw new IllegalStateException("cleanup admission in progress; retry");
            if (closePhase == CLOSE_UNKNOWN) throw new IllegalStateException("cleanup admission outcome unknown");
            closePhase = CLOSE_ADMITTING;
            closing.set(true);
        }
        int status = -1;
        boolean returned = false;
        try {
            status = nativeBeginClose(registration);
            returned = true;
        } finally {
            synchronized (closeAdmission) {
                // An external unregister may already have completed and
                // published its callback while this JNI call was in flight.
                // Do not erase that independent confirmation on its return.
                if (closePhase != CLOSE_ACCEPTED) {
                    if (!returned) closePhase = CLOSE_UNKNOWN;
                    else if (status == 0) closePhase = CLOSE_ACCEPTED;
                    else { closePhase = CLOSE_OPEN; closing.set(false); }
                }
            }
        }
        if (status != 0) throw new IllegalStateException("cleanup admission failed: " + status);
    }
    @Override public boolean awaitClosed(long timeoutMillis) {
        if (timeoutMillis < 0 || timeoutMillis > 60_000) throw new IllegalArgumentException("bounded timeout required");
        if (timeoutMillis>0 && Looper.myLooper()==Looper.getMainLooper())
            throw new IllegalStateException("positive close waits require a worker thread");
        if (!closing.get()) return false;
        nativePollCompletion(registration);
        long deadline=System.nanoTime()+timeoutMillis*1_000_000L;
        synchronized(completion) {
            while(!completed && timeoutMillis>0) {
                long remaining=deadline-System.nanoTime();if(remaining<=0)break;
                try {completion.wait(remaining/1_000_000L,(int)(remaining%1_000_000L));}
                catch(InterruptedException error) {Thread.currentThread().interrupt();return false;}
            }
        }
        nativePollCompletion(registration);
        synchronized(completion) {return completed && completionStatus==0;}
    }
    private void onNativeClosed(int status) {
        synchronized (closeAdmission) { closePhase = CLOSE_ACCEPTED; closing.set(true); }
        synchronized(completion) {completionStatus=status;completed=true;completion.notifyAll();}
    }
    @Override public void close() { beginClose(); }
    // Diagnostic leases expose no address or Java ByteBuffer. Their separate
    // owner registry lets the last lease finish even after backend close.
    public LeaseProbe holdRetiringLeaseForDiagnostics(long bytes) {
        if (closing.get() || bytes <= 0) throw new IllegalStateException("backend closed or empty probe");
        return new LeaseProbe(nativeHoldProbe(registration,bytes));
    }
    public static final class LeaseProbe implements AutoCloseable {
        private long token;
        private LeaseProbe(long token) { if (token <= 0) throw new IllegalStateException("probe admission failed"); this.token=token; }
        public synchronized boolean readable() { return token != 0 && nativeReadProbe(token); }
        @Override public synchronized void close() { if (token != 0) {nativeReleaseProbe(token);token=0;} }
    }
    private static native long nativeCreate(long normal,long moderate,long critical,long chunk,NativePressureBackend listener,long[] finalSnapshot);
    private static native int nativeBind(long token,NativePressureBackend listener,long[] finalSnapshot);
    private static native boolean nativeExists(long token);
    private static native int nativeNotify(long token,int pressure);
    private static native long[] nativeSnapshot(long token);
    private static native int nativeBeginClose(long token);
    private static native void nativePollCompletion(long token);
    private static native long nativeHoldProbe(long token,long bytes);
    private static native boolean nativeReadProbe(long probe);
    private static native void nativeReleaseProbe(long probe);
}
