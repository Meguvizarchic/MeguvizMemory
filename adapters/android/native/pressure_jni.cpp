#include <mgmem/android_pressure_jni.h>
#include <jni.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#if defined(MGMEM_JNI_CLOSE_TEST_HOOKS)
namespace mgmem_jni_close_test {
void before_create();
void before_detach(std::thread *worker);
}
#endif

namespace {
using u64 = uint64_t;
constexpr u64 max_id = static_cast<u64>(std::numeric_limits<jlong>::max());
constexpr unsigned close_open=0,close_starting=1,close_admitted=2;
struct Holder {
    mgmem_domain *domain = nullptr;
    std::atomic<u64> active_probes{0};
    // active_probes is updated and waited on under this mutex so a Probe
    // release racing with the closer cannot be missed. The closer keeps a 25 ms
    // timeout as a backstop.
    std::mutex probe_mutex;
    std::condition_variable probe_changed;
    ~Holder() {
        // Owned-domain reaper retains Holder while any diagnostic lease exists.
        // No final cleanup is delegated to the last Java lease-close thread.
        if (!domain) return;
        if (mgmem_domain_begin_close_v1(domain) != MGMEM_OK || mgmem_domain_collect_v1(domain) != MGMEM_OK ||
            mgmem_domain_drain_v1(domain,0) != MGMEM_OK || mgmem_domain_destroy_v1(domain) != MGMEM_OK) std::terminate();
    }
};
struct OwnerPin {
    std::atomic<unsigned> references{1};
    std::shared_ptr<Holder> holder;
    static void retain(void *context) noexcept {static_cast<OwnerPin *>(context)->references.fetch_add(1);}
    static void release(void *context) noexcept {
        auto *pin = static_cast<OwnerPin *>(context);
        if (pin->references.fetch_sub(1,std::memory_order_acq_rel)==1) delete pin;
    }
};
struct Bridge {
    std::mutex mutex;
    std::condition_variable changed;
    mgmem_pressure_controller *control = nullptr;
    std::shared_ptr<Holder> owned;
    // STARTING also rejects new events but is not yet a successful admission.
    std::atomic<unsigned> closing{close_open};
    std::atomic<bool> ready{false};
    bool done = false;
    bool publishing = false, completion_published = false;
    bool final_snapshot_available = false;
    mgmem_status close_status = MGMEM_BUSY;
    mgmem_pressure_snapshot_v1 final_snapshot{};
    JavaVM *vm=nullptr;
    jobject listener=nullptr;
    jlongArray final_buffer=nullptr;
    jmethodID completion_method=nullptr;
};
struct Probe {
    std::mutex mutex;
    std::shared_ptr<Holder> holder;
    mgmem_borrow_v1 view{};
    explicit Probe(std::shared_ptr<Holder> value):holder(std::move(value)) {
        std::lock_guard<std::mutex> lock(holder->probe_mutex);
        holder->active_probes.fetch_add(1,std::memory_order_relaxed);
    }
    ~Probe() {
        if (view.lease.id && mgmem_borrow_release_v1(holder->domain,view.lease)!=MGMEM_OK) std::terminate();
        // A quiet core is not sufficient to destroy a raw domain: the complete
        // last C API call must have returned before the owner can disappear.
        {
            std::lock_guard<std::mutex> lock(holder->probe_mutex);
            holder->active_probes.fetch_sub(1,std::memory_order_release);
        }
        holder->probe_changed.notify_all();
    }
};
std::mutex registry_mutex;
std::map<u64,std::shared_ptr<Bridge>> bridges;
std::map<u64,std::shared_ptr<Probe>> probes;
u64 next_bridge=1,next_probe=1;
std::shared_ptr<Bridge> bridge(u64 id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found=bridges.find(id);return found==bridges.end() || !found->second->ready.load()?nullptr:found->second;
}
std::shared_ptr<Probe> probe(u64 id) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found=probes.find(id);return found==probes.end()?nullptr:found->second;
}
void fail(JNIEnv *env,const char *message) {
    if (env->ExceptionCheck()) return;
    jclass type=env->FindClass("java/lang/IllegalStateException");
    if (type && !env->ExceptionCheck()) env->ThrowNew(type,message);
}
void snapshot_values(const std::shared_ptr<Bridge> &state,const mgmem_pressure_snapshot_v1 &observed,jlong *values) {
    const auto &d=observed.domain;
    const auto number=[](u64 n)->jlong{return n<=max_id?static_cast<jlong>(n):-1;};
    if (state->done && !state->final_snapshot_available) {
        std::fill(values,values+13,static_cast<jlong>(-1));
        values[13]=number(state->close_status);
        values[14]=state->owned?1:0;
        values[15]=1;
        return;
    }
    const jlong result[]={number(observed.state),number(d.state),number(d.budget_bytes),
        number(d.backing_bytes),number(d.reserved_bytes),number(d.live_bytes),
        number(d.retiring_bytes),number(d.retained_bytes),number(observed.over_budget_bytes),
        number(observed.events_accepted),number(observed.trim_attempts),number(observed.trim_successes),
        number(observed.deferred_busy),number(observed.last_status),state->owned?1:0,state->done?1:0};
    std::copy(std::begin(result),std::end(result),values);
}
jint attach_current_thread(JavaVM *vm,JNIEnv **env) noexcept {
#if defined(__ANDROID__)
    return vm->AttachCurrentThread(env,nullptr);
#else
    return vm->AttachCurrentThread(reinterpret_cast<void **>(env),nullptr);
#endif
}
void publish_closed(u64 id,const std::shared_ptr<Bridge> &state,JNIEnv *supplied=nullptr) noexcept {
    JavaVM *vm=nullptr;jobject listener=nullptr;jlongArray buffer=nullptr;jmethodID method=nullptr;
    mgmem_status status=MGMEM_BUSY;jlong values[16]{};
    {
        std::unique_lock<std::mutex> lock(state->mutex,std::defer_lock);
        if(supplied) {if(!lock.try_lock())return;} else lock.lock();
        if(!state->done || state->publishing || state->completion_published)return;
        state->publishing=true;
        vm=state->vm;listener=state->listener;buffer=state->final_buffer;method=state->completion_method;
        status=state->close_status;snapshot_values(state,state->final_snapshot,values);
    }
    // A single publisher owns these global pins until commit. No JNI operation
    // or Java callback executes while holding the Bridge mutex.
    bool attached=false,published=listener==nullptr,deleted=false;JNIEnv *env=supplied;
    if(vm && listener) {
        if(!env) {
            JNIEnv *candidate=nullptr;
            const auto found=vm->GetEnv(reinterpret_cast<void **>(&candidate),JNI_VERSION_1_6);
            if(found==JNI_OK)env=candidate;
            else if(found==JNI_EDETACHED) {
                candidate=nullptr;
                if(attach_current_thread(vm,&candidate)==JNI_OK) {env=candidate;attached=true;}
            }
        }
        if(env) {
            env->SetLongArrayRegion(buffer,0,16,values);
            if(!env->ExceptionCheck())env->CallVoidMethod(listener,method,static_cast<jint>(status));
            published=!env->ExceptionCheck();
            if(!published)env->ExceptionClear();
            if(published && status==MGMEM_OK) {
                env->DeleteGlobalRef(listener);env->DeleteGlobalRef(buffer);deleted=true;
            }
        }
        if(attached)vm->DetachCurrentThread();
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if(deleted) {state->listener=nullptr;state->final_buffer=nullptr;}
        state->completion_published=published;state->publishing=false;
        state->changed.notify_all();
    }
    // Failed attachment/Java delivery keeps all pins and releases the claim for
    // retry. A reported cleanup error is quarantined, never republished/freed.
    if(published && status==MGMEM_OK) {std::lock_guard<std::mutex> lock(registry_mutex);bridges.erase(id);}
}
void reap(u64 id,const std::shared_ptr<Bridge> &state) noexcept {
    mgmem_pressure_controller *control=nullptr;
    {std::lock_guard<std::mutex> lock(state->mutex);control=state->control;}
    auto result=mgmem_pressure_begin_close_v1(control);
    if (result==MGMEM_OK) {
        do {result=mgmem_pressure_drain_v1(control,100);} while (result==MGMEM_TIMEOUT);
        mgmem_pressure_snapshot_v1 last{};
        const auto observed=mgmem_pressure_snapshot_v1_get(control,&last,sizeof(last));
        if (observed!=MGMEM_OK) result=observed;
        else if (last.state!=MGMEM_CLOSED || last.domain.struct_size<sizeof(mgmem_snapshot_v1) ||
                 last.domain.abi_major!=MGMEM_ABI_MAJOR || last.domain.abi_minor>MGMEM_ABI_MINOR ||
                 last.domain.state>MGMEM_CLOSED) result=MGMEM_PROVIDER_ERROR;
        else {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->final_snapshot=last;state->final_snapshot_available=true;state->control=nullptr;
            }
            // Detach under the lock after all prior Bridge API calls returned;
            // destroy joins then invokes the foreign owner's release outside it.
            const auto destroyed=mgmem_pressure_destroy_v1(control);
            if(destroyed!=MGMEM_OK && destroyed!=MGMEM_PROVIDER_ERROR) {
                std::lock_guard<std::mutex> lock(state->mutex);state->control=control;
            }
            if (result==MGMEM_OK) result=destroyed;
        }
    }
    if (result==MGMEM_OK && state->owned) {
        auto *domain=state->owned->domain;
        result=mgmem_domain_begin_close_v1(domain);
        // Wait for the last diagnostic lease to release. The condition variable
        // removes the fixed 25 ms timer; the timeout remains as a lost-notify
        // backstop so a missed wakeup can never hang the closer.
        while(result==MGMEM_OK) {
            std::unique_lock<std::mutex> lock(state->owned->probe_mutex);
            if (state->owned->active_probes.load(std::memory_order_acquire)==0) break;
            state->owned->probe_changed.wait_for(lock,std::chrono::milliseconds(25),
                [&]{return state->owned->active_probes.load(std::memory_order_acquire)==0;});
        }
        while (result==MGMEM_OK) {
            const auto status=mgmem_domain_collect_v1(domain);
            if (status==MGMEM_BUSY) {std::this_thread::sleep_for(std::chrono::milliseconds(25));continue;}
            result=status;break;
        }
        if (result==MGMEM_OK) result=mgmem_domain_drain_v1(domain,0);
        if (result==MGMEM_OK) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                result=mgmem_domain_snapshot_v1(domain,&state->final_snapshot.domain,sizeof(state->final_snapshot.domain));
                if(result==MGMEM_OK)state->owned->domain=nullptr;
                else state->final_snapshot_available=false;
            }
            if (result==MGMEM_OK) {
                result=mgmem_domain_destroy_v1(domain);
                std::lock_guard<std::mutex> lock(state->mutex);
                if(result==MGMEM_OK || result==MGMEM_PROVIDER_ERROR)state->final_snapshot.over_budget_bytes=0;
                else state->owned->domain=domain; // Non-consuming failure remains quarantined.
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->close_status=result;state->final_snapshot.last_status=result;
        state->done=true;state->changed.notify_all();
    }
    // The Java owner is allowed to close and never poll again. A transient VM
    // attach or callback failure must not strand its global references and
    // registration forever. Reuse this reaper, preserving the single-publisher
    // claim in publish_closed; never free uncertain resources on a timeout.
    auto delay=std::chrono::milliseconds(25);
    for (;;) {
        publish_closed(id,state);
        std::unique_lock<std::mutex> lock(state->mutex);
        if (state->completion_published) break;
        state->changed.wait_for(lock,delay,[&] {return state->completion_published;});
        if (state->completion_published) break;
        delay=std::min(delay*2,std::chrono::milliseconds(1000));
    }
}
mgmem_status schedule_close(u64 id) noexcept {
    auto state=bridge(id);if(!state)return MGMEM_IS_CLOSED;
    unsigned expected=close_open;
    if(!state->closing.compare_exchange_strong(expected,close_starting))
        return expected==close_admitted?MGMEM_OK:MGMEM_BUSY;
    std::thread *worker=nullptr;
    try {
#if defined(MGMEM_JNI_CLOSE_TEST_HOOKS)
        mgmem_jni_close_test::before_create();
#endif
        // Allocate the thread object before its constructor starts the worker.
        worker=new std::thread([state,id] {reap(id,state);});
    } catch (...) {
        // Construction failed: no thread exists and this admission is safe to retry.
        state->closing.store(close_open);
        return MGMEM_OUT_OF_MEMORY;
    }
    try {
#if defined(MGMEM_JNI_CLOSE_TEST_HOOKS)
        mgmem_jni_close_test::before_detach(worker);
#endif
        worker->detach();
        delete worker;
    } catch (...) {
        // The worker has already started. Never retry or destroy a possibly
        // joinable std::thread: retain its object and OS handle indefinitely.
        // The worker still owns state and completes the usual cleanup.
    }
    state->closing.store(close_admitted);
    return MGMEM_OK;
}
}

extern "C" MGMEM_PRESSURE_API mgmem_status mgmem_android_pressure_register_v1(mgmem_domain *domain,
    const mgmem_domain_lifetime_v1 *lifetime,const mgmem_pressure_options_v1 *options,u64 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out=0;
    try {
        auto state=std::make_shared<Bridge>();
        u64 id=0;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            if (next_bridge==max_id) throw std::bad_alloc();
            id=next_bridge++;
            bridges.emplace(id,state); // All allocating publication metadata first.
        }
        const auto status=mgmem_pressure_create_v1(domain,lifetime,options,&state->control);
        if(status!=MGMEM_OK){std::lock_guard<std::mutex> lock(registry_mutex);bridges.erase(id);return status;}
        state->ready.store(true);*out=id;return MGMEM_OK;
    } catch (const std::bad_alloc &) {return MGMEM_OUT_OF_MEMORY;}
    catch (...) {return MGMEM_PROVIDER_ERROR;}
}
extern "C" MGMEM_PRESSURE_API mgmem_status mgmem_android_pressure_unregister_v1(u64 registration) {
    return schedule_close(registration);
}

#define JNI_NAME(method) Java_io_github_jichuo1_meguviz_memory_android_NativePressureBackend_##method
namespace {
struct JavaBinding {
    jobject listener=nullptr;
    jlongArray buffer=nullptr;
    jmethodID method=nullptr;
    JavaVM *vm=nullptr;
};
bool prepare_binding(JNIEnv *env,jobject listener,jlongArray buffer,JavaBinding &binding) {
    binding.listener=env->NewGlobalRef(listener);
    if (env->ExceptionCheck() || !binding.listener) return false;
    binding.buffer=static_cast<jlongArray>(env->NewGlobalRef(buffer));
    if (env->ExceptionCheck() || !binding.buffer) return false;
    jclass type=env->GetObjectClass(listener);
    if (env->ExceptionCheck() || !type) return false;
    binding.method=env->GetMethodID(type,"onNativeClosed","(I)V");
    if (env->ExceptionCheck() || !binding.method) return false;
    const auto status=env->GetJavaVM(&binding.vm);
    return !env->ExceptionCheck() && status==JNI_OK && binding.vm;
}
void release_binding(JNIEnv *env,JavaBinding &binding) {
    // DeleteGlobalRef is permitted even with a pending Java exception.
    if(binding.listener)env->DeleteGlobalRef(binding.listener);
    if(binding.buffer)env->DeleteGlobalRef(binding.buffer);
    binding.listener=nullptr;binding.buffer=nullptr;
}
}
extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeCreate)(JNIEnv *env,jclass,jlong normal,jlong moderate,jlong critical,jlong chunk,jobject listener,jlongArray buffer) {
    JavaBinding binding;
    try {
        if (normal<=0 || moderate<0 || critical<0 || critical>moderate || moderate>normal || chunk<=0) {fail(env,"invalid budgets");return 0;}
        if(!listener || !buffer){fail(env,"invalid binding");return 0;}
        const auto length=env->GetArrayLength(buffer);
        if(env->ExceptionCheck())return 0;
        if(length!=16){fail(env,"invalid binding");return 0;}
        // Java pins and callback metadata are admitted before any native domain
        // or worker. Binding OOM cannot leave an unreachable native owner.
        if(!prepare_binding(env,listener,buffer,binding))throw std::bad_alloc();
        auto owned=std::make_shared<Holder>();
        mgmem_domain_options_v1 options{sizeof(options),MGMEM_ABI_MAJOR,MGMEM_ABI_MINOR,0,MGMEM_ARENA,0,
            static_cast<u64>(normal),static_cast<u64>(chunk),nullptr};
        if (mgmem_domain_create_v1(&options,&owned->domain)!=MGMEM_OK) throw std::bad_alloc();
        auto *pin=new OwnerPin;pin->holder=owned;
        const mgmem_domain_lifetime_v1 lifetime{sizeof(lifetime),1,pin,&OwnerPin::retain,&OwnerPin::release};
        const mgmem_pressure_options_v1 policy{sizeof(policy),1,static_cast<u64>(normal),static_cast<u64>(moderate),static_cast<u64>(critical),25,0};
        u64 id=0;const auto status=mgmem_android_pressure_register_v1(owned->domain,&lifetime,&policy,&id);
        OwnerPin::release(pin);
        if (status!=MGMEM_OK) throw std::bad_alloc();
        auto state=bridge(id);if(!state)throw std::bad_alloc();
        std::lock_guard<std::mutex> lock(state->mutex);
        if(state->closing.load())throw std::bad_alloc();
        state->owned=std::move(owned);state->vm=binding.vm;
        state->listener=binding.listener;state->final_buffer=binding.buffer;state->completion_method=binding.method;
        binding.listener=nullptr;binding.buffer=nullptr;
        return static_cast<jlong>(id);
    } catch (...) {
        release_binding(env,binding);
        if(!env->ExceptionCheck())fail(env,"native creation failed");return 0;
    }
}
extern "C" JNIEXPORT jboolean JNICALL JNI_NAME(nativeExists)(JNIEnv *,jclass,jlong token) {return token>0 && bridge(token)?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeBind)(JNIEnv *env,jclass,jlong token,jobject listener,jlongArray buffer) {
    auto state=token>0?bridge(token):nullptr;
    if(!state || !listener || !buffer)return MGMEM_INVALID_ARGUMENT;
    const auto length=env->GetArrayLength(buffer);
    if(env->ExceptionCheck() || length!=16)return MGMEM_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if(state->closing.load() || state->listener || state->done)return MGMEM_BUSY;
    }
    JavaBinding binding;mgmem_status result=MGMEM_OUT_OF_MEMORY;
    if(prepare_binding(env,listener,buffer,binding)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if(state->closing.load() || state->listener || state->done)result=MGMEM_BUSY;
        else {
            state->vm=binding.vm;state->listener=binding.listener;state->final_buffer=binding.buffer;state->completion_method=binding.method;
            binding.listener=nullptr;binding.buffer=nullptr;
            result=MGMEM_OK;
        }
    }
    if(result!=MGMEM_OK) {
        release_binding(env,binding);
    }
    return result;
}
extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeNotify)(JNIEnv *,jclass,jlong token,jint pressure) {
    auto state=token>0?bridge(token):nullptr;
    if (!state || state->closing.load()) return MGMEM_IS_CLOSED;
    std::unique_lock<std::mutex> lock(state->mutex,std::try_to_lock);
    if (!lock.owns_lock()) return MGMEM_BUSY;
    if (state->closing.load() || !state->control) return MGMEM_IS_CLOSED;
    return mgmem_pressure_notify_v1(state->control,static_cast<uint32_t>(pressure));
}
extern "C" JNIEXPORT jlongArray JNICALL JNI_NAME(nativeSnapshot)(JNIEnv *env,jclass,jlong token) {
    auto state=token>0?bridge(token):nullptr;
    if (!state) return nullptr;
    jlong values[16];
    {
        std::unique_lock<std::mutex> lock(state->mutex,std::try_to_lock);
        if (!lock.owns_lock()) return nullptr;
        mgmem_pressure_snapshot_v1 observed=state->final_snapshot;
        if (!state->done && state->control && mgmem_pressure_snapshot_v1_get(state->control,&observed,sizeof(observed))!=MGMEM_OK) return nullptr;
        if (state->owned && state->owned->domain && !state->done) {
            if(mgmem_domain_snapshot_v1(state->owned->domain,&observed.domain,sizeof(observed.domain))!=MGMEM_OK)return nullptr;
            const auto &d=observed.domain;
            if(d.backing_bytes>std::numeric_limits<u64>::max()-d.reserved_bytes)return nullptr;
            const u64 charged=d.backing_bytes+d.reserved_bytes;
            observed.over_budget_bytes=charged>d.budget_bytes?charged-d.budget_bytes:0;
        }
        snapshot_values(state,observed,values);
    }
    auto result=env->NewLongArray(16);if(result)env->SetLongArrayRegion(result,0,16,values);return result;
}
extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeBeginClose)(JNIEnv *,jclass,jlong token) {return token>0?schedule_close(token):MGMEM_IS_CLOSED;}
extern "C" JNIEXPORT void JNICALL JNI_NAME(nativePollCompletion)(JNIEnv *env,jclass,jlong token) {
    auto state=token>0?bridge(token):nullptr;if(state)publish_closed(token,state,env);
}
extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeHoldProbe)(JNIEnv *env,jclass,jlong token,jlong bytes) {
    try {
        auto state=token>0?bridge(token):nullptr;
        if (!state || bytes<=0 || bytes>64*1024*1024) {fail(env,"invalid probe");return 0;}
        const char *failure=nullptr;
        const auto admit=[&]() -> jlong {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->owned || state->closing.load()) {failure="owned domain required";return 0;}
        auto item=std::make_shared<Probe>(state->owned);
        mgmem_handle_v1 owner{};
        auto status=mgmem_allocate_v1(item->holder->domain,bytes,64,&owner);
        if (status!=MGMEM_OK) {failure="probe allocation refused";return 0;}
        mgmem_borrow_v1 writer{};
        status=mgmem_borrow_acquire_v1(item->holder->domain,owner,0,bytes,MGMEM_WRITE,&writer);
        if(status==MGMEM_OK) {std::memset(writer.data,0x5a,static_cast<size_t>(bytes));status=mgmem_borrow_release_v1(item->holder->domain,writer.lease);}
        if(status==MGMEM_OK)status=mgmem_borrow_acquire_v1(item->holder->domain,owner,0,bytes,MGMEM_READ,&item->view);
        const auto release=mgmem_release_v1(item->holder->domain,owner);
        if(status!=MGMEM_OK || release!=MGMEM_OK) {failure="probe admission failed";return 0;}
        std::lock_guard<std::mutex> registry_lock(registry_mutex);
        if(next_probe==max_id) {failure="probe IDs exhausted";return 0;}
        const u64 id=next_probe++;probes.emplace(id,item);return static_cast<jlong>(id);
        };
        const auto id=admit();if(failure)fail(env,failure);return id;
    } catch (...) {fail(env,"probe creation failed");return 0;}
}
extern "C" JNIEXPORT jboolean JNICALL JNI_NAME(nativeReadProbe)(JNIEnv *,jclass,jlong token) {
    auto item=token>0?probe(token):nullptr;if(!item)return JNI_FALSE;
    std::lock_guard<std::mutex> lock(item->mutex);
    return item->view.data && static_cast<const unsigned char *>(item->view.data)[0]==0x5a &&
        static_cast<const unsigned char *>(item->view.data)[item->view.length-1]==0x5a?JNI_TRUE:JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL JNI_NAME(nativeReleaseProbe)(JNIEnv *,jclass,jlong token) {
    std::shared_ptr<Probe> held;
    {std::lock_guard<std::mutex> lock(registry_mutex);auto found=probes.find(token);if(found!=probes.end()){held=std::move(found->second);probes.erase(found);}}
    // Destruction occurs outside registry; an in-flight read has its own shared pin.
}
