#include <cstdarg>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <mgmem/pressure.h>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace mgmem_jni_close_test {
void before_create();
void before_detach(std::thread *worker);
}
// A narrow test-only seam. The worker/controller and Memory remain real;
// only the JNI reaper's observation can be made to fail or report bad data.
namespace {
std::atomic<int> injected_snapshot_mode{0};
mgmem_status test_pressure_snapshot(mgmem_pressure_controller *control,mgmem_pressure_snapshot_v1 *out,uint32_t size) {
    const auto status=mgmem_pressure_snapshot_v1_get(control,out,size);
    if(status!=MGMEM_OK)return status;
    const auto mode=injected_snapshot_mode.load();
    if(mode==1)return MGMEM_PROVIDER_ERROR;
    if(mode==2)out->state=MGMEM_OPEN;
    if(mode==3)out->domain.struct_size=0;
    return MGMEM_OK;
}
}
#define mgmem_pressure_snapshot_v1_get test_pressure_snapshot
#define MGMEM_JNI_CLOSE_TEST_HOOKS 1
// Compile the real production composition with JDK JNI headers. Function tables
// below emulate only VM operations; the pressure worker and Memory are real.
#include "../adapters/android/native/pressure_jni.cpp"
#undef MGMEM_JNI_CLOSE_TEST_HOOKS
#undef mgmem_pressure_snapshot_v1_get

namespace mgmem_jni_close_test {
std::mutex gate;
std::condition_variable changed;
bool block_create=false,create_entered=false,release_create=false,fail_create=false,fail_detach=false;
std::thread *retained_worker=nullptr;
void before_create() {
    std::unique_lock<std::mutex> lock(gate);
    if(!block_create)return;
    create_entered=true;changed.notify_all();
    if(!changed.wait_for(lock,std::chrono::seconds(3),[]{return release_create;}))throw std::bad_alloc();
    if(fail_create)throw std::bad_alloc();
}
void before_detach(std::thread *worker) {
    std::lock_guard<std::mutex> lock(gate);
    if(fail_detach) {retained_worker=worker;throw std::runtime_error("injected detach failure");}
}
}

namespace test {
struct Fixture {
    std::shared_ptr<Bridge> state;
    u64 id=100;
    std::atomic<unsigned> calls{0},global_deletes{0},attach_calls{0},detach_calls{0};
    std::atomic<unsigned> forbidden_pending_calls{0},global_attempts{0},global_created{0};
    std::atomic<int> fail_binding_step{-1},callback_status{-1};
    std::atomic<bool> jni_unlocked{true},snapshot_reentered{true},foreign_unlocked{false};
    std::atomic<bool> detached{false},attach_fails{false},throw_once{false};
    std::atomic<bool> block_callback{false},entered{false},resume{false};
    std::mutex gate;std::condition_variable changed;
    std::atomic<unsigned> lifetime_retains{0},lifetime_releases{0};
    std::mutex values_mutex;std::array<jlong,16> values{};
    bool foreign_bind=false,foreign_snapshot=false;
    std::atomic<bool> foreign_actual_reentry{false};
};
Fixture *active=nullptr;
thread_local bool pending_exception=false;
JNINativeInterface_ env_functions{};
JNIInvokeInterface_ vm_functions{};
JNIEnv env{&env_functions};JavaVM vm{&vm_functions};
void operation() {
    if(!active || !active->state)return;
    if(active->state->mutex.try_lock())active->state->mutex.unlock();
    else active->jni_unlocked=false;
}
bool forbidden_pending() {
    if(!pending_exception)return false;
    ++active->forbidden_pending_calls;return true;
}
jint JNICALL get_env(JavaVM *,void **out,jint) {
    operation();*out=nullptr;
    if(active->detached)return JNI_EDETACHED;
    *out=&env;return JNI_OK;
}
jint JNICALL attach(JavaVM *,void **out,void *) {
    operation();++active->attach_calls;*out=nullptr;
    if(active->attach_fails)return JNI_ERR;
    *out=&env;return JNI_OK;
}
jint JNICALL detach(JavaVM *) {operation();++active->detach_calls;return JNI_OK;}
jsize JNICALL length(JNIEnv *,jarray) {
    if(forbidden_pending())return 0;
    operation();if(active->fail_binding_step==0){pending_exception=true;return 0;}return 16;
}
jobject JNICALL new_global(JNIEnv *,jobject obj) {
    if(forbidden_pending())return nullptr;
    operation();const auto step=++active->global_attempts;
    if(active->fail_binding_step==static_cast<int>(step)){pending_exception=true;return nullptr;}
    ++active->global_created;return obj;
}
void JNICALL delete_global(JNIEnv *,jobject) {operation();++active->global_deletes;}
jclass JNICALL object_class(JNIEnv *,jobject) {
    if(forbidden_pending())return nullptr;
    operation();if(active->fail_binding_step==3){pending_exception=true;return nullptr;}return reinterpret_cast<jclass>(4);
}
jmethodID JNICALL method(JNIEnv *,jclass,const char *,const char *) {
    if(forbidden_pending())return nullptr;
    operation();if(active->fail_binding_step==4){pending_exception=true;return nullptr;}return reinterpret_cast<jmethodID>(3);
}
jint JNICALL java_vm(JNIEnv *,JavaVM **out) {
    if(forbidden_pending()){*out=nullptr;return JNI_ERR;}
    operation();if(active->fail_binding_step==5){pending_exception=true;*out=nullptr;return JNI_ERR;}
    *out=&vm;return JNI_OK;
}
jclass JNICALL find_class(JNIEnv *,const char *) {if(forbidden_pending())return nullptr;operation();return reinterpret_cast<jclass>(4);}
jint JNICALL throw_new(JNIEnv *,jclass,const char *) {if(forbidden_pending())return JNI_ERR;operation();pending_exception=true;return JNI_OK;}
jlongArray JNICALL new_array(JNIEnv *,jsize) {if(forbidden_pending())return nullptr;operation();return reinterpret_cast<jlongArray>(5);}
void JNICALL set_array(JNIEnv *,jlongArray,jsize,jsize size,const jlong *values) {
    if(forbidden_pending())return;
    operation();if(size==16){std::lock_guard<std::mutex> lock(active->values_mutex);std::copy(values,values+16,active->values.begin());}
}
jboolean JNICALL exception_check(JNIEnv *) {operation();return pending_exception?JNI_TRUE:JNI_FALSE;}
void JNICALL exception_clear(JNIEnv *) {operation();pending_exception=false;}
void JNICALL call(JNIEnv *,jobject,jmethodID,va_list args) {
    if(forbidden_pending())return;
    operation();++active->calls;active->callback_status=va_arg(args,jint);
    const auto result=JNI_NAME(nativeSnapshot)(&env,nullptr,static_cast<jlong>(active->id));
    if(!result)active->snapshot_reentered=false;
    // Exercise a nested completion poll while this publisher still owns pins.
    JNI_NAME(nativePollCompletion)(&env,nullptr,static_cast<jlong>(active->id));
    if(active->throw_once.exchange(false))pending_exception=true;
    if(active->block_callback) {
        std::unique_lock<std::mutex> lock(active->gate);
        active->entered=true;active->changed.notify_all();
        active->changed.wait_for(lock,std::chrono::seconds(2),[]{return active->resume.load();});
    }
}
void initialize() {
    vm_functions.GetEnv=&get_env;vm_functions.AttachCurrentThread=&attach;vm_functions.DetachCurrentThread=&detach;
    env_functions.GetArrayLength=&length;env_functions.NewGlobalRef=&new_global;env_functions.DeleteGlobalRef=&delete_global;
    env_functions.GetObjectClass=&object_class;env_functions.GetMethodID=&method;env_functions.GetJavaVM=&java_vm;
    env_functions.NewLongArray=&new_array;env_functions.SetLongArrayRegion=&set_array;
    env_functions.ExceptionCheck=&exception_check;env_functions.ExceptionClear=&exception_clear;
    env_functions.FindClass=&find_class;env_functions.ThrowNew=&throw_new;
    env_functions.CallVoidMethodV=&call;
}
void make_done(Fixture &f,mgmem_status status=MGMEM_OK) {
    f.state=std::make_shared<Bridge>();f.state->ready=true;f.state->closing=true;f.state->done=true;
    f.state->close_status=status;f.state->final_snapshot.state=MGMEM_CLOSED;f.state->final_snapshot.domain.state=MGMEM_CLOSED;
    f.state->final_snapshot_available=true;
    f.state->vm=&vm;f.state->listener=reinterpret_cast<jobject>(1);
    f.state->final_buffer=reinterpret_cast<jlongArray>(2);f.state->completion_method=reinterpret_cast<jmethodID>(3);
    std::lock_guard<std::mutex> lock(registry_mutex);bridges.emplace(f.id,f.state);
}
bool exists(u64 id) {std::lock_guard<std::mutex> lock(registry_mutex);return bridges.count(id)!=0;}
void lifetime_retain(void *p) {++static_cast<Fixture *>(p)->lifetime_retains;}
void lifetime_release(void *p) {
    auto &f=*static_cast<Fixture *>(p);
    if(f.foreign_bind || f.foreign_snapshot) {
        // This branch actually enters the public JNI function unconditionally.
        // Old nativeBind deadlocks on the mutex already held by the reaper;
        // the runner owns a separate process and captures a bounded timeout.
        if(f.foreign_bind)f.foreign_actual_reentry=JNI_NAME(nativeBind)(&env,nullptr,static_cast<jlong>(f.id),
            reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2))==MGMEM_BUSY;
        else f.foreign_actual_reentry=JNI_NAME(nativeSnapshot)(&env,nullptr,static_cast<jlong>(f.id))!=nullptr;
        ++f.lifetime_releases;return;
    }
    if(f.state->mutex.try_lock()) {
        f.state->mutex.unlock();f.foreign_unlocked=true;
        // A real JNI entry can reject a closing bind without deadlocking.
        // The old implementation fails the try-lock and never enters this
        // blocking entry, keeping its counterexample bounded.
        const auto result=JNI_NAME(nativeBind)(&env,nullptr,static_cast<jlong>(f.id),
            reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2));
        if(result!=MGMEM_BUSY)f.foreign_unlocked=false;
    }
    ++f.lifetime_releases;
}
void throwing_lifetime_release(void *p) {
    ++static_cast<Fixture *>(p)->lifetime_releases;
    throw std::runtime_error("injected lifetime callback violation");
}
template <typename Predicate> bool bounded_wait(Predicate predicate,int milliseconds) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    do {if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    while(std::chrono::steady_clock::now()<end);
    return predicate();
}
void close_test_release(void *p) {
    auto &f=*static_cast<Fixture *>(p);
    ++f.lifetime_releases;
    if(!mgmem_jni_close_test::fail_detach)return;
    std::unique_lock<std::mutex> lock(f.gate);
    f.entered=true;f.changed.notify_all();
    if(!f.changed.wait_for(lock,std::chrono::seconds(3),[&]{return f.resume.load();}))std::_Exit(8);
}
bool close_admission_case(Fixture &f,bool fail_create,bool fail_detach) {
    mgmem_domain *domain=nullptr;
    mgmem_domain_options_v1 options{sizeof(options),1,1,0,MGMEM_SYSTEM,0,4096,0,nullptr};
    if(mgmem_domain_create_v1(&options,&domain)!=MGMEM_OK)return false;
    mgmem_domain_lifetime_v1 life{sizeof(life),1,&f,&lifetime_retain,&close_test_release};
    mgmem_pressure_options_v1 policy{sizeof(policy),1,4096,2048,0,1,0};
    if(mgmem_android_pressure_register_v1(domain,&life,&policy,&f.id)!=MGMEM_OK)return false;
    f.state=bridge(f.id);
    {
        std::lock_guard<std::mutex> lock(mgmem_jni_close_test::gate);
        mgmem_jni_close_test::block_create=true;
        mgmem_jni_close_test::create_entered=false;
        mgmem_jni_close_test::release_create=false;
        mgmem_jni_close_test::fail_create=fail_create;
        mgmem_jni_close_test::fail_detach=fail_detach;
        mgmem_jni_close_test::retained_worker=nullptr;
    }
    mgmem_status first=MGMEM_PROVIDER_ERROR;
    std::thread caller([&]{first=mgmem_android_pressure_unregister_v1(f.id);});
    bool entered=false;
    {
        std::unique_lock<std::mutex> lock(mgmem_jni_close_test::gate);
        entered=mgmem_jni_close_test::changed.wait_for(lock,std::chrono::seconds(2),[]{return mgmem_jni_close_test::create_entered;});
    }
    const auto concurrent=mgmem_android_pressure_unregister_v1(f.id);
    const auto during_notify=JNI_NAME(nativeNotify)(nullptr,nullptr,static_cast<jlong>(f.id),MGMEM_PRESSURE_NORMAL);
    {
        std::lock_guard<std::mutex> lock(mgmem_jni_close_test::gate);
        mgmem_jni_close_test::release_create=true;mgmem_jni_close_test::changed.notify_all();
    }
    caller.join();
    bool retry=true;
    if(fail_create) {
        const auto after_failure=JNI_NAME(nativeNotify)(nullptr,nullptr,static_cast<jlong>(f.id),MGMEM_PRESSURE_NORMAL);
        {
            std::lock_guard<std::mutex> lock(mgmem_jni_close_test::gate);
            mgmem_jni_close_test::block_create=false;
        }
        retry=first==MGMEM_OUT_OF_MEMORY && after_failure!=MGMEM_IS_CLOSED &&
              mgmem_android_pressure_unregister_v1(f.id)==MGMEM_OK;
    } else if(fail_detach) {
        const bool release_entered=bounded_wait([&]{return f.entered.load();},2000);
        const auto accepted_again=mgmem_android_pressure_unregister_v1(f.id);
        retry=first==MGMEM_OK && release_entered && accepted_again==MGMEM_OK &&
              mgmem_jni_close_test::retained_worker!=nullptr;
        f.resume=true;f.changed.notify_all();
    } else retry=first==MGMEM_OK;
    if(!bounded_wait([&]{return !exists(f.id);},3000))std::_Exit(9);
    if(fail_detach) {
        // Test-only recovery of the deliberately retained thread object; real
        // production cannot safely infer detach outcome and does not retry.
        auto *worker=mgmem_jni_close_test::retained_worker;
        if(worker) {if(worker->joinable())worker->join();delete worker;}
    }
    const bool paired=f.lifetime_retains==1 && f.lifetime_releases==1;
    const bool clean=mgmem_domain_begin_close_v1(domain)==MGMEM_OK && mgmem_domain_collect_v1(domain)==MGMEM_OK &&
        mgmem_domain_drain_v1(domain,0)==MGMEM_OK && mgmem_domain_destroy_v1(domain)==MGMEM_OK;
    return entered && concurrent==MGMEM_BUSY && during_notify==MGMEM_IS_CLOSED && retry && paired && clean;
}
bool binding_exception_matrix() {
    for(int bind=0;bind!=2;++bind)for(int step=0;step!=6;++step) {
        Fixture f;active=&f;pending_exception=false;f.fail_binding_step=step;f.id=900+bind*10+step;
        if(bind) {
            f.state=std::make_shared<Bridge>();f.state->ready=true;
            std::lock_guard<std::mutex> lock(registry_mutex);bridges.emplace(f.id,f.state);
        }
        size_t before;
        {std::lock_guard<std::mutex> lock(registry_mutex);before=bridges.size();}
        const auto result=bind ? static_cast<jlong>(JNI_NAME(nativeBind)(&env,nullptr,static_cast<jlong>(f.id),
                                reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2)))
                               : JNI_NAME(nativeCreate)(&env,nullptr,4096,2048,0,256,
                                reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2));
        size_t after;
        {std::lock_guard<std::mutex> lock(registry_mutex);after=bridges.size();}
        const bool status=bind ? result==(step==0?MGMEM_INVALID_ARGUMENT:MGMEM_OUT_OF_MEMORY) : result==0;
        const bool preserved=pending_exception && f.forbidden_pending_calls==0 &&
            f.global_deletes==f.global_created && before==after;
        bool unbound=true;
        if(bind) {
            {std::lock_guard<std::mutex> lock(f.state->mutex);
                unbound=!f.state->listener && !f.state->final_buffer && !f.state->vm;}
            // The real external registration remains retryable after Java
            // handles the preserved exception; this does not create a domain.
            pending_exception=false;f.fail_binding_step=-1;f.global_attempts=0;
            const auto retry=JNI_NAME(nativeBind)(&env,nullptr,static_cast<jlong>(f.id),
                    reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2));
            unbound=unbound && retry==MGMEM_OK && f.state->listener && f.state->final_buffer && f.state->vm;
            env.DeleteGlobalRef(f.state->listener);env.DeleteGlobalRef(f.state->final_buffer);
            f.state->listener=nullptr;f.state->final_buffer=nullptr;
            {std::lock_guard<std::mutex> lock(registry_mutex);bridges.erase(f.id);}
            unbound=unbound && f.global_created==f.global_deletes;
        }
        if(!status || !preserved || !unbound) {
            std::cerr<<"binding exception case failed bind="<<bind<<" step="<<step
                     <<" result="<<result<<" forbidden="<<f.forbidden_pending_calls
                     <<" created="<<f.global_created<<" deleted="<<f.global_deletes<<"\n";
            return false;
        }
    }
    pending_exception=false;active=nullptr;return true;
}
bool snapshot_error_case(Fixture &f,bool owned,int mode) {
    active=&f;pending_exception=false;
    mgmem_domain *domain=nullptr;
    if(owned) {
        const auto id=JNI_NAME(nativeCreate)(&env,nullptr,4096,2048,0,256,
                            reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2));
        if(id<=0)return false;
        f.id=static_cast<u64>(id);f.state=bridge(f.id);
        if(!f.state || !f.state->owned)return false;
        domain=f.state->owned->domain;
    } else {
        mgmem_domain_options_v1 options{sizeof(options),1,1,0,MGMEM_SYSTEM,0,4096,0,nullptr};
        if(mgmem_domain_create_v1(&options,&domain)!=MGMEM_OK)return false;
        mgmem_domain_lifetime_v1 life{sizeof(life),1,&f,&lifetime_retain,&lifetime_release};
        mgmem_pressure_options_v1 policy{sizeof(policy),1,4096,2048,0,1,0};
        if(mgmem_android_pressure_register_v1(domain,&life,&policy,&f.id)!=MGMEM_OK)return false;
        f.state=bridge(f.id);
        if(JNI_NAME(nativeBind)(&env,nullptr,static_cast<jlong>(f.id),
            reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2))!=MGMEM_OK)return false;
    }
    f.state->closing.store(true);injected_snapshot_mode=mode;
    std::atomic<bool> finished{false};
    std::thread reaper([state=f.state,id=f.id,&finished]{reap(id,state);finished=true;});
    if(!bounded_wait([&]{return finished.load();},3000)) {
        std::cout<<"{\"case\":\"snapshot-error\",\"pass\":false,\"state\":\"REAPER_STILL_ACTIVE\"}\n"<<std::flush;
        std::_Exit(3);
    }
    reaper.join();injected_snapshot_mode=0;
    mgmem_pressure_controller *controller=nullptr;
    bool quarantined=false;
    {std::lock_guard<std::mutex> lock(f.state->mutex);
        controller=f.state->control;
        quarantined=f.state->done && f.state->close_status==MGMEM_PROVIDER_ERROR &&
                    !f.state->final_snapshot_available && controller &&
                    (!owned || (f.state->owned && f.state->owned->domain==domain));
    }
    mgmem_snapshot_v1 domain_snapshot{};
    quarantined=quarantined && exists(f.id) && f.calls==1 && f.callback_status==MGMEM_PROVIDER_ERROR &&
        f.global_deletes==0 && (owned || (f.lifetime_retains==1 && f.lifetime_releases==0)) &&
        mgmem_domain_snapshot_v1(domain,&domain_snapshot,sizeof(domain_snapshot))==MGMEM_OK && domain_snapshot.state==MGMEM_OPEN;
    {std::lock_guard<std::mutex> lock(f.values_mutex);
        for(size_t i=0;i<13;++i)quarantined=quarantined && f.values[i]==-1;
        quarantined=quarantined && f.values[13]==MGMEM_PROVIDER_ERROR && f.values[14]==(owned?1:0) && f.values[15]==1;
    }
    if(!quarantined) {
        // The old implementation might already have destroyed this domain;
        // never touch uncertain resources while recording a counterexample.
        std::cout<<"{\"case\":\"snapshot-error\",\"pass\":false,\"state\":\"UNSAFE_OR_FALSE_SUCCESS\"}\n"<<std::flush;
        std::_Exit(4);
    }
    // This is a controlled test-only observation fault: after reaper joined we
    // know controller is closed and can explicitly clean the fixture. No such
    // inference is made for production's real unavailable/error condition.
    {std::lock_guard<std::mutex> lock(f.state->mutex);f.state->control=nullptr;}
    bool clean=mgmem_pressure_destroy_v1(controller)==MGMEM_OK;
    clean=clean && (owned || f.lifetime_releases==1);
    if(owned){std::lock_guard<std::mutex> lock(f.state->mutex);f.state->owned->domain=nullptr;}
    clean=clean && mgmem_domain_begin_close_v1(domain)==MGMEM_OK && mgmem_domain_collect_v1(domain)==MGMEM_OK &&
        mgmem_domain_drain_v1(domain,0)==MGMEM_OK && mgmem_domain_destroy_v1(domain)==MGMEM_OK;
    {std::lock_guard<std::mutex> lock(registry_mutex);bridges.erase(f.id);}
    return clean;
}
bool auto_retry_case(Fixture &f,const std::string &test_case) {
    const bool error=test_case=="auto-error-quarantine";
    const bool racing=test_case=="auto-concurrent-poll";
    const bool java_failure=test_case=="auto-java-recovery";
    mgmem_domain *domain=nullptr;
    mgmem_domain_options_v1 options{sizeof(options),1,1,0,MGMEM_SYSTEM,0,4096,0,nullptr};
    if(mgmem_domain_create_v1(&options,&domain)!=MGMEM_OK)return false;
    mgmem_domain_lifetime_v1 life{sizeof(life),1,&f,&lifetime_retain,
        error?&throwing_lifetime_release:&lifetime_release};
    mgmem_pressure_options_v1 policy{sizeof(policy),1,4096,2048,0,1,0};
    if(mgmem_android_pressure_register_v1(domain,&life,&policy,&f.id)!=MGMEM_OK)return false;
    f.state=bridge(f.id);
    if(JNI_NAME(nativeBind)(&env,nullptr,static_cast<jlong>(f.id),
        reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2))!=MGMEM_OK)return false;
    f.detached=true;f.attach_fails=!java_failure;f.throw_once=java_failure;
    f.state->closing.store(true);
    std::atomic<bool> reaper_returned{false};
    // Exact production reaper on a joinable test thread; join proves the test
    // itself cannot leave a detached worker accessing its stack fixture.
    std::thread reaper([state=f.state,id=f.id,&reaper_returned]{reap(id,state);reaper_returned=true;});
    bool first_failure=true,retained=true,while_publishing=true;
    if(!java_failure) {
        first_failure=bounded_wait([&]{
            std::lock_guard<std::mutex> lock(f.state->mutex);
            return f.state->done && f.attach_calls>=1 && !f.state->publishing;
        },2000);
        {std::lock_guard<std::mutex> lock(f.state->mutex);
            retained=f.state->listener && f.state->final_buffer && !f.state->completion_published &&
                exists(f.id) && f.global_deletes==0 && f.calls==0;}
    }
    std::thread concurrent_poll;
    if(racing)f.block_callback=true;
    f.attach_fails=false; // Fake VM has recovered; ordinary case uses no Java poll.
    if(racing) {
        concurrent_poll=std::thread([&]{JNI_NAME(nativePollCompletion)(&env,nullptr,static_cast<jlong>(f.id));});
        const bool entered=bounded_wait([&]{return f.entered.load();},2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while_publishing=entered && f.calls==1 && f.global_deletes==0 && exists(f.id);
        f.resume=true;f.changed.notify_all();concurrent_poll.join();
    }
    const bool automatic=bounded_wait([&]{return reaper_returned.load();},2500);
    // A failed test must not leave a joinable test thread behind; explicitly
    // recover only after recording whether autonomous completion succeeded.
    if(!automatic)JNI_NAME(nativePollCompletion)(&env,nullptr,static_cast<jlong>(f.id));
    if(!bounded_wait([&]{return reaper_returned.load();},2000)) {
        std::cout<<"{\"case\":\""<<test_case<<"\",\"pass\":false,\"state\":\"REAPER_STILL_ACTIVE\"}\n"<<std::flush;
        std::_Exit(3); // Isolated test process: never free a live stack fixture.
    }
    reaper.join();
    const unsigned expected_calls=java_failure?2:1;
    bool publication=false;
    {std::lock_guard<std::mutex> lock(f.state->mutex);
        publication=f.state->completion_published && !f.state->publishing &&
            f.state->close_status==(error?MGMEM_PROVIDER_ERROR:MGMEM_OK);}
    const bool retained_error=error && exists(f.id) && f.global_deletes==0 && f.calls==1;
    const bool normal=(!error && !exists(f.id) && f.global_deletes==2 && f.calls==expected_calls);
    // Verify a reported error is published exactly once, while its uncertain
    // state remains quarantined. Fake refs are sentinel values, not JVM refs.
    if(error) {
        JNI_NAME(nativePollCompletion)(&env,nullptr,static_cast<jlong>(f.id));
        publication=publication && f.calls==1 && f.global_deletes==0 && exists(f.id);
        std::lock_guard<std::mutex> lock(registry_mutex);bridges.erase(f.id);
    }
    mgmem_snapshot_v1 snapshot{};
    const bool external_open=mgmem_domain_snapshot_v1(domain,&snapshot,sizeof(snapshot))==MGMEM_OK && snapshot.state==MGMEM_OPEN;
    bool zero=mgmem_domain_begin_close_v1(domain)==MGMEM_OK && mgmem_domain_collect_v1(domain)==MGMEM_OK &&
        mgmem_domain_drain_v1(domain,0)==MGMEM_OK && mgmem_domain_snapshot_v1(domain,&snapshot,sizeof(snapshot))==MGMEM_OK &&
        snapshot.state==MGMEM_CLOSED && snapshot.backing_bytes==0 && snapshot.reserved_bytes==0 &&
        snapshot.active_allocations==0 && snapshot.active_leases==0;
    zero=mgmem_domain_destroy_v1(domain)==MGMEM_OK && zero;
    return first_failure && retained && while_publishing && automatic && publication && (error?retained_error:normal) &&
        f.lifetime_retains==1 && f.lifetime_releases==1 && f.jni_unlocked && external_open && zero;
}

// A close that races a held diagnostic Probe must wait for the release instead
// of completing early, then finish once the last Probe is released.
bool probe_close_case(Fixture &f) {
    active=&f;pending_exception=false;
    const auto id=JNI_NAME(nativeCreate)(&env,nullptr,4096,2048,0,256,
                        reinterpret_cast<jobject>(1),reinterpret_cast<jlongArray>(2));
    if(id<=0)return false;
    f.id=static_cast<u64>(id);f.state=bridge(f.id);
    if(!f.state || !f.state->owned)return false;
    const auto probe=JNI_NAME(nativeHoldProbe)(&env,nullptr,static_cast<jlong>(f.id),64);
    if(!probe)return false;
    const bool readable=JNI_NAME(nativeReadProbe)(&env,nullptr,probe)==JNI_TRUE;
    std::atomic<int> first{static_cast<int>(MGMEM_BUSY)};
    std::thread caller([&]{first.store(mgmem_android_pressure_unregister_v1(f.id));});
    // Longer than the 25 ms notification backstop, so a held Probe must keep the
    // closer waiting rather than letting the close complete early.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const bool still_open=exists(f.id);
    JNI_NAME(nativeReleaseProbe)(&env,nullptr,probe);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(exists(f.id) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    caller.join();
    const bool closed=first.load()==MGMEM_OK && !exists(f.id);
    active=nullptr;
    return readable && still_open && closed && f.calls==1 && f.global_deletes==2;
}
}
int main(int argc,char **argv) {
    using namespace test;
    if(argc!=2)return 2;
    initialize();Fixture f;active=&f;const std::string test_case=argv[1];
    bool pass=false;
    if(test_case=="close-admission-create-fail" || test_case=="close-admission-detach-fail" ||
       test_case=="close-admission-success") {
        pass=close_admission_case(f,test_case=="close-admission-create-fail",test_case=="close-admission-detach-fail");
    } else if(test_case=="binding-exception-matrix") {
        pass=binding_exception_matrix();active=&f;
    } else if(test_case=="snapshot-error-external" || test_case=="snapshot-error-owned" ||
              test_case=="snapshot-state-external" || test_case=="snapshot-state-owned" ||
              test_case=="snapshot-header-external" || test_case=="snapshot-header-owned") {
        const bool owned=test_case.find("-owned")!=std::string::npos;
        const int mode=test_case.find("snapshot-error-")==0?1:test_case.find("snapshot-state-")==0?2:3;
        pass=snapshot_error_case(f,owned,mode);
    } else if(test_case=="auto-attach-recovery" || test_case=="auto-java-recovery" ||
       test_case=="auto-concurrent-poll" || test_case=="auto-error-quarantine") {
        pass=auto_retry_case(f,test_case);
    } else if(test_case=="foreign-release" || test_case=="foreign-release-bind" || test_case=="foreign-release-snapshot") {
        f.foreign_bind=test_case=="foreign-release-bind";f.foreign_snapshot=test_case=="foreign-release-snapshot";
        mgmem_domain *domain=nullptr;
        mgmem_domain_options_v1 options{sizeof(options),1,1,0,MGMEM_SYSTEM,0,4096,0,nullptr};
        if(mgmem_domain_create_v1(&options,&domain)!=MGMEM_OK)return 3;
        mgmem_domain_lifetime_v1 life{sizeof(life),1,&f,&lifetime_retain,&lifetime_release};
        mgmem_pressure_options_v1 policy{sizeof(policy),1,4096,2048,0,1,0};
        if(mgmem_android_pressure_register_v1(domain,&life,&policy,&f.id)!=MGMEM_OK)return 3;
        f.state=bridge(f.id);
        if(mgmem_android_pressure_unregister_v1(f.id)!=MGMEM_OK)return 3;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(f.foreign_bind?5:3);
        while(exists(f.id) && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(exists(f.id)) {
            // A callback is still executing. Never destroy its foreign domain or
            // stack context; fail-stop this isolated diagnostic process.
            std::cout<<"{\"case\":\""<<test_case<<"\",\"pass\":false,\"state\":\"INCOMPLETE_CALLBACK_STILL_ACTIVE\"}\n"<<std::flush;
            std::_Exit(3);
        }
        pass=!exists(f.id) && (f.foreign_bind || f.foreign_snapshot?f.foreign_actual_reentry:f.foreign_unlocked) &&
            f.lifetime_retains==1 && f.lifetime_releases==1;
        mgmem_domain_begin_close_v1(domain);mgmem_domain_collect_v1(domain);mgmem_domain_drain_v1(domain,0);
        if(mgmem_domain_destroy_v1(domain)!=MGMEM_OK)pass=false;
    } else if(test_case=="probe-close-wait") {
        pass=probe_close_case(f);
    } else {
        make_done(f,test_case=="error-quarantine"?MGMEM_PROVIDER_ERROR:MGMEM_OK);
        if(test_case=="callback-reentry") {
            publish_closed(f.id,f.state);
            pass=f.calls==1 && f.jni_unlocked && f.snapshot_reentered && f.global_deletes==2 && !exists(f.id);
        } else if(test_case=="single-publisher") {
            f.block_callback=true;
            std::thread first([&]{publish_closed(f.id,f.state);});
            {std::unique_lock<std::mutex> lock(f.gate);f.changed.wait_for(lock,std::chrono::seconds(2),[&]{return f.entered.load();});}
            std::thread second([&]{publish_closed(f.id,f.state,&env);});second.join();
            const bool snapshot_during_callback=JNI_NAME(nativeSnapshot)(&env,nullptr,static_cast<jlong>(f.id))!=nullptr;
            f.resume=true;f.changed.notify_all();first.join();publish_closed(f.id,f.state,&env);
            pass=f.entered && snapshot_during_callback && f.calls==1 && f.jni_unlocked && f.global_deletes==2 && !exists(f.id);
        } else if(test_case=="attach-retry") {
            f.detached=true;f.attach_fails=true;publish_closed(f.id,f.state);
            const bool retained=exists(f.id) && f.global_deletes==0 && f.calls==0 && f.state->listener;
            f.attach_fails=false;publish_closed(f.id,f.state);
            pass=retained && f.attach_calls==2 && f.detach_calls==1 && f.calls==1 && f.global_deletes==2 &&
                f.jni_unlocked && !exists(f.id);
        } else if(test_case=="java-exception-retry") {
            f.throw_once=true;publish_closed(f.id,f.state);
            const bool retained=exists(f.id) && f.global_deletes==0 && f.calls==1 && !pending_exception;
            publish_closed(f.id,f.state);
            pass=retained && f.calls==2 && f.global_deletes==2 && f.jni_unlocked && !exists(f.id);
        } else if(test_case=="error-quarantine") {
            publish_closed(f.id,f.state);publish_closed(f.id,f.state,&env);
            pass=f.calls==1 && f.global_deletes==0 && exists(f.id) && f.jni_unlocked;
        } else return 2;
    }
    std::cout<<"{\"case\":\""<<test_case<<"\",\"pass\":"<<(pass?"true":"false")
             <<",\"callbacks\":"<<f.calls<<",\"global_ref_deletes\":"<<f.global_deletes
             <<",\"JNI_operations_outside_Bridge\":"<<(f.jni_unlocked?"true":"false")
             <<",\"snapshot_reentered\":"<<(f.snapshot_reentered?"true":"false")
             <<",\"foreign_actual_JNI_reentry\":"<<(f.foreign_actual_reentry?"true":"false")
             <<",\"foreign_release_outside_Bridge\":"<<(f.foreign_unlocked?"true":"false")<<"}\n";
    return pass?0:1;
}
