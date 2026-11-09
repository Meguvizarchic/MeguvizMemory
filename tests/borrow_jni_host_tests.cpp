#include <mgmem/android_borrow_jni.h>
#include <jni.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"failure at line %d: %s\n",__LINE__,#x); std::abort(); } } while (false)
#define JNI_NAME(method) Java_io_github_jichuo1_meguviz_memory_android_NativeMemoryLease_##method
extern "C" jlong JNICALL JNI_NAME(nativeClaim)(JNIEnv *,jclass,jlong);
extern "C" jint JNICALL JNI_NAME(nativeCopy)(JNIEnv *,jclass,jlong,jlong,jbyteArray,jint,jint,jboolean);
extern "C" jint JNICALL JNI_NAME(nativeClose)(JNIEnv *,jclass,jlong);
extern "C" jint JNICALL JNI_NAME(nativeTransfer)(JNIEnv *,jclass,jlong,jlong,jlong,jlong,jint);
extern "C" void mgmem_borrow_test_fail_staging_allocation();

namespace {
struct Array { std::vector<jbyte> data; };
thread_local bool pending = false;
std::atomic<int> fail_at{0}, calls_after_exception{0};
std::mutex gate_mutex;
std::condition_variable gate_changed;
bool block_read=false, entered=false, release_read=false;
bool block_write=false, release_write=false;
unsigned writes_entered=0;

jsize JNICALL length(JNIEnv *,jarray value) {
    if (pending) ++calls_after_exception;
    if (fail_at == 1) {pending=true; return 0;}
    return static_cast<jsize>(reinterpret_cast<Array *>(value)->data.size());
}
jboolean JNICALL exception(JNIEnv *) {return pending ? JNI_TRUE : JNI_FALSE;}
void JNICALL get_region(JNIEnv *,jbyteArray value,jsize start,jsize count,jbyte *out) {
    if (pending) ++calls_after_exception;
    if (fail_at == 2) {if(count) out[0]=42;pending=true; return;}
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (block_write) {++writes_entered;gate_changed.notify_all();gate_changed.wait(lock,[]{return release_write;});}
    }
    auto *array=reinterpret_cast<Array *>(value);
    std::memcpy(out,array->data.data()+start,static_cast<size_t>(count));
}
void JNICALL set_region(JNIEnv *,jbyteArray value,jsize start,jsize count,const jbyte *in) {
    if (pending) ++calls_after_exception;
    if (fail_at == 3) {pending=true; return;}
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        if (block_read) {entered=true;gate_changed.notify_all();gate_changed.wait(lock,[]{return release_read;});}
    }
    auto *array=reinterpret_cast<Array *>(value);
    std::memcpy(array->data.data()+start,in,static_cast<size_t>(count));
}
struct Env {
    JNINativeInterface_ table{};
    JNIEnv_ env{};
    Env() {table.GetArrayLength=&length;table.ExceptionCheck=&exception;
        table.GetByteArrayRegion=&get_region;table.SetByteArrayRegion=&set_region;env.functions=&table;}
};
struct Owner {
    std::atomic<int> pins{1};
    static void retain(void *p) {++static_cast<Owner *>(p)->pins;}
    static void release(void *p) {--static_cast<Owner *>(p)->pins;}
};
struct Fixture {
    mgmem_domain *domain=nullptr;
    mgmem_handle_v1 owner{};
    Owner lifetime;
    mgmem_domain_lifetime_v1 callbacks{sizeof(callbacks),1,&lifetime,&Owner::retain,&Owner::release};
    Fixture() {
        mgmem_domain_options_v1 opts{sizeof(opts),MGMEM_ABI_MAJOR,MGMEM_ABI_MINOR,0,MGMEM_SYSTEM,0,8192,0,nullptr};
        REQUIRE(mgmem_domain_create_v1(&opts,&domain)==MGMEM_OK);
        REQUIRE(mgmem_allocate_v1(domain,128,64,&owner)==MGMEM_OK);
        mgmem_borrow_v1 initial{};
        REQUIRE(mgmem_borrow_acquire_v1(domain,owner,0,128,MGMEM_WRITE,&initial)==MGMEM_OK);
        std::memset(initial.data,0x5a,128);
        REQUIRE(mgmem_borrow_release_v1(domain,initial.lease)==MGMEM_OK);
    }
    void finish() {
        REQUIRE(lifetime.pins==1);
        REQUIRE(mgmem_domain_collect_v1(domain)==MGMEM_OK);
        REQUIRE(mgmem_domain_drain_v1(domain,0)==MGMEM_OK);
        REQUIRE(mgmem_domain_destroy_v1(domain)==MGMEM_OK);
        domain=nullptr;
    }
};
jbyteArray as_array(Array &a) {return reinterpret_cast<jbyteArray>(&a);}
std::atomic<int> failing_releases{0};
mgmem_status external_retain(void *) {return MGMEM_OK;}
void external_release(void *) {++failing_releases;throw 7;}
}

int main() {
    Env environment; auto *env=&environment.env;
    {
        Fixture f; uint64_t id=9;
        REQUIRE(mgmem_android_borrow_register_v1(f.domain,&f.callbacks,f.owner,0,16,MGMEM_READ,0,&id)==MGMEM_INVALID_ARGUMENT && id==0);
        REQUIRE(f.lifetime.pins==1);
        REQUIRE(mgmem_android_borrow_register_v1(f.domain,&f.callbacks,f.owner,0,16,MGMEM_READ,MGMEM_ANDROID_BORROW_INITIALIZED,&id)==MGMEM_OK);
        REQUIRE(f.lifetime.pins==2);
        REQUIRE(mgmem_android_borrow_cancel_v1(id)==MGMEM_OK && f.lifetime.pins==1);
        REQUIRE(mgmem_android_borrow_cancel_v1(id)==MGMEM_STALE_HANDLE);
        REQUIRE(mgmem_release_v1(f.domain,f.owner)==MGMEM_OK);
        REQUIRE(mgmem_domain_begin_close_v1(f.domain)==MGMEM_OK);f.finish();
    }
    {
        Fixture f; uint64_t id=0;
        REQUIRE(mgmem_android_borrow_register_v1(f.domain,&f.callbacks,f.owner,4,16,MGMEM_READ,MGMEM_ANDROID_BORROW_INITIALIZED,&id)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,id)==16);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,id)==0);
        REQUIRE(mgmem_android_borrow_cancel_v1(id)==MGMEM_BUSY);
        REQUIRE(mgmem_release_v1(f.domain,f.owner)==MGMEM_OK);
        REQUIRE(mgmem_domain_begin_close_v1(f.domain)==MGMEM_OK);
        Array data{std::vector<jbyte>(20,0)};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),2,16,JNI_FALSE)==MGMEM_OK);
        for(int i=2;i<18;++i) REQUIRE(data.data[i]==0x5a);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),0,1,JNI_TRUE)==MGMEM_UNSUPPORTED);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,15,as_array(data),0,2,JNI_FALSE)==MGMEM_INVALID_ARGUMENT);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,16,as_array(data),20,0,JNI_FALSE)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,id)==MGMEM_OK && f.lifetime.pins==1);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),0,1,JNI_FALSE)==MGMEM_STALE_HANDLE);
        f.finish();
    }
    {
        Fixture f;uint64_t id=0;
        REQUIRE(mgmem_android_borrow_register_v1(f.domain,&f.callbacks,f.owner,0,8,MGMEM_WRITE,MGMEM_ANDROID_BORROW_INITIALIZED,&id)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,id)==-8);
        Array data{std::vector<jbyte>{1,2,3,4}};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,2,as_array(data),0,4,JNI_TRUE)==MGMEM_OK);
        Array output{std::vector<jbyte>(4,0)};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,2,as_array(output),0,4,JNI_FALSE)==MGMEM_OK && output.data==data.data);
        for (int stage=1;stage<=3;++stage) {
            Array before{std::vector<jbyte>(8,0)};
            REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(before),0,8,JNI_FALSE)==MGMEM_OK);
            fail_at=stage;pending=false;calls_after_exception=0;
            REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),0,1,stage==2?JNI_TRUE:JNI_FALSE)==MGMEM_PROVIDER_ERROR);
            REQUIRE(pending && calls_after_exception==0);pending=false;fail_at=0;
            Array after{std::vector<jbyte>(8,0)};
            REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(after),0,8,JNI_FALSE)==MGMEM_OK && before.data==after.data);
        }
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),0,
            MGMEM_ANDROID_BORROW_MAX_WRITE_BYTES+1,JNI_TRUE)==MGMEM_INVALID_ARGUMENT);
        Array before_oom{std::vector<jbyte>(8,0)},after_oom{std::vector<jbyte>(8,0)};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(before_oom),0,8,JNI_FALSE)==MGMEM_OK);
        mgmem_borrow_test_fail_staging_allocation();
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),0,4,JNI_TRUE)==MGMEM_OUT_OF_MEMORY);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(after_oom),0,8,JNI_FALSE)==MGMEM_OK && before_oom.data==after_oom.data);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(data),0,4,JNI_TRUE)==MGMEM_OK);
        REQUIRE(mgmem_release_v1(f.domain,f.owner)==MGMEM_OK);
        REQUIRE(mgmem_domain_begin_close_v1(f.domain)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,id)==MGMEM_OK);f.finish();
    }
    {
        Fixture f;uint64_t id=0;
        REQUIRE(mgmem_android_borrow_register_v1(f.domain,&f.callbacks,f.owner,0,16,MGMEM_READ,MGMEM_ANDROID_BORROW_INITIALIZED,&id)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,id)==16);
        REQUIRE(mgmem_release_v1(f.domain,f.owner)==MGMEM_OK);
        REQUIRE(mgmem_domain_begin_close_v1(f.domain)==MGMEM_OK);
        Array output{std::vector<jbyte>(16,0)};block_read=true;entered=false;release_read=false;
        std::atomic<bool> closed{false};
        std::thread reader([&]{REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,id,0,as_array(output),0,16,JNI_FALSE)==MGMEM_OK);});
        {std::unique_lock<std::mutex> lock(gate_mutex);gate_changed.wait(lock,[]{return entered;});}
        std::thread closer([&]{REQUIRE(JNI_NAME(nativeClose)(env,nullptr,id)==MGMEM_OK);closed=true;});
        REQUIRE(!closed && f.lifetime.pins==2);
        {std::lock_guard<std::mutex> lock(gate_mutex);release_read=true;gate_changed.notify_all();}
        reader.join();closer.join();block_read=false;
        REQUIRE(closed && f.lifetime.pins==1 && output.data[0]==0x5a);f.finish();
    }
    {
        // Hold all runtime staging permits inside concurrent JNI calls. A
        // seventeenth writer is rejected before mutating its native backing;
        // after release, quota is reusable and close waits for an active write.
        constexpr size_t chunk=MGMEM_ANDROID_BORROW_MAX_WRITE_BYTES;
        constexpr unsigned parallel=16;
        static_assert(parallel*chunk==MGMEM_ANDROID_BORROW_RUNTIME_STAGING_BYTES,"test expects 16 slots");
        mgmem_domain *domain=nullptr;
        mgmem_domain_options_v1 opts{sizeof(opts),MGMEM_ABI_MAJOR,MGMEM_ABI_MINOR,0,MGMEM_SYSTEM,0,24*1024*1024,0,nullptr};
        REQUIRE(mgmem_domain_create_v1(&opts,&domain)==MGMEM_OK);
        Owner owner;mgmem_domain_lifetime_v1 life{sizeof(life),1,&owner,&Owner::retain,&Owner::release};
        std::vector<uint64_t> ids(parallel+1);
        for (auto &id:ids) {
            mgmem_handle_v1 allocation{};mgmem_borrow_v1 init{};
            REQUIRE(mgmem_allocate_v1(domain,chunk,64,&allocation)==MGMEM_OK);
            REQUIRE(mgmem_borrow_acquire_v1(domain,allocation,0,chunk,MGMEM_WRITE,&init)==MGMEM_OK);
            std::memset(init.data,0x5a,chunk);
            REQUIRE(mgmem_borrow_release_v1(domain,init.lease)==MGMEM_OK);
            REQUIRE(mgmem_android_borrow_register_v1(domain,&life,allocation,0,chunk,MGMEM_WRITE,MGMEM_ANDROID_BORROW_INITIALIZED,&id)==MGMEM_OK);
            REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,id)==-static_cast<jlong>(chunk));
            REQUIRE(mgmem_release_v1(domain,allocation)==MGMEM_OK);
        }
        REQUIRE(mgmem_domain_begin_close_v1(domain)==MGMEM_OK);
        Array data{std::vector<jbyte>(chunk,7)};
        {std::lock_guard<std::mutex> lock(gate_mutex);block_write=true;release_write=false;writes_entered=0;}
        std::vector<std::thread> writers;
        for(unsigned i=0;i<parallel;++i) writers.emplace_back([&,i] {
            REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,ids[i],0,as_array(data),0,static_cast<jint>(chunk),JNI_TRUE)==MGMEM_OK);
        });
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            REQUIRE(gate_changed.wait_for(lock,std::chrono::seconds(10),[]{return writes_entered==parallel;}));
        }
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,ids.back(),0,as_array(data),0,static_cast<jint>(chunk),JNI_TRUE)==MGMEM_OUT_OF_BUDGET);
        Array untouched{std::vector<jbyte>(1,0)};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,ids.back(),0,as_array(untouched),0,1,JNI_FALSE)==MGMEM_OK && untouched.data[0]==0x5a);
        std::atomic<bool> closed{false};
        std::thread closer([&] {REQUIRE(JNI_NAME(nativeClose)(env,nullptr,ids[0])==MGMEM_OK);closed=true;});
        REQUIRE(!closed && owner.pins==parallel+2);
        {std::lock_guard<std::mutex> lock(gate_mutex);release_write=true;block_write=false;gate_changed.notify_all();}
        for(auto &thread:writers)thread.join();closer.join();
        REQUIRE(closed && owner.pins==parallel+1);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,ids.back(),0,as_array(data),0,static_cast<jint>(chunk),JNI_TRUE)==MGMEM_OK);
        for(unsigned i=1;i<ids.size();++i) REQUIRE(JNI_NAME(nativeClose)(env,nullptr,ids[i])==MGMEM_OK);
        REQUIRE(owner.pins==1 && mgmem_domain_collect_v1(domain)==MGMEM_OK &&
                mgmem_domain_drain_v1(domain,0)==MGMEM_OK && mgmem_domain_destroy_v1(domain)==MGMEM_OK);
    }
    {
        Fixture source, destination; uint64_t from=0, to=0;
        REQUIRE(mgmem_android_borrow_register_v1(source.domain,&source.callbacks,source.owner,0,32,MGMEM_READ,MGMEM_ANDROID_BORROW_INITIALIZED,&from)==MGMEM_OK);
        REQUIRE(mgmem_android_borrow_register_v1(destination.domain,&destination.callbacks,destination.owner,0,32,MGMEM_WRITE,MGMEM_ANDROID_BORROW_INITIALIZED,&to)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,from)==32 && JNI_NAME(nativeClaim)(env,nullptr,to)==-32);
        REQUIRE(mgmem_release_v1(source.domain,source.owner)==MGMEM_OK && mgmem_domain_begin_close_v1(source.domain)==MGMEM_OK);
        REQUIRE(mgmem_release_v1(destination.domain,destination.owner)==MGMEM_OK && mgmem_domain_begin_close_v1(destination.domain)==MGMEM_OK);
        Array cleared{std::vector<jbyte>(32,0)}, output{std::vector<jbyte>(32,0)};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,to,0,as_array(cleared),0,32,JNI_TRUE)==MGMEM_OK);
        // No array JNI function or staging allocation is needed for transfer.
        mgmem_borrow_test_fail_staging_allocation();
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,from,4,to,8,16)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,to,0,as_array(cleared),0,1,JNI_TRUE)==MGMEM_OUT_OF_MEMORY);
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,to,0,as_array(output),0,32,JNI_FALSE)==MGMEM_OK);
        for (unsigned i=0;i<32;++i) REQUIRE(output.data[i]==(i>=8 && i<24 ? 0x5a : 0));
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,to,0,from,0,1)==MGMEM_UNSUPPORTED);
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,from,31,to,0,2)==MGMEM_INVALID_ARGUMENT);
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,from,0,to,32,1)==MGMEM_INVALID_ARGUMENT);
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,from,0,to,0,MGMEM_ANDROID_BORROW_MAX_WRITE_BYTES+1)==MGMEM_INVALID_ARGUMENT);
        Array unchanged{std::vector<jbyte>(32,0)};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,to,0,as_array(unchanged),0,32,JNI_FALSE)==MGMEM_OK && unchanged.data==output.data);
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,from,32,to,32,0)==MGMEM_OK);
        Array pattern{std::vector<jbyte>{0,1,2,3,4,5,6,7}};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,to,0,as_array(pattern),0,8,JNI_TRUE)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,to,0,to,2,6)==MGMEM_OK);
        Array overlap{std::vector<jbyte>(8,0)}, expected{std::vector<jbyte>{0,1,0,1,2,3,4,5}};
        REQUIRE(JNI_NAME(nativeCopy)(env,nullptr,to,0,as_array(overlap),0,8,JNI_FALSE)==MGMEM_OK && overlap.data==expected.data);
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,from)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,from,0,to,0,1)==MGMEM_STALE_HANDLE);
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,to)==MGMEM_OK);
        source.finish();destination.finish();
    }
    {
        // Opposite transfer directions share the same token lock order.
        Fixture a,b; uint64_t x=0,y=0;
        REQUIRE(mgmem_android_borrow_register_v1(a.domain,&a.callbacks,a.owner,0,32,MGMEM_WRITE,MGMEM_ANDROID_BORROW_INITIALIZED,&x)==MGMEM_OK);
        REQUIRE(mgmem_android_borrow_register_v1(b.domain,&b.callbacks,b.owner,0,32,MGMEM_WRITE,MGMEM_ANDROID_BORROW_INITIALIZED,&y)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,x)==-32 && JNI_NAME(nativeClaim)(env,nullptr,y)==-32);
        std::thread forward([&] {for (unsigned i=0;i<1000;++i) REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,x,0,y,0,32)==MGMEM_OK);});
        std::thread reverse([&] {for (unsigned i=0;i<1000;++i) REQUIRE(JNI_NAME(nativeTransfer)(nullptr,nullptr,y,0,x,0,32)==MGMEM_OK);});
        forward.join();reverse.join();
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,x)==MGMEM_OK && JNI_NAME(nativeClose)(env,nullptr,y)==MGMEM_OK);
        REQUIRE(mgmem_release_v1(a.domain,a.owner)==MGMEM_OK && mgmem_domain_begin_close_v1(a.domain)==MGMEM_OK);
        REQUIRE(mgmem_release_v1(b.domain,b.owner)==MGMEM_OK && mgmem_domain_begin_close_v1(b.domain)==MGMEM_OK);
        a.finish();b.finish();
    }
    {
        // A violating origin callback can consume a lease before returning an
        // error. The adapter must not release the owner pin or retry the lease.
        mgmem_domain *domain=nullptr;
        mgmem_domain_options_v1 opts{sizeof(opts),MGMEM_ABI_MAJOR,MGMEM_ABI_MINOR,0,MGMEM_SYSTEM,0,8192,0,nullptr};
        REQUIRE(mgmem_domain_create_v1(&opts,&domain)==MGMEM_OK);
        static unsigned char bytes[32]{};
        mgmem_external_buffer_v1 ext{sizeof(ext),MGMEM_ABI_MAJOR,MGMEM_ABI_MINOR,0,bytes,sizeof(bytes),1,
            923,MGMEM_READ,0,nullptr,&external_retain,&external_release,mgmem_runtime_origin_v1()};
        mgmem_handle_v1 allocation{};
        REQUIRE(mgmem_import_external_v1(domain,&ext,&allocation)==MGMEM_OK);
        Owner owner;mgmem_domain_lifetime_v1 life{sizeof(life),1,&owner,&Owner::retain,&Owner::release};
        uint64_t id=0;
        REQUIRE(mgmem_android_borrow_register_v1(domain,&life,allocation,0,32,MGMEM_READ,MGMEM_ANDROID_BORROW_INITIALIZED,&id)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClaim)(env,nullptr,id)==32);
        REQUIRE(mgmem_release_v1(domain,allocation)==MGMEM_OK);
        REQUIRE(mgmem_domain_begin_close_v1(domain)==MGMEM_OK);
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,id)==MGMEM_PROVIDER_ERROR);
        REQUIRE(failing_releases==1 && owner.pins==2);
        REQUIRE(JNI_NAME(nativeClose)(env,nullptr,id)==MGMEM_STALE_HANDLE && failing_releases==1 && owner.pins==2);
        // The domain and pin remain intentionally quarantined until process exit.
    }
    std::puts("borrow_jni_host: PASS");
}
