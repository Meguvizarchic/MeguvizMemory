package io.github.jichuo1.meguviz.memory.android;

import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/** Runs against a host JVM and real core/JNI, not an Android-device test. */
public final class NativeMemoryLeaseJvmTest {
    static { System.loadLibrary("mgmem_pressure_jni"); }
    private static native long create(boolean writable);
    private static native long createFailingClose();
    private static native boolean finish();
    private static native boolean quarantined();
    private static native boolean setReleaseHook(Runnable hook);
    private static void require(boolean value) { if (!value) throw new AssertionError(); }
    private static void rejected(Runnable call, Class<? extends Throwable> expected) {
        try { call.run(); } catch (Throwable error) {
            if (expected.isInstance(error)) return;
            throw new AssertionError(error);
        }
        throw new AssertionError("expected " + expected.getSimpleName());
    }
    private static void callbackCanObserveClosed(NativeMemoryLease callbackLease,boolean expectFailure) throws Exception {
        CountDownLatch started=new CountDownLatch(1),done=new CountDownLatch(1);
        AtomicBoolean completedInRelease=new AtomicBoolean();
        AtomicReference<Throwable> workerError=new AtomicReference<>();
        AtomicReference<Thread> workerRef=new AtomicReference<>();
        require(setReleaseHook(() -> {
            Thread worker=new Thread(() -> {
                started.countDown();
                try {
                    require(callbackLease.closeState()==NativeMemoryLease.CLOSE_RELEASING);
                    require(callbackLease.closeFailureStatus()==NativeMemoryLease.CLOSE_FAILURE_UNKNOWN);
                    callbackLease.close();
                    rejected(() -> callbackLease.read(0,new byte[1],0,1),IllegalStateException.class);
                }
                catch(Throwable error) {workerError.set(error);}
                finally {done.countDown();}
                // Observe the terminal volatile state from this distinct thread
                // after the release callback has allowed nativeClose to return.
                try {
                    long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);
                    while(callbackLease.closeState()==NativeMemoryLease.CLOSE_RELEASING && System.nanoTime()<deadline)
                        Thread.yield();
                    require(callbackLease.closeState()==(expectFailure ? NativeMemoryLease.CLOSE_FAILED : NativeMemoryLease.CLOSE_RELEASED));
                    require(callbackLease.closeFailureStatus()==(expectFailure ? 9 : NativeMemoryLease.CLOSE_FAILURE_UNKNOWN));
                } catch(Throwable error) {workerError.compareAndSet(null,error);}
            },"borrow-close-callback-probe");
            workerRef.set(worker);worker.start();
            try {
                if(started.await(2,TimeUnit.SECONDS)) completedInRelease.set(done.await(2,TimeUnit.SECONDS));
            } catch(InterruptedException error) {
                Thread.currentThread().interrupt();workerError.set(error);
            }
        }));
        if(expectFailure) rejected(callbackLease::close,IllegalStateException.class);
        else callbackLease.close();
        require(setReleaseHook(null));
        Thread worker=workerRef.get();require(worker!=null);worker.join(3000);
        require(!worker.isAlive() && workerError.get()==null);
        require(completedInRelease.get());
    }

    public static void main(String[] unused) throws Exception {
        final long token=create(false);require(token>0);
        NativeMemoryLease read=NativeMemoryLease.fromNativeRegistration(token);
        require(read.length()==16 && !read.isWritable());
        require(read.closeState()==NativeMemoryLease.CLOSE_OPEN && read.closeFailureStatus()==NativeMemoryLease.CLOSE_FAILURE_UNKNOWN);
        rejected(() -> NativeMemoryLease.fromNativeRegistration(token),IllegalStateException.class);
        byte[] output=new byte[16];read.read(0,output,0,output.length);
        byte[] expected=new byte[16];Arrays.fill(expected,(byte)0x5a);require(Arrays.equals(output,expected));
        rejected(() -> read.write(0,new byte[1],0,1),IllegalStateException.class);
        rejected(() -> read.read(Long.MAX_VALUE,output,0,1),IndexOutOfBoundsException.class);
        rejected(() -> read.read(0,output,Integer.MAX_VALUE,1),IndexOutOfBoundsException.class);
        read.read(16,output,16,0);
        rejected(() -> read.copyTo(read,0,0,1),IllegalStateException.class);
        read.close();read.close();
        require(read.closeState()==NativeMemoryLease.CLOSE_RELEASED && read.closeFailureStatus()==NativeMemoryLease.CLOSE_FAILURE_UNKNOWN);
        rejected(() -> read.read(0,output,0,1),IllegalStateException.class);
        require(finish());

        long writeToken=create(true);require(writeToken>0);
        NativeMemoryLease write=NativeMemoryLease.fromNativeRegistration(writeToken);
        require(write.length()==16 && write.isWritable());
        byte[] input={1,2,3,4};write.write(3,input,0,input.length);
        byte[] actual=new byte[4];write.read(3,actual,0,4);require(Arrays.equals(actual,input));
        byte[] sequence={0,1,2,3,4,5,6,7};write.write(0,sequence,0,sequence.length);
        write.copyTo(write,0,2,6);
        byte[] moved=new byte[8];write.read(0,moved,0,8);
        require(Arrays.equals(moved,new byte[]{0,1,0,1,2,3,4,5}));
        write.copyTo(write,2,0,6);write.read(0,moved,0,8);
        require(Arrays.equals(moved,new byte[]{0,1,2,3,4,5,4,5}));
        write.copyTo(write,16,16,0);
        rejected(() -> write.copyTo(write,Long.MAX_VALUE,0,1),IndexOutOfBoundsException.class);
        rejected(() -> write.copyTo(write,0,16,1),IndexOutOfBoundsException.class);
        rejected(() -> write.copyTo(write,0,0,NativeMemoryLease.MAX_COPY_BYTES+1),IllegalArgumentException.class);
        write.close();rejected(() -> write.copyTo(write,0,0,1),IllegalStateException.class);require(finish());

        long callbackToken=create(false);require(callbackToken>0);
        callbackCanObserveClosed(NativeMemoryLease.fromNativeRegistration(callbackToken),false);
        require(finish());

        long exceptionToken=create(false);require(exceptionToken>0);
        NativeMemoryLease exceptionLease=NativeMemoryLease.fromNativeRegistration(exceptionToken);
        RuntimeException injected=new IllegalStateException("test-only pending JNI exception");
        require(setReleaseHook(() -> {throw injected;}));
        try {exceptionLease.close();throw new AssertionError("expected the original JNI exception");}
        catch(IllegalStateException error) {require(error==injected);}
        require(exceptionLease.closeState()==NativeMemoryLease.CLOSE_FAILED &&
                exceptionLease.closeFailureStatus()==NativeMemoryLease.CLOSE_FAILURE_UNKNOWN);
        require(setReleaseHook(null));exceptionLease.close();
        // This particular test callback decrements its pin before throwing;
        // a real JNI exception alone cannot establish that cleanup succeeded.
        require(finish());

        long failingToken=createFailingClose();require(failingToken>0);
        NativeMemoryLease failing=NativeMemoryLease.fromNativeRegistration(failingToken);
        callbackCanObserveClosed(failing,true);
        failing.close(); // The consumed error cannot be retried.
        require(quarantined()); // Deliberately retained until this test JVM exits.
        System.out.println("borrow_jni_real_jvm: PASS");
    }
}
