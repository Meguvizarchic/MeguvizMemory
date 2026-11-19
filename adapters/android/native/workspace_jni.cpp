#include <mgmem/android_borrow_jni.h>
#include <mgmem/pressure.h>
#include <jni.h>

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#if defined(MGMEM_MANAGED_TESTING)
#include <chrono>
#endif

namespace {
using u64 = uint64_t;
constexpr u64 max_id = static_cast<u64>(std::numeric_limits<jlong>::max());
constexpr size_t max_domains = 1024;
constexpr size_t max_owners = 65536;
constexpr unsigned max_pins = 65536;
constexpr unsigned max_workers = 32;
constexpr size_t max_close_guards = 65536;
constexpr size_t managed_pressure_results = 128;
// Java managed-adapter admission statuses; these do not change the C ABI.
constexpr mgmem_status managed_domain_limit = 256;
constexpr mgmem_status managed_owner_limit = 257;
constexpr mgmem_status managed_pin_limit = 258;
constexpr mgmem_status managed_worker_limit = 259;
constexpr mgmem_status managed_guard_limit = 260;
enum State : jint { OPEN = 0, CLOSING = 1, CLOSED = 2, QUARANTINED = 3 };
enum PressurePhase { PRESSURE_NONE, PRESSURE_ATTACHING, PRESSURE_OPEN, PRESSURE_CLOSING, PRESSURE_CLOSED };

struct Domain {
    std::mutex mutex;
    std::condition_variable changed;
    mgmem_domain *native = nullptr;
    State state = OPEN;
    mgmem_status failure = MGMEM_OK;
    u64 inflight = 0, owners = 0, pins = 0;
    bool ready = false, destroying = false;
    bool worker_started = false, final_valid = false;
    std::atomic<bool> claimed{false};
    std::atomic<bool> emergency{false};
    mgmem_snapshot_v1 final_snapshot{};
    std::thread worker;
    mgmem_pressure_controller *pressure = nullptr;
    PressurePhase pressure_phase = PRESSURE_NONE;
    u64 pressure_generation = 0, pressure_calls = 0, direct_budget_calls = 0;
    bool pressure_worker_started = false, pressure_cleanup_owned = false, pressure_final_valid = false;
    mgmem_pressure_snapshot_v2 pressure_final{};
    std::array<mgmem_pressure_event_result_v2, managed_pressure_results> pressure_results{};
    u64 pressure_result_first = 0;
    size_t pressure_result_count = 0;
    std::thread pressure_worker;
};

struct Owner {
    std::mutex mutex;
    std::shared_ptr<Domain> domain;
    mgmem_handle_v1 handle{};
    u64 bytes = 0;
    bool ready = false, live = false;
};

struct CloseGuard {
    std::mutex mutex;
    std::shared_ptr<Domain> domain;
    bool ready = false, live = false;
};

struct Registry {
    std::mutex mutex;
    std::map<u64, std::shared_ptr<Domain>> domains;
    std::map<u64, std::shared_ptr<Owner>> owners;
    std::map<u64, std::shared_ptr<CloseGuard>> guards;
    u64 next_domain = 1, next_owner = 1, next_guard = 1;
    std::atomic<unsigned> workers{0}, pins{0};
};

// Registry and quarantined records deliberately survive until process exit.
// The JNI DSO must not be unloaded while any record or worker exists.
Registry &registry() { static Registry *value = new Registry; return *value; }

#if defined(MGMEM_MANAGED_TESTING)
struct TestGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false, entered = false, released = false, timed_out = false;
};
TestGate allocate_gate, borrow_gate, pin_gate, state_gate, begin_gate, drop_gate, failure_gate, publish_gate;
TestGate guard_exit_gate, guard_post_exit_gate, guard_admission_failure_gate;
TestGate pressure_attach_gate, pressure_budget_gate, pressure_submit_tail_gate;
std::atomic<bool> fail_worker{false}, throw_after_create{false}, throw_after_allocate{false},
    throw_after_borrow{false}, fail_drop{false}, throw_after_drop{false},
    fail_after_destroy{false}, fail_registration_unknown{false}, fail_quarantine{false},
    checked_after_create{false}, checked_after_claim{false}, checked_after_allocate{false},
    checked_after_borrow{false}, checked_after_release{false}, checked_quarantine{false},
    fail_guard_admission{false}, throw_after_guard{false}, fail_guard_exit{false}, force_guard_underflow{false},
    fail_guard_before_quarantine{false}, fail_quarantine_always{false},
    checked_after_guard_exit{false}, checked_after_pressure_submit{false};

TestGate *test_gate(jint kind) noexcept {
    switch (kind) {
    case 1: return &allocate_gate;
    case 2: return &borrow_gate;
    case 3: return &pin_gate;
    case 4: return &state_gate;
    case 5: return &begin_gate;
    case 6: return &drop_gate;
    case 7: return &failure_gate;
    case 8: return &publish_gate;
    case 9: return &guard_exit_gate;
    case 10: return &guard_post_exit_gate;
    case 11: return &pressure_attach_gate;
    case 12: return &pressure_budget_gate;
    case 13: return &pressure_submit_tail_gate;
    case 14: return &guard_admission_failure_gate;
    default: return nullptr;
    }
}

void wait_test_gate(TestGate &gate) {
    std::unique_lock<std::mutex> lock(gate.mutex);
    if (!gate.armed) return;
    gate.entered = true;
    gate.changed.notify_all();
    if (!gate.changed.wait_for(lock, std::chrono::seconds(10), [&] { return gate.released; }))
        gate.timed_out = true;
    gate.armed = false;
}

void test_pending_exception(JNIEnv *env, bool checked = false) {
    jclass type = env->FindClass(checked ? "java/io/IOException" : "java/lang/IllegalStateException");
    if (type && !env->ExceptionCheck()) env->ThrowNew(type, "managed test injected exception");
    if (type) env->DeleteLocalRef(type);
}
#endif

bool reserve(std::atomic<unsigned> &counter, unsigned maximum) noexcept {
    auto value = counter.load(std::memory_order_relaxed);
    while (value < maximum) {
        if (counter.compare_exchange_weak(value, value + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
    }
    return false;
}

std::shared_ptr<Domain> domain_for(jlong id) {
    if (id <= 0) return {};
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.domains.find(static_cast<u64>(id));
    return found == r.domains.end() ? nullptr : found->second;
}

std::shared_ptr<Owner> owner_for(jlong id) {
    if (id <= 0) return {};
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.owners.find(static_cast<u64>(id));
    return found == r.owners.end() ? nullptr : found->second;
}

std::shared_ptr<CloseGuard> guard_for(jlong id) {
    if (id <= 0) return {};
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.guards.find(static_cast<u64>(id));
    return found == r.guards.end() ? nullptr : found->second;
}

void erase_domain(u64 id, const std::shared_ptr<Domain> &d) {
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.domains.find(id);
    if (found != r.domains.end() && found->second == d) r.domains.erase(found);
}

void erase_owner(u64 id, const std::shared_ptr<Owner> &o) {
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.owners.find(id);
    if (found != r.owners.end() && found->second == o) r.owners.erase(found);
}

void erase_guard(u64 id, const std::shared_ptr<CloseGuard> &guard) {
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto found = r.guards.find(id);
    if (found != r.guards.end() && found->second == guard) r.guards.erase(found);
}

void quarantine(const std::shared_ptr<Domain> &d, mgmem_status status) noexcept {
    if (!d) return;
    try {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->state == CLOSED) return; // All core calls and destruction already completed.
        d->emergency.store(true, std::memory_order_release);
        d->state = QUARANTINED;
        if (d->failure == MGMEM_OK) d->failure = status == MGMEM_OK ? MGMEM_PROVIDER_ERROR : status;
    } catch (...) { d->emergency.store(true, std::memory_order_release); }
    d->changed.notify_all();
}

struct Operation {
    std::shared_ptr<Domain> domain;
    mgmem_status status = MGMEM_PROVIDER_ERROR;
    bool admitted = false;

    Operation(std::shared_ptr<Domain> value, bool allow_closing) : domain(std::move(value)) {
        if (!domain) { status = MGMEM_STALE_HANDLE; return; }
        std::lock_guard<std::mutex> lock(domain->mutex);
        if (!domain->ready || !domain->claimed.load(std::memory_order_acquire)) { status = MGMEM_BUSY; return; }
        if (domain->emergency.load(std::memory_order_acquire) || domain->state == QUARANTINED) return;
        if (domain->state == CLOSED || domain->destroying || (!allow_closing && domain->state != OPEN)) {
            status = MGMEM_IS_CLOSED; return;
        }
        if (domain->inflight == std::numeric_limits<u64>::max()) { status = MGMEM_OVERFLOW; return; }
        ++domain->inflight; admitted = true; status = MGMEM_OK;
    }

    ~Operation() noexcept {
        if (!admitted) return;
        try {
            std::lock_guard<std::mutex> lock(domain->mutex);
            --domain->inflight;
        } catch (...) { domain->emergency.store(true, std::memory_order_release); }
        domain->changed.notify_all();
    }
};

struct LeasePin {
    std::shared_ptr<Domain> domain;
    // Caller and the one anticipated adapter retain are reserved in advance.
    // The void retain callback performs no allocation and cannot overflow.
    std::atomic<unsigned> references{2};
    std::atomic<bool> retained{false}, released{false};
    explicit LeasePin(std::shared_ptr<Domain> value) : domain(std::move(value)) {}
};

void drop_pin(LeasePin *pin) noexcept {
    if (pin->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    const auto domain = pin->domain;
    try {
        std::lock_guard<std::mutex> lock(domain->mutex);
        --domain->pins;
        registry().pins.fetch_sub(1, std::memory_order_release);
    } catch (...) {
        // Keep the count/slot reserved and prevent destruction on uncertainty.
        domain->emergency.store(true, std::memory_order_release);
    }
    domain->changed.notify_all();
    delete pin;
}

void retain_pin(void *context) noexcept {
    auto *pin = static_cast<LeasePin *>(context);
    if (pin->retained.exchange(true, std::memory_order_acq_rel))
        pin->domain->emergency.store(true, std::memory_order_release);
}

void release_pin(void *context) noexcept {
    auto *pin = static_cast<LeasePin *>(context);
    if (pin->released.exchange(true, std::memory_order_acq_rel)) {
        pin->domain->emergency.store(true, std::memory_order_release);
        return;
    }
#if defined(MGMEM_MANAGED_TESTING)
    try { wait_test_gate(pin_gate); } catch (...) { pin->domain->emergency.store(true); return; }
#endif
    // The borrow adapter invokes this only after its core borrow_release call
    // has returned. It may still be unwinding its own JNI/registry stack.
    drop_pin(pin);
}

// Admission is decided under the domain lock; controller and core calls are
// always made after releasing it. The guard outlives the complete API call.
struct BudgetCall {
    std::shared_ptr<Domain> d;
    mgmem_pressure_controller *controller = nullptr;
    mgmem_status status = MGMEM_PROVIDER_ERROR;
    bool admitted = false, direct = false;
    explicit BudgetCall(std::shared_ptr<Domain> value) : d(std::move(value)) {
        if (!d) { status = MGMEM_STALE_HANDLE; return; }
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->ready || !d->claimed.load()) { status = MGMEM_BUSY; return; }
        if (d->emergency.load() || d->state == QUARANTINED) return;
        if (d->state != OPEN) { status = MGMEM_IS_CLOSED; return; }
        if (d->pressure_phase == PRESSURE_ATTACHING || d->pressure_phase == PRESSURE_CLOSING) {
            status = MGMEM_BUSY; return;
        }
        direct = d->pressure_phase != PRESSURE_OPEN;
        auto &count = direct ? d->direct_budget_calls : d->pressure_calls;
        if (count == max_id || d->inflight == max_id) { status = MGMEM_OVERFLOW; return; }
        ++count; ++d->inflight;
        controller = d->pressure; admitted = true; status = MGMEM_OK;
    }
    ~BudgetCall() {
        if (!admitted) return;
        try {
            std::lock_guard<std::mutex> lock(d->mutex);
            --(direct ? d->direct_budget_calls : d->pressure_calls); --d->inflight;
        } catch (...) { d->emergency.store(true); }
        d->changed.notify_all();
    }
};

struct PressureCall {
    std::shared_ptr<Domain> d;
    mgmem_pressure_controller *controller = nullptr;
    mgmem_status status = MGMEM_PROVIDER_ERROR;
    bool admitted = false;
    PressureCall(std::shared_ptr<Domain> value, u64 generation) : d(std::move(value)) {
        if (!d) { status = MGMEM_STALE_HANDLE; return; }
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->emergency.load() || d->state == QUARANTINED) return;
        if (generation != d->pressure_generation || d->pressure_phase != PRESSURE_OPEN || d->state != OPEN) {
            status = MGMEM_IS_CLOSED; return;
        }
        if (d->pressure_calls == max_id) { status = MGMEM_OVERFLOW; return; }
        ++d->pressure_calls; admitted = true; controller = d->pressure; status = MGMEM_OK;
    }
    ~PressureCall() {
        if (!admitted) return;
        try { std::lock_guard<std::mutex> lock(d->mutex); --d->pressure_calls; }
        catch (...) { d->emergency.store(true); }
        d->changed.notify_all();
    }
};

mgmem_status finish_pressure_cleanup(const std::shared_ptr<Domain> &d, bool own_thread) {
    mgmem_pressure_controller *controller;
    {
        std::unique_lock<std::mutex> lock(d->mutex);
        d->changed.wait(lock, [&] { return d->emergency.load() || d->pressure_calls == 0; });
        if (d->emergency.load()) return MGMEM_PROVIDER_ERROR;
        controller = d->pressure;
    }
    auto status = mgmem_pressure_begin_close_v2(controller);
    if (status == MGMEM_OK) {
        do { status = mgmem_pressure_drain_v2(controller, 1000); } while (status == MGMEM_TIMEOUT);
    }
    mgmem_pressure_snapshot_v2 final{};
    if (status == MGMEM_OK) status = mgmem_pressure_snapshot_v2_get(controller, &final, sizeof(final));
    if (status == MGMEM_OK && (final.struct_size < sizeof(final) || final.version != 2 ||
            final.state != MGMEM_CLOSED || final.applied_pressure > MGMEM_PRESSURE_CRITICAL ||
            final.trim_pending != 0 || final.queue_depth != 0 || final.counters_saturated > 1 ||
            final.effective_budget_bytes > final.manual_budget_bytes)) status = MGMEM_PROVIDER_ERROR;
    std::array<mgmem_pressure_event_result_v2, managed_pressure_results> results{};
    const size_t result_count = final.events_accepted < managed_pressure_results ?
        static_cast<size_t>(final.events_accepted) : managed_pressure_results;
    const u64 result_first = result_count ? final.events_accepted - result_count + 1 : 0;
    for (size_t i = 0; status == MGMEM_OK && i < result_count; ++i) {
        const auto ticket = result_first + i;
        auto &result = results[i];
        status = mgmem_pressure_event_result_v2_get(controller, ticket, &result, sizeof(result));
        if (status == MGMEM_OK && (result.struct_size < sizeof(result) || result.version != 2 ||
                result.ticket != ticket || result.state < MGMEM_PRESSURE_EVENT_APPLIED ||
                result.state > MGMEM_PRESSURE_EVENT_EXPIRED ||
                (result.state == MGMEM_PRESSURE_EVENT_APPLIED && (result.status != MGMEM_OK || !result.apply_sequence)) ||
                (result.state == MGMEM_PRESSURE_EVENT_FAILED && result.status == MGMEM_OK) ||
                (result.state == MGMEM_PRESSURE_EVENT_CANCELLED && result.status != MGMEM_IS_CLOSED)))
            status = MGMEM_PROVIDER_ERROR;
    }
    if (status == MGMEM_OK) status = mgmem_pressure_destroy_v2(controller);
    if (status != MGMEM_OK) return status;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->pressure = nullptr; d->pressure_final = final; d->pressure_final_valid = true;
        d->pressure_results = results; d->pressure_result_first = result_first; d->pressure_result_count = result_count;
        if (own_thread) d->pressure_worker.detach();
        d->pressure_phase = PRESSURE_CLOSED;
        d->pressure_cleanup_owned = false;
    }
    d->changed.notify_all();
    return MGMEM_OK;
}

void pressure_close_worker(std::shared_ptr<Domain> d) noexcept {
    struct WorkerSlot { ~WorkerSlot() { registry().workers.fetch_sub(1, std::memory_order_release); } } slot;
    try {
        auto status = finish_pressure_cleanup(d, true);
        if (status == MGMEM_OK) return;
        { std::lock_guard<std::mutex> lock(d->mutex); if (d->pressure_worker.joinable()) d->pressure_worker.detach(); }
        quarantine(d, status);
    } catch (...) {
        try { std::lock_guard<std::mutex> lock(d->mutex); if (d->pressure_worker.joinable()) d->pressure_worker.detach(); } catch (...) {}
        quarantine(d, MGMEM_PROVIDER_ERROR);
    }
}

void close_worker(std::shared_ptr<Domain> d) noexcept {
    struct WorkerSlot { ~WorkerSlot() { registry().workers.fetch_sub(1, std::memory_order_release); } } slot;
    try {
        bool own_pressure_cleanup = false;
        {
            std::unique_lock<std::mutex> lock(d->mutex);
            d->changed.wait(lock, [&] { return d->emergency.load() || d->pressure_phase != PRESSURE_ATTACHING; });
            if (d->emergency.load()) return;
            if (d->pressure_phase == PRESSURE_OPEN) d->pressure_phase = PRESSURE_CLOSING;
            if (d->pressure_phase == PRESSURE_CLOSING) {
                if (!d->pressure_cleanup_owned) { d->pressure_cleanup_owned = true; own_pressure_cleanup = true; }
                else {
                    d->changed.wait(lock, [&] { return d->emergency.load() || d->pressure_phase == PRESSURE_CLOSED; });
                    if (d->emergency.load()) return;
                }
            }
        }
        auto status = own_pressure_cleanup ? finish_pressure_cleanup(d, false) : MGMEM_OK;
        if (status == MGMEM_OK) status = mgmem_domain_begin_close_v1(d->native);
        if (status != MGMEM_OK) { quarantine(d, status); return; }
        {
            std::unique_lock<std::mutex> lock(d->mutex);
            d->changed.wait(lock, [&] {
                return d->emergency.load(std::memory_order_acquire) ||
                       (d->inflight == 0 && d->owners == 0 && d->pins == 0);
            });
            if (d->emergency.load(std::memory_order_acquire)) return;
            d->destroying = true;
        }
        status = mgmem_domain_collect_v1(d->native);
        if (status == MGMEM_OK) status = mgmem_domain_drain_v1(d->native, 0);
        mgmem_snapshot_v1 final{};
        if (status == MGMEM_OK) status = mgmem_domain_snapshot_v1(d->native, &final, sizeof(final));
        if (status == MGMEM_OK && (final.state != MGMEM_CLOSED || final.backing_bytes || final.reserved_bytes ||
                final.live_bytes || final.retiring_bytes || final.active_allocations || final.active_leases))
            status = MGMEM_PROVIDER_ERROR;
        if (status == MGMEM_OK) {
            status = mgmem_domain_destroy_v1(d->native);
#if defined(MGMEM_MANAGED_TESTING)
            if (status == MGMEM_OK && fail_after_destroy.exchange(false)) status = MGMEM_PROVIDER_ERROR;
#endif
        }
        if (status != MGMEM_OK) {
            // A failed destroy may already have consumed the pointer. Never
            // retry or inspect it; retain the record as unknown/quarantined.
            quarantine(d, status);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            d->native = nullptr;
            d->final_snapshot = final;
            d->final_valid = true;
            if (d->emergency.load(std::memory_order_acquire)) {
                d->state = QUARANTINED;
                if (d->failure == MGMEM_OK) d->failure = MGMEM_PROVIDER_ERROR;
                return;
            }
            // Detach on the worker itself after every core API returned. A
            // detach failure keeps the record/possibly joinable thread forever;
            // it never invokes std::thread destruction on the Java UI thread.
            try { d->worker.detach(); }
            catch (...) {
                d->emergency.store(true, std::memory_order_release);
                d->state = QUARANTINED;
                d->failure = MGMEM_PROVIDER_ERROR;
                return;
            }
            d->state = CLOSED;
        }
        d->changed.notify_all();
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); }
}

jlong failed(mgmem_status status) { return -static_cast<jlong>(status == MGMEM_OK ? MGMEM_PROVIDER_ERROR : status); }
jlong signed_value(u64 value) { return value <= max_id ? static_cast<jlong>(value) : -1; }
}

#define JNI_NAME(method) Java_io_github_jichuo1_meguviz_memory_android_ManagedMemoryDomain_##method

extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeCreate)(JNIEnv *env, jclass, jboolean arena, jlong budget, jlong chunk) {
    if (budget <= 0 || (arena && chunk <= 0)) return failed(MGMEM_INVALID_ARGUMENT);
    std::shared_ptr<Domain> d;
    u64 id = 0;
    try {
        d = std::make_shared<Domain>();
        auto &r = registry();
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            if (r.domains.size() >= max_domains) return failed(managed_domain_limit);
            if (r.next_domain > max_id) return failed(MGMEM_OVERFLOW);
            id = r.next_domain++;
            r.domains.emplace(id, d);
        }
        const mgmem_domain_options_v1 options{sizeof(options), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
            arena ? MGMEM_ARENA : MGMEM_SYSTEM, 0, static_cast<u64>(budget), static_cast<u64>(chunk), nullptr};
        auto status = mgmem_domain_create_v1(&options, &d->native);
        if (status != MGMEM_OK) {
            if (status == MGMEM_PROVIDER_ERROR || d->native) quarantine(d, status);
            else erase_domain(id, d);
            return failed(status);
        }
        { std::lock_guard<std::mutex> lock(d->mutex); d->ready = true; }
#if defined(MGMEM_MANAGED_TESTING)
        if (throw_after_create.exchange(false)) test_pending_exception(env);
        else if (checked_after_create.exchange(false)) test_pending_exception(env, true);
#else
        (void)env;
#endif
        return static_cast<jlong>(id);
    } catch (const std::bad_alloc &) {
        if (d && d->native) quarantine(d, MGMEM_OUT_OF_MEMORY);
        else if (d && id) { try { erase_domain(id, d); } catch (...) {} }
        return failed(MGMEM_OUT_OF_MEMORY);
    } catch (...) {
        if (d && d->native) quarantine(d, MGMEM_PROVIDER_ERROR);
        else if (d && id) { try { erase_domain(id, d); } catch (...) {} }
        return failed(MGMEM_PROVIDER_ERROR);
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeClaimDomain)(JNIEnv *env, jclass, jlong id) {
    std::shared_ptr<Domain> d;
    try {
        d = domain_for(id); if (!d) return MGMEM_STALE_HANDLE;
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->ready || d->emergency.load() || d->state != OPEN) return MGMEM_PROVIDER_ERROR;
        if (d->claimed.exchange(true, std::memory_order_acq_rel)) return MGMEM_BUSY;
#if defined(MGMEM_MANAGED_TESTING)
        if (checked_after_claim.exchange(false)) test_pending_exception(env, true);
#else
        (void)env;
#endif
        return MGMEM_OK;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeAllocate)(JNIEnv *env, jclass, jlong domain_id, jlong bytes, jlong alignment) {
    if (bytes <= 0 || alignment <= 0 || (static_cast<u64>(alignment) & (static_cast<u64>(alignment) - 1)) != 0)
        return failed(MGMEM_INVALID_ARGUMENT);
    std::shared_ptr<Domain> d;
    std::shared_ptr<Owner> owner;
    u64 id = 0;
    bool published = false, core_entered = false;
    try {
        d = domain_for(domain_id);
        Operation operation(d, false);
        if (!operation.admitted) return failed(operation.status);
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(allocate_gate);
#else
        (void)env;
#endif
        owner = std::make_shared<Owner>(); owner->domain = d; owner->bytes = static_cast<u64>(bytes);
        auto &r = registry();
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            if (r.owners.size() >= max_owners) return failed(managed_owner_limit);
            if (r.next_owner > max_id) return failed(MGMEM_OVERFLOW);
            id = r.next_owner++;
            r.owners.emplace(id, owner);
            published = true;
        }
        core_entered = true;
        auto status = mgmem_allocate_v1(d->native, static_cast<u64>(bytes), static_cast<u64>(alignment), &owner->handle);
        if (status != MGMEM_OK) {
            if (status == MGMEM_PROVIDER_ERROR) quarantine(d, status);
            else erase_owner(id, owner);
            return failed(status);
        }
        owner->live = true;
        { std::lock_guard<std::mutex> lock(d->mutex); ++d->owners; }
        mgmem_borrow_v1 init{};
        status = mgmem_borrow_acquire_v1(d->native, owner->handle, 0, static_cast<u64>(bytes), MGMEM_WRITE, &init);
        if (status == MGMEM_PROVIDER_ERROR) { quarantine(d, status); return failed(status); }
        if (status == MGMEM_OK) {
            std::memset(init.data, 0, static_cast<size_t>(bytes));
            status = mgmem_borrow_release_v1(d->native, init.lease);
            if (status != MGMEM_OK) { quarantine(d, status); return failed(status); }
        }
        bool closing = false;
        { std::lock_guard<std::mutex> lock(d->mutex); closing = d->state != OPEN || d->emergency.load(); }
        if (status != MGMEM_OK || closing) {
            const auto released = mgmem_release_v1(d->native, owner->handle);
            if (released != MGMEM_OK) { quarantine(d, released); return failed(released); }
            owner->live = false;
            { std::lock_guard<std::mutex> lock(d->mutex); --d->owners; }
            erase_owner(id, owner);
            return failed(status == MGMEM_OK ? MGMEM_IS_CLOSED : status);
        }
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(publish_gate);
#endif
        { std::lock_guard<std::mutex> lock(owner->mutex); owner->ready = true; }
#if defined(MGMEM_MANAGED_TESTING)
        if (throw_after_allocate.exchange(false)) test_pending_exception(env);
        else if (checked_after_allocate.exchange(false)) test_pending_exception(env, true);
#endif
        return static_cast<jlong>(id);
    } catch (const std::bad_alloc &) {
        if (d && published && core_entered) quarantine(d, MGMEM_OUT_OF_MEMORY);
        else if (owner && published) { try { erase_owner(id, owner); } catch (...) {} }
        return failed(MGMEM_OUT_OF_MEMORY);
    } catch (...) {
        if (d && published && core_entered) quarantine(d, MGMEM_PROVIDER_ERROR);
        else if (owner && published) { try { erase_owner(id, owner); } catch (...) {} }
        return failed(MGMEM_PROVIDER_ERROR);
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeSetBudget)(JNIEnv *, jclass, jlong id, jlong bytes) {
    if (bytes < 0) return MGMEM_INVALID_ARGUMENT;
    std::shared_ptr<Domain> d;
    try {
        d = domain_for(id); BudgetCall op(d); if (!op.admitted) return op.status;
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(pressure_budget_gate);
#endif
        u64 sequence = 0;
        const auto status = op.direct ? mgmem_domain_set_budget_v1(d->native, static_cast<u64>(bytes)) :
            mgmem_pressure_set_manual_budget_v2(op.controller, static_cast<u64>(bytes), &sequence);
        if (status == MGMEM_PROVIDER_ERROR) quarantine(d, status);
        return status;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeAttachPressure)(JNIEnv *, jclass, jlong id, jlong moderate, jlong critical) {
    if (moderate < 0 || critical < 0 || critical > moderate) return failed(MGMEM_INVALID_ARGUMENT);
    std::shared_ptr<Domain> d;
    LeasePin *pin = nullptr;
    u64 generation = 0;
    bool attaching = false, entered = false;
    try {
        d = domain_for(id); Operation op(d, false); if (!op.admitted) return failed(op.status);
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (d->state != OPEN) return failed(MGMEM_IS_CLOSED);
            if ((d->pressure_phase != PRESSURE_NONE && d->pressure_phase != PRESSURE_CLOSED) || d->direct_budget_calls)
                return failed(MGMEM_BUSY);
            if (d->pressure_generation == max_id) return failed(MGMEM_OVERFLOW);
            generation = ++d->pressure_generation;
            d->pressure_phase = PRESSURE_ATTACHING; attaching = true;
        }
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(pressure_attach_gate);
#endif
        pin = new LeasePin(d);
        if (!reserve(registry().pins, max_pins)) {
            delete pin; pin = nullptr;
            { std::lock_guard<std::mutex> lock(d->mutex); d->pressure_phase = PRESSURE_NONE; }
            d->changed.notify_all(); return failed(managed_pin_limit);
        }
        { std::lock_guard<std::mutex> lock(d->mutex); ++d->pins; }
        const mgmem_domain_lifetime_v1 lifetime{sizeof(lifetime), 1, pin, retain_pin, release_pin};
        const mgmem_pressure_options_v2 options{sizeof(options), 2, static_cast<u64>(moderate),
            static_cast<u64>(critical), 10, 32, 128, 0};
        mgmem_pressure_controller *controller = nullptr;
        entered = true;
        const auto status = mgmem_pressure_create_v2(d->native, &lifetime, &options, &controller);
        const bool retained = pin->retained.load(), released = pin->released.load();
        if (status == MGMEM_PROVIDER_ERROR || (released && !retained) ||
            (status == MGMEM_OK && (!controller || !retained || released)) ||
            (status != MGMEM_OK && (controller || (retained && !released)))) {
            quarantine(d, MGMEM_PROVIDER_ERROR); pin = nullptr; return failed(MGMEM_PROVIDER_ERROR);
        }
        if (!retained) drop_pin(pin);
        drop_pin(pin); pin = nullptr;
        bool closing = false;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (status == MGMEM_OK) {
                d->pressure = controller;
                d->pressure_final_valid = false;
                closing = d->state != OPEN || d->emergency.load();
                d->pressure_phase = closing ? PRESSURE_CLOSING : PRESSURE_OPEN;
            } else d->pressure_phase = PRESSURE_NONE;
        }
        d->changed.notify_all();
        return status != MGMEM_OK ? failed(status) : closing ? failed(MGMEM_IS_CLOSED) : static_cast<jlong>(generation);
    } catch (...) {
        if (entered || pin) quarantine(d, MGMEM_PROVIDER_ERROR);
        else if (attaching && d) {
            try { std::lock_guard<std::mutex> lock(d->mutex); d->pressure_phase = PRESSURE_NONE; }
            catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); }
            d->changed.notify_all();
        }
        return failed(MGMEM_PROVIDER_ERROR);
    }
}

extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativePressureSubmit)(JNIEnv *env, jclass, jlong id, jlong generation, jint level) {
    std::shared_ptr<Domain> d;
    try {
        u64 ticket = 0;
        {
            d = domain_for(id); PressureCall call(d, static_cast<u64>(generation));
            if (!call.admitted) return failed(call.status);
            const auto status = mgmem_pressure_submit_v2(call.controller, static_cast<uint32_t>(level), &ticket);
            if (status == MGMEM_PROVIDER_ERROR) quarantine(d, status);
            if (status != MGMEM_OK) return failed(status);
            if (!ticket || ticket > max_id) { quarantine(d, MGMEM_PROVIDER_ERROR); return failed(MGMEM_PROVIDER_ERROR); }
        }
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(pressure_submit_tail_gate);
        if (checked_after_pressure_submit.exchange(false)) test_pending_exception(env, true);
#else
        (void)env;
#endif
        return static_cast<jlong>(ticket);
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return failed(MGMEM_PROVIDER_ERROR); }
}

extern "C" JNIEXPORT jlongArray JNICALL JNI_NAME(nativePressureResult)(JNIEnv *env, jclass, jlong id, jlong generation, jlong ticket) {
    if (ticket <= 0) return nullptr;
    std::shared_ptr<Domain> d;
    try {
        mgmem_pressure_event_result_v2 result{};
        d = domain_for(id); if (!d) return nullptr;
        bool final = false;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (d->pressure_generation != static_cast<u64>(generation)) return nullptr;
            if (d->pressure_phase == PRESSURE_CLOSED && d->pressure_final_valid) {
                const auto value = static_cast<u64>(ticket);
                if (value > d->pressure_final.events_accepted) return nullptr;
                if (value < d->pressure_result_first) {
                    result = {sizeof(result), 2, MGMEM_PRESSURE_EVENT_EXPIRED, MGMEM_OK, value, 0};
                } else {
                    const u64 index = value - d->pressure_result_first;
                    if (index >= d->pressure_result_count) return nullptr;
                    result = d->pressure_results[static_cast<size_t>(index)];
                }
                final = true;
            }
        }
        if (!final) {
            PressureCall call(d, static_cast<u64>(generation)); if (!call.admitted) return nullptr;
            const auto status = mgmem_pressure_event_result_v2_get(call.controller, static_cast<u64>(ticket), &result, sizeof(result));
            if (status != MGMEM_OK) { if (status == MGMEM_PROVIDER_ERROR) quarantine(d, status); return nullptr; }
        }
        jlong values[]{result.state, result.status, signed_value(result.ticket), signed_value(result.apply_sequence)};
        jlongArray array = env->NewLongArray(4); if (array) env->SetLongArrayRegion(array, 0, 4, values); return array;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return nullptr; }
}

extern "C" JNIEXPORT jlongArray JNICALL JNI_NAME(nativePressureSnapshot)(JNIEnv *env, jclass, jlong id, jlong generation) {
    std::shared_ptr<Domain> d;
    try {
        d = domain_for(id); if (!d) return nullptr;
        mgmem_pressure_snapshot_v2 snap{};
        bool final = false;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (d->pressure_generation != static_cast<u64>(generation)) return nullptr;
            if (d->pressure_phase == PRESSURE_CLOSED && d->pressure_final_valid) { snap = d->pressure_final; final = true; }
        }
        if (!final) {
            PressureCall call(d, static_cast<u64>(generation)); if (!call.admitted) return nullptr;
            auto status = mgmem_pressure_snapshot_v2_get(call.controller, &snap, sizeof(snap));
            if (status != MGMEM_OK) { if (status == MGMEM_PROVIDER_ERROR) quarantine(d, status); return nullptr; }
        }
        jlong values[]{snap.state,snap.applied_pressure,snap.trim_pending,snap.last_trim_status,snap.queue_depth,
            snap.counters_saturated,signed_value(snap.manual_budget_bytes),signed_value(snap.effective_budget_bytes),
            signed_value(snap.apply_sequence),signed_value(snap.events_accepted),signed_value(snap.events_applied),
            signed_value(snap.events_failed),signed_value(snap.events_cancelled),signed_value(snap.events_busy),
            signed_value(snap.trim_attempts),signed_value(snap.trim_successes),signed_value(snap.trim_busy),signed_value(snap.trim_failures),
            signed_value(snap.events_critical_busy),signed_value(snap.results_evicted)};
        jlongArray array = env->NewLongArray(20); if (array) env->SetLongArrayRegion(array, 0, 20, values); return array;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return nullptr; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativePressureState)(JNIEnv *, jclass, jlong id, jlong generation) {
    try {
        auto d = domain_for(id); if (!d) return CLOSED;
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->emergency.load() || d->state == QUARANTINED) return QUARANTINED;
        if (d->pressure_generation != static_cast<u64>(generation) || d->pressure_phase == PRESSURE_CLOSED) return CLOSED;
        return d->pressure_phase == PRESSURE_OPEN && d->state == OPEN ? OPEN : CLOSING;
    } catch (...) { return QUARANTINED; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativePressureBeginClose)(JNIEnv *, jclass, jlong id, jlong generation) {
    std::shared_ptr<Domain> d;
    try {
        d = domain_for(id); if (!d) return MGMEM_OK;
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->emergency.load() || d->state == QUARANTINED) return MGMEM_PROVIDER_ERROR;
        if (d->pressure_generation != static_cast<u64>(generation) || d->pressure_phase == PRESSURE_CLOSED) return MGMEM_OK;
        if (d->pressure_phase == PRESSURE_OPEN) d->pressure_phase = PRESSURE_CLOSING;
        if (d->pressure_phase != PRESSURE_CLOSING) return MGMEM_BUSY;
        if (d->pressure_cleanup_owned) return MGMEM_OK;
        if (d->state == CLOSING && d->worker_started) return MGMEM_OK;
        if (!reserve(registry().workers, max_workers)) return managed_worker_limit;
#if defined(MGMEM_MANAGED_TESTING)
        if (fail_worker.exchange(false)) {
            registry().workers.fetch_sub(1, std::memory_order_release);
            return MGMEM_OUT_OF_MEMORY;
        }
#endif
        d->pressure_cleanup_owned = true;
        try { d->pressure_worker = std::thread(pressure_close_worker, d); }
        catch (...) { d->pressure_cleanup_owned = false; registry().workers.fetch_sub(1); return MGMEM_OUT_OF_MEMORY; }
        d->pressure_worker_started = true;
        return MGMEM_OK;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeBorrow)(JNIEnv *env, jclass, jlong id, jlong offset, jlong bytes, jboolean writable) {
    if (offset < 0 || bytes <= 0) return failed(MGMEM_INVALID_ARGUMENT);
    std::shared_ptr<Owner> owner;
    std::shared_ptr<Domain> d;
    LeasePin *pin = nullptr;
    try {
        owner = owner_for(id); if (!owner) return failed(MGMEM_STALE_HANDLE);
        d = owner->domain; Operation op(d, false); if (!op.admitted) return failed(op.status);
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(borrow_gate);
#else
        (void)env;
#endif
        if (static_cast<u64>(offset) > owner->bytes || static_cast<u64>(bytes) > owner->bytes - static_cast<u64>(offset))
            return failed(MGMEM_INVALID_ARGUMENT);
        pin = new LeasePin(d);
        auto &r = registry();
        if (!reserve(r.pins, max_pins)) { delete pin; return failed(managed_pin_limit); }
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            ++d->pins;
        }
        const mgmem_domain_lifetime_v1 lifetime{sizeof(lifetime), 1, pin, retain_pin, release_pin};
        u64 registration = 0;
        mgmem_status status;
        {
            std::lock_guard<std::mutex> lock(owner->mutex);
            status = !owner->ready || !owner->live ? MGMEM_STALE_HANDLE :
#if defined(MGMEM_MANAGED_TESTING)
                fail_registration_unknown.exchange(false) ? MGMEM_PROVIDER_ERROR :
#endif
                mgmem_android_borrow_register_v1(d->native, &lifetime, owner->handle,
                    static_cast<u64>(offset), static_cast<u64>(bytes), writable ? MGMEM_WRITE : MGMEM_READ,
                    MGMEM_ANDROID_BORROW_INITIALIZED, &registration);
        }
        const bool retained = pin->retained.load(std::memory_order_acquire);
        const bool released = pin->released.load(std::memory_order_acquire);
        if (status == MGMEM_PROVIDER_ERROR || (released && !retained) ||
                (status == MGMEM_OK && (!registration || !retained || released)) ||
                (status != MGMEM_OK && retained && !released)) {
            // The adapter may have retained an unpublished entry or lease.
            // Keep both pre-reserved references and its domain pin; no retry.
            quarantine(d, MGMEM_PROVIDER_ERROR);
            pin = nullptr;
            return failed(MGMEM_PROVIDER_ERROR);
        }
        if (!retained) drop_pin(pin); // No adapter callback will own its reserved reference.
        drop_pin(pin); // The complete registration call has returned.
        pin = nullptr;
#if defined(MGMEM_MANAGED_TESTING)
        if (status == MGMEM_OK && registration && throw_after_borrow.exchange(false)) test_pending_exception(env);
        else if (status == MGMEM_OK && registration && checked_after_borrow.exchange(false)) test_pending_exception(env, true);
#endif
        return status == MGMEM_OK && registration ? static_cast<jlong>(registration) : failed(status);
    } catch (const std::bad_alloc &) {
        if (pin) quarantine(d, MGMEM_OUT_OF_MEMORY);
        return failed(MGMEM_OUT_OF_MEMORY);
    } catch (...) {
        if (pin) quarantine(d, MGMEM_PROVIDER_ERROR);
        return failed(MGMEM_PROVIDER_ERROR);
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeCancel)(JNIEnv *, jclass, jlong id) {
    try { return id > 0 ? mgmem_android_borrow_cancel_v1(static_cast<u64>(id)) : MGMEM_INVALID_ARGUMENT; }
    catch (...) { return MGMEM_PROVIDER_ERROR; }
}

// A separate in-flight guard spans the entire borrowing-adapter close/cancel
// call, including its post-release registry work and Java error observation.
extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeGuardEnter)(JNIEnv *env, jclass, jlong id) {
    std::shared_ptr<Domain> d;
    std::shared_ptr<CloseGuard> guard;
    u64 guard_id = 0;
    bool published = false, counted = false;
    try {
        d = domain_for(id); if (!d) return failed(MGMEM_STALE_HANDLE);
#if defined(MGMEM_MANAGED_TESTING)
        if (fail_guard_admission.exchange(false)) {
            wait_test_gate(guard_admission_failure_gate);
            return failed(managed_guard_limit);
        }
#else
        (void)env;
#endif
        guard = std::make_shared<CloseGuard>(); guard->domain = d;
        auto &r = registry();
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            if (r.guards.size() >= max_close_guards) return failed(managed_guard_limit);
            if (r.next_guard > max_id) return failed(MGMEM_OVERFLOW);
            guard_id = r.next_guard++;
            r.guards.emplace(guard_id, guard); published = true;
        }
        mgmem_status status = MGMEM_OK;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (!d->ready || !d->claimed.load()) status = MGMEM_BUSY;
            else if (d->emergency.load() || d->state == QUARANTINED) status = MGMEM_PROVIDER_ERROR;
            else if (d->state == CLOSED || d->destroying) status = MGMEM_IS_CLOSED;
            else if (d->inflight == std::numeric_limits<u64>::max()) status = MGMEM_OVERFLOW;
            else { ++d->inflight; counted = true; }
        }
        if (status != MGMEM_OK) { erase_guard(guard_id, guard); return failed(status); }
        { std::lock_guard<std::mutex> lock(guard->mutex); guard->ready = true; guard->live = true; }
#if defined(MGMEM_MANAGED_TESTING)
        if (throw_after_guard.exchange(false)) test_pending_exception(env, true);
#endif
        return static_cast<jlong>(guard_id);
    } catch (const std::bad_alloc &) {
        bool uncertain = counted;
        if (counted) quarantine(d, MGMEM_OUT_OF_MEMORY);
        else if (published) {
            try { erase_guard(guard_id, guard); }
            catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); uncertain = true; }
        }
        return failed(uncertain ? MGMEM_PROVIDER_ERROR : MGMEM_OUT_OF_MEMORY);
    } catch (...) {
        if (counted || published) quarantine(d, MGMEM_PROVIDER_ERROR);
        return failed(MGMEM_PROVIDER_ERROR);
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeGuardExit)(JNIEnv *env, jclass, jlong id, jboolean failed_close) {
    std::shared_ptr<CloseGuard> guard;
    std::shared_ptr<Domain> d;
    try {
        guard = guard_for(id); if (!guard) return MGMEM_STALE_HANDLE;
        d = guard->domain;
        {
            std::lock_guard<std::mutex> lock(guard->mutex);
            if (!guard->ready || !guard->live) return MGMEM_STALE_HANDLE;
            guard->live = false;
        }
#if defined(MGMEM_MANAGED_TESTING)
        if (fail_guard_before_quarantine.exchange(false)) return MGMEM_PROVIDER_ERROR;
#endif
        if (failed_close) quarantine(d, MGMEM_PROVIDER_ERROR);
        // Registry removal can fail or block. It must finish while the domain
        // in-flight count still prevents cleanup and destruction.
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(guard_exit_gate);
        if (fail_guard_exit.exchange(false)) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
#endif
        erase_guard(static_cast<u64>(id), guard);
        bool underflow = false;
        {
            std::lock_guard<std::mutex> domain_lock(d->mutex);
#if defined(MGMEM_MANAGED_TESTING)
            if (force_guard_underflow.exchange(false)) d->inflight = 0;
#endif
            if (d->inflight == 0) {
                d->emergency.store(true); d->state = QUARANTINED;
                if (d->failure == MGMEM_OK) d->failure = MGMEM_PROVIDER_ERROR;
                underflow = true;
            } else --d->inflight;
        }
        d->changed.notify_all();
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(guard_post_exit_gate);
        if (checked_after_guard_exit.exchange(false)) test_pending_exception(env, true);
#else
        (void)env;
#endif
        return underflow ? MGMEM_PROVIDER_ERROR : MGMEM_OK;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeRelease)(JNIEnv *env, jclass, jlong id) {
    std::shared_ptr<Owner> owner;
    std::shared_ptr<Domain> d;
    try {
        owner = owner_for(id); if (!owner) return MGMEM_STALE_HANDLE;
        d = owner->domain; Operation op(d, true); if (!op.admitted) return op.status;
        {
            std::lock_guard<std::mutex> lock(owner->mutex);
            if (!owner->ready || !owner->live) return MGMEM_STALE_HANDLE;
            const auto status = mgmem_release_v1(d->native, owner->handle);
            if (status != MGMEM_OK) { quarantine(d, status); return status; }
            owner->live = false;
        }
        { std::lock_guard<std::mutex> lock(d->mutex); --d->owners; }
        erase_owner(static_cast<u64>(id), owner);
#if defined(MGMEM_MANAGED_TESTING)
        if (checked_after_release.exchange(false)) test_pending_exception(env, true);
#else
        (void)env;
#endif
        return MGMEM_OK;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeBeginClose)(JNIEnv *, jclass, jlong id) {
    auto d = std::shared_ptr<Domain>();
    try {
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(begin_gate);
#endif
        d = domain_for(id); if (!d) return MGMEM_STALE_HANDLE;
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->ready || !d->claimed.load(std::memory_order_acquire)) return MGMEM_BUSY;
        if (d->emergency.load(std::memory_order_acquire) || d->state == QUARANTINED) return MGMEM_PROVIDER_ERROR;
        if (d->state == CLOSED) return MGMEM_OK;
        d->state = CLOSING;
        if (d->worker_started) return MGMEM_OK;
        auto &r = registry();
        if (!reserve(r.workers, max_workers)) return managed_worker_limit;
#if defined(MGMEM_MANAGED_TESTING)
        if (fail_worker.exchange(false)) {
            r.workers.fetch_sub(1, std::memory_order_release);
            return MGMEM_OUT_OF_MEMORY;
        }
#endif
        try { d->worker = std::thread(close_worker, d); }
        catch (...) { r.workers.fetch_sub(1, std::memory_order_release); return MGMEM_OUT_OF_MEMORY; }
        d->worker_started = true;
        return MGMEM_OK;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeState)(JNIEnv *, jclass, jlong id) {
    try {
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(state_gate);
#endif
        auto d = domain_for(id); if (!d) return QUARANTINED;
        std::lock_guard<std::mutex> lock(d->mutex);
        return !d->ready || !d->claimed.load(std::memory_order_acquire) ||
            d->emergency.load(std::memory_order_acquire) ? QUARANTINED : d->state;
    } catch (...) { return QUARANTINED; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeFailure)(JNIEnv *, jclass, jlong id) {
    try {
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(failure_gate);
#endif
        auto d = domain_for(id); if (!d) return MGMEM_STALE_HANDLE;
        std::lock_guard<std::mutex> lock(d->mutex);
        return d->failure == MGMEM_OK && d->emergency.load(std::memory_order_acquire) ? MGMEM_PROVIDER_ERROR : d->failure;
    } catch (...) { return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativePrepareForget)(JNIEnv *, jclass, jlong id) {
    std::shared_ptr<Domain> d;
    try {
        d = domain_for(id); if (!d) return MGMEM_STALE_HANDLE;
        std::lock_guard<std::mutex> lock(d->mutex);
        return d->state == CLOSED && !d->emergency.load() && !d->worker.joinable() ? MGMEM_OK : MGMEM_BUSY;
    } catch (...) { return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeDropClosed)(JNIEnv *env, jclass, jlong id) {
    try {
#if defined(MGMEM_MANAGED_TESTING)
        if (fail_drop.exchange(false)) return MGMEM_BUSY;
#else
        (void)env;
#endif
        auto d = domain_for(id); if (!d) return MGMEM_STALE_HANDLE;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (d->state != CLOSED || d->emergency.load() || d->worker.joinable()) return MGMEM_BUSY;
        }
        erase_domain(static_cast<u64>(id), d);
#if defined(MGMEM_MANAGED_TESTING)
        wait_test_gate(drop_gate);
        if (throw_after_drop.exchange(false)) test_pending_exception(env);
#endif
        return MGMEM_OK;
    } catch (...) { return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jlongArray JNICALL JNI_NAME(nativeSnapshot)(JNIEnv *env, jclass, jlong id) {
    std::shared_ptr<Domain> d;
    try {
        d = domain_for(id); if (!d) return nullptr;
        mgmem_snapshot_v1 snap{};
        State state;
        {
            std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->ready || !d->claimed.load(std::memory_order_acquire) || d->emergency.load() || d->state == QUARANTINED) return nullptr;
            state = d->state;
            if (state == CLOSED) {
                if (!d->final_valid) return nullptr;
                snap = d->final_snapshot;
            } else {
                if (d->destroying || d->inflight == std::numeric_limits<u64>::max()) return nullptr;
                ++d->inflight;
            }
        }
        if (state != CLOSED) {
            const auto status = mgmem_domain_snapshot_v1(d->native, &snap, sizeof(snap));
            { std::lock_guard<std::mutex> lock(d->mutex); --d->inflight; }
            d->changed.notify_all();
            if (status != MGMEM_OK) { if (status == MGMEM_PROVIDER_ERROR) quarantine(d, status); return nullptr; }
        }
        const jlong values[] = {static_cast<jlong>(state), static_cast<jlong>(snap.state),
            signed_value(snap.budget_bytes), signed_value(snap.backing_bytes),
            signed_value(snap.reserved_bytes), signed_value(snap.peak_backing_bytes), signed_value(snap.live_bytes),
            signed_value(snap.retiring_bytes), signed_value(snap.active_allocations), signed_value(snap.active_leases)};
        auto result = env->NewLongArray(10);
        if (!result || env->ExceptionCheck()) return nullptr;
        env->SetLongArrayRegion(result, 0, 10, values);
        return env->ExceptionCheck() ? nullptr : result;
    } catch (...) { quarantine(d, MGMEM_PROVIDER_ERROR); return nullptr; }
}

extern "C" JNIEXPORT void JNICALL JNI_NAME(nativeQuarantine)(JNIEnv *env, jclass, jlong id) {
#if defined(MGMEM_MANAGED_TESTING)
    if (fail_quarantine_always.load()) { test_pending_exception(env); return; }
    if (fail_quarantine.exchange(false)) { test_pending_exception(env); return; }
    if (checked_quarantine.exchange(false)) { test_pending_exception(env, true); return; }
#else
    (void)env;
#endif
    try { quarantine(domain_for(id), MGMEM_PROVIDER_ERROR); } catch (...) {}
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeWorkerCount)(JNIEnv *, jclass) {
    try { return static_cast<jint>(registry().workers.load(std::memory_order_acquire)); }
    catch (...) { return -1; }
}

extern "C" JNIEXPORT jlongArray JNICALL JNI_NAME(nativeRuntimeCounts)(JNIEnv *env, jclass) {
    try {
        auto &r = registry();
        jlong values[7]{};
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            values[0] = static_cast<jlong>(r.domains.size());
            values[1] = static_cast<jlong>(r.owners.size());
            values[6] = static_cast<jlong>(r.guards.size());
            for (const auto &item : r.domains) {
                if (!item.second->claimed.load(std::memory_order_acquire)) ++values[4];
                if (item.second->emergency.load(std::memory_order_acquire)) ++values[5];
            }
        }
        values[2] = static_cast<jlong>(r.pins.load(std::memory_order_acquire));
        values[3] = static_cast<jlong>(r.workers.load(std::memory_order_acquire));
        auto result = env->NewLongArray(7);
        if (!result || env->ExceptionCheck()) return nullptr;
        env->SetLongArrayRegion(result, 0, 7, values);
        return env->ExceptionCheck() ? nullptr : result;
    } catch (...) { return nullptr; }
}

#if defined(MGMEM_MANAGED_TESTING)
#define TEST_NAME(method) Java_io_github_jichuo1_meguviz_memory_android_ManagedMemoryDomainJvmTest_##method
extern "C" void mgmem_managed_test_pressure_gate_arm();
extern "C" int mgmem_managed_test_pressure_gate_wait_entered();
extern "C" int mgmem_managed_test_pressure_gate_wait_close();
extern "C" int mgmem_managed_test_pressure_gate_release();
extern "C" JNIEXPORT jint JNICALL Java_io_github_jichuo1_meguviz_memory_android_ManagedPressureBindingJvmTest_nativePressureGate(JNIEnv *, jclass, jint action) {
    switch (action) {
    case 0: mgmem_managed_test_pressure_gate_arm(); return 1;
    case 1: return mgmem_managed_test_pressure_gate_wait_entered();
    case 2: return mgmem_managed_test_pressure_gate_wait_close();
    case 3: return mgmem_managed_test_pressure_gate_release();
    default: return 0;
    }
}
extern "C" JNIEXPORT void JNICALL TEST_NAME(nativeArmGate)(JNIEnv *, jclass, jint kind) {
    try {
        auto *gate = test_gate(kind); if (!gate) return;
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->armed = true; gate->entered = false; gate->released = false; gate->timed_out = false;
    } catch (...) {}
}
extern "C" JNIEXPORT jboolean JNICALL TEST_NAME(nativeWaitGate)(JNIEnv *, jclass, jint kind) {
    try {
        auto *gate = test_gate(kind); if (!gate) return JNI_FALSE;
        std::unique_lock<std::mutex> lock(gate->mutex);
        return gate->changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }) ? JNI_TRUE : JNI_FALSE;
    } catch (...) { return JNI_FALSE; }
}
extern "C" JNIEXPORT jboolean JNICALL TEST_NAME(nativeReleaseGate)(JNIEnv *, jclass, jint kind) {
    try {
        auto *gate = test_gate(kind); if (!gate) return JNI_FALSE;
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true; gate->changed.notify_all();
        return gate->entered && !gate->timed_out ? JNI_TRUE : JNI_FALSE;
    } catch (...) { return JNI_FALSE; }
}
extern "C" JNIEXPORT void JNICALL TEST_NAME(nativeInject)(JNIEnv *, jclass, jint kind) {
    switch (kind) {
    case 1: fail_worker.store(true); break;
    case 2: throw_after_create.store(true); break;
    case 3: throw_after_allocate.store(true); break;
    case 4: throw_after_borrow.store(true); break;
    case 5: fail_drop.store(true); break;
    case 6: throw_after_drop.store(true); break;
    case 7: fail_after_destroy.store(true); break;
    case 8: fail_registration_unknown.store(true); break;
    case 9: fail_quarantine.store(true); break;
    case 10: checked_after_create.store(true); break;
    case 11: checked_after_claim.store(true); break;
    case 12: checked_after_allocate.store(true); break;
    case 13: checked_after_borrow.store(true); break;
    case 14: checked_after_release.store(true); break;
    case 15: checked_quarantine.store(true); break;
    case 16: fail_guard_admission.store(true); break;
    case 17: throw_after_guard.store(true); break;
    case 18: fail_guard_exit.store(true); break;
    case 19: force_guard_underflow.store(true); break;
    case 20: checked_after_guard_exit.store(true); break;
    case 21: fail_guard_before_quarantine.store(true); fail_quarantine_always.store(true); break;
    case 22: checked_after_pressure_submit.store(true); break;
    default: break;
    }
}
extern "C" JNIEXPORT jint JNICALL TEST_NAME(nativeRawState)(JNIEnv *, jclass, jlong id) {
    try {
        auto d = domain_for(id); if (!d) return -1;
        std::lock_guard<std::mutex> lock(d->mutex);
        return d->state;
    } catch (...) { return -1; }
}
#endif
