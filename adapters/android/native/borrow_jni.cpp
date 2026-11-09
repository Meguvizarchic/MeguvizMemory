#include <mgmem/android_borrow_jni.h>
#include <jni.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>

namespace {
using u64 = uint64_t;
constexpr u64 max_token = static_cast<u64>(std::numeric_limits<jlong>::max());

struct Entry {
    std::mutex mutex;
    mgmem_domain *domain = nullptr;
    mgmem_domain_lifetime_v1 lifetime{};
    mgmem_borrow_v1 view{};
    uint32_t access = MGMEM_READ;
    bool pinned = false;
    bool ready = false;
    bool claimed = false;
    bool closed = false;
};

std::mutex registry_mutex;
std::map<u64, std::shared_ptr<Entry>> registry;
u64 next_token = 1;
std::atomic<size_t> staged_bytes{0};
#if defined(MGMEM_BORROW_TESTING)
std::atomic<bool> fail_next_staging_allocation{false};
#endif

class StagingReservation {
public:
    explicit StagingReservation(size_t bytes) : bytes_(bytes) {
        auto current = staged_bytes.load(std::memory_order_relaxed);
        const size_t limit = MGMEM_ANDROID_BORROW_RUNTIME_STAGING_BYTES;
        while (current <= limit && bytes <= limit - current) {
            if (staged_bytes.compare_exchange_weak(current, current + bytes,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                admitted_ = true;
                break;
            }
        }
    }
    ~StagingReservation() {
        if (admitted_) staged_bytes.fetch_sub(bytes_, std::memory_order_release);
    }
    StagingReservation(const StagingReservation &) = delete;
    StagingReservation &operator=(const StagingReservation &) = delete;
    bool admitted() const { return admitted_; }
private:
    size_t bytes_;
    bool admitted_ = false;
};

std::shared_ptr<Entry> lookup(u64 token) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = registry.find(token);
    return found == registry.end() ? nullptr : found->second;
}

void erase(u64 token, const std::shared_ptr<Entry> &entry) {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto found = registry.find(token);
    if (found != registry.end() && found->second == entry) registry.erase(found);
}

/* No adapter mutex is held while invoking the core or foreign callbacks. An
 * unsuccessful callback may already have consumed its input; quarantine the
 * entry and pin and never retry it. The registry intentionally retains it. */
mgmem_status close_registration(u64 token, bool java_claimed) {
    auto entry = lookup(token);
    if (!entry) return MGMEM_STALE_HANDLE;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->ready || entry->closed) return MGMEM_STALE_HANDLE;
        if (entry->claimed != java_claimed) return MGMEM_BUSY;
        entry->closed = true;
    }
    const auto status = mgmem_borrow_release_v1(entry->domain, entry->view.lease);
    if (status != MGMEM_OK) return status;
    try {
        entry->lifetime.release(entry->lifetime.context);
        entry->pinned = false;
    } catch (...) {
        return MGMEM_PROVIDER_ERROR;
    }
    erase(token, entry);
    return MGMEM_OK;
}
}

#if defined(MGMEM_BORROW_TESTING)
/* Built only into the host fault-injection executable, never the JNI library. */
extern "C" void mgmem_borrow_test_fail_staging_allocation() {
    fail_next_staging_allocation.store(true, std::memory_order_relaxed);
}
#endif

extern "C" MGMEM_PRESSURE_API mgmem_status mgmem_android_borrow_register_v1(
    mgmem_domain *domain, const mgmem_domain_lifetime_v1 *lifetime,
    mgmem_handle_v1 owner, u64 offset, u64 length, uint32_t access, uint32_t flags, u64 *out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = 0;
    if (!domain || !lifetime || lifetime->struct_size < sizeof(*lifetime) ||
        lifetime->version != 1 || !lifetime->retain || !lifetime->release ||
        !length || length > max_token || length > std::numeric_limits<size_t>::max() ||
        access > MGMEM_WRITE || flags != MGMEM_ANDROID_BORROW_INITIALIZED ||
        owner.runtime_origin != mgmem_runtime_origin_v1())
        return MGMEM_INVALID_ARGUMENT;
    try {
        auto entry = std::make_shared<Entry>();
        entry->domain = domain;
        entry->lifetime = *lifetime;
        entry->access = access;
        u64 token;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            if (next_token > max_token) return MGMEM_OVERFLOW;
            token = next_token++;
            registry.emplace(token, entry); // Reserve all publication metadata first.
        }
        try {
            lifetime->retain(lifetime->context);
            entry->pinned = true;
        } catch (...) {
            // The retain contract was violated: whether it took a pin is unknown.
            // Keep the entry for diagnostics and never call the uncertain release.
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->closed = true;
            return MGMEM_PROVIDER_ERROR;
        }
        const auto status = mgmem_borrow_acquire_v1(domain, owner, offset, length, access, &entry->view);
        if (status != MGMEM_OK) {
            try {
                lifetime->release(lifetime->context);
                entry->pinned = false;
            } catch (...) {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->closed = true;
                return MGMEM_PROVIDER_ERROR;
            }
            erase(token, entry);
            return status;
        }
        { std::lock_guard<std::mutex> lock(entry->mutex); entry->ready = true; }
        *out = token;
        return MGMEM_OK;
    } catch (const std::bad_alloc &) {
        return MGMEM_OUT_OF_MEMORY;
    } catch (...) {
        return MGMEM_PROVIDER_ERROR;
    }
}

extern "C" MGMEM_PRESSURE_API mgmem_status mgmem_android_borrow_cancel_v1(u64 token) {
    try { return token ? close_registration(token, false) : MGMEM_INVALID_ARGUMENT; }
    catch (...) { return MGMEM_PROVIDER_ERROR; }
}

#define JNI_NAME(method) Java_io_github_jichuo1_meguviz_memory_android_NativeMemoryLease_##method

extern "C" JNIEXPORT jlong JNICALL JNI_NAME(nativeClaim)(JNIEnv *, jclass, jlong token) {
    try {
        auto entry = token > 0 ? lookup(static_cast<u64>(token)) : nullptr;
        if (!entry) return 0;
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->ready || entry->closed || entry->claimed) return 0;
        entry->claimed = true;
        const auto length = static_cast<jlong>(entry->view.length);
        return entry->access == MGMEM_WRITE ? -length : length;
    } catch (...) { return 0; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeCopy)(JNIEnv *env, jclass, jlong token,
    jlong offset, jbyteArray array, jint array_offset, jint count, jboolean write) {
    try {
        if (token <= 0 || offset < 0 || !array || array_offset < 0 || count < 0)
            return MGMEM_INVALID_ARGUMENT;
        if (write && static_cast<u64>(count) > MGMEM_ANDROID_BORROW_MAX_WRITE_BYTES)
            return MGMEM_INVALID_ARGUMENT;
        const auto array_length = env->GetArrayLength(array);
        if (env->ExceptionCheck()) return MGMEM_PROVIDER_ERROR;
        if (array_offset > array_length || count > array_length - array_offset)
            return MGMEM_INVALID_ARGUMENT;
        auto entry = lookup(static_cast<u64>(token));
        if (!entry) return MGMEM_STALE_HANDLE;
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->ready || entry->closed || !entry->claimed) return MGMEM_IS_CLOSED;
        const auto start = static_cast<u64>(offset);
        if (start > entry->view.length || static_cast<u64>(count) > entry->view.length - start)
            return MGMEM_INVALID_ARGUMENT;
        if (write && entry->access != MGMEM_WRITE) return MGMEM_UNSUPPORTED;
        if (!count) return MGMEM_OK;
        auto *bytes = static_cast<jbyte *>(entry->view.data) + static_cast<size_t>(start);
        if (!write) {
            env->SetByteArrayRegion(array, array_offset, count, bytes);
            return env->ExceptionCheck() ? MGMEM_PROVIDER_ERROR : MGMEM_OK;
        }
        const auto size = static_cast<size_t>(count);
        StagingReservation reservation(size);
        if (!reservation.admitted()) return MGMEM_OUT_OF_BUDGET;
        std::unique_ptr<jbyte[]> staging;
#if defined(MGMEM_BORROW_TESTING)
        if (!fail_next_staging_allocation.exchange(false, std::memory_order_relaxed))
#endif
            staging.reset(new (std::nothrow) jbyte[size]);
        if (!staging) return MGMEM_OUT_OF_MEMORY;
        env->GetByteArrayRegion(array, array_offset, count, staging.get());
        if (env->ExceptionCheck()) return MGMEM_PROVIDER_ERROR;
        std::memcpy(bytes, staging.get(), size);
        return MGMEM_OK;
    } catch (...) { return MGMEM_PROVIDER_ERROR; }
}

// Entry locks, not Java object monitors, protect both borrowed ranges through
// the copy. Tokens are monotonic, so ascending token order also handles opposite
// direction transfers without lock inversion; self-copy acquires just one lock.
extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeTransfer)(JNIEnv *, jclass,
    jlong source_token, jlong source_offset, jlong destination_token, jlong destination_offset, jint count) {
    try {
        if (source_token <= 0 || destination_token <= 0 || source_offset < 0 ||
            destination_offset < 0 || count < 0 ||
            static_cast<u64>(count) > MGMEM_ANDROID_BORROW_MAX_WRITE_BYTES)
            return MGMEM_INVALID_ARGUMENT;
        auto source = lookup(static_cast<u64>(source_token));
        auto destination = source_token == destination_token ? source : lookup(static_cast<u64>(destination_token));
        if (!source || !destination) return MGMEM_STALE_HANDLE;
        auto first = source_token < destination_token ? source : destination;
        auto second = source_token < destination_token ? destination : source;
        std::unique_lock<std::mutex> first_lock(first->mutex);
        std::unique_lock<std::mutex> second_lock;
        if (source != destination) second_lock = std::unique_lock<std::mutex>(second->mutex);
        if (!source->ready || source->closed || !source->claimed ||
            !destination->ready || destination->closed || !destination->claimed)
            return MGMEM_IS_CLOSED;
        if (destination->access != MGMEM_WRITE) return MGMEM_UNSUPPORTED;
        const auto start = static_cast<u64>(source_offset);
        const auto target = static_cast<u64>(destination_offset);
        const auto bytes = static_cast<u64>(count);
        if (start > source->view.length || bytes > source->view.length - start ||
            target > destination->view.length || bytes > destination->view.length - target)
            return MGMEM_INVALID_ARGUMENT;
        if (!count) return MGMEM_OK;
        std::memmove(static_cast<unsigned char *>(destination->view.data) + static_cast<size_t>(target),
                     static_cast<const unsigned char *>(source->view.data) + static_cast<size_t>(start),
                     static_cast<size_t>(bytes));
        return MGMEM_OK;
    } catch (...) { return MGMEM_PROVIDER_ERROR; }
}

extern "C" JNIEXPORT jint JNICALL JNI_NAME(nativeClose)(JNIEnv *, jclass, jlong token) {
    try { return token > 0 ? close_registration(static_cast<u64>(token), true) : MGMEM_INVALID_ARGUMENT; }
    catch (...) { return MGMEM_PROVIDER_ERROR; }
}
