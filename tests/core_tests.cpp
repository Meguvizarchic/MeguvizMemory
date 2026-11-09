#include <mgmem/mgmem.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
std::atomic<unsigned> checks{0};

[[noreturn]] void fail(const char *expression, const char *file, int line) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    std::abort();
}

#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) fail(#expression, __FILE__, __LINE__); \
} while (false)

mgmem_domain_options_v1 options(uint32_t mode = MGMEM_SYSTEM,
                               uint64_t budget = 4096, uint64_t chunk = 256) {
    mgmem_domain_options_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_major = MGMEM_ABI_MAJOR;
    result.abi_minor = MGMEM_ABI_MINOR;
    result.mode = mode;
    result.budget_bytes = budget;
    result.arena_chunk_bytes = chunk;
    return result;
}

mgmem_domain *create(mgmem_domain_options_v1 config = options()) {
    mgmem_domain *domain = nullptr;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_OK);
    CHECK(domain != nullptr);
    return domain;
}

mgmem_snapshot_v1 snapshot(mgmem_domain *domain) {
    mgmem_snapshot_v1 result{};
    CHECK(mgmem_domain_snapshot_v1(domain, &result, sizeof(result)) == MGMEM_OK);
    CHECK(result.struct_size == sizeof(result));
    CHECK(result.abi_major == MGMEM_ABI_MAJOR);
    CHECK(result.backing_bytes == result.live_bytes + result.retiring_bytes + result.retained_bytes);
    CHECK(result.backing_bytes + result.reserved_bytes <= result.budget_bytes);
    return result;
}

mgmem_handle_v1 allocate(mgmem_domain *domain, uint64_t bytes, uint64_t alignment = 16) {
    mgmem_handle_v1 result{};
    CHECK(mgmem_allocate_v1(domain, bytes, alignment, &result) == MGMEM_OK);
    CHECK(result.domain_id != 0 && result.id != 0);
    return result;
}

mgmem_borrow_v1 borrow(mgmem_domain *domain, mgmem_handle_v1 allocation,
                     uint64_t bytes, uint32_t access = MGMEM_WRITE, uint64_t offset = 0) {
    mgmem_borrow_v1 result{};
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, offset, bytes, access, &result) == MGMEM_OK);
    CHECK(result.data != nullptr && result.length == bytes);
    return result;
}

void close(mgmem_domain *domain) {
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    CHECK(snapshot(domain).state == MGMEM_CLOSED);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
}

struct Provider {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<void *, std::pair<uint64_t, uint64_t>> active;
    unsigned retains = 0;
    unsigned releases = 0;
    unsigned allocation_calls = 0;
    unsigned deallocation_calls = 0;
    bool fail_next = false;
    bool throw_oom_next = false;
    bool throw_error_next = false;
    bool block_next = false;
    bool entered = false;
    bool proceed = false;
    bool block_deallocate_next = false;
    bool entered_deallocate = false;
    bool proceed_deallocate = false;

    static void *native_allocate(uint64_t bytes, uint64_t alignment) {
#if defined(_WIN32)
        return _aligned_malloc(static_cast<size_t>(bytes), static_cast<size_t>(alignment));
#else
        void *address = nullptr;
        if (posix_memalign(&address, static_cast<size_t>(alignment), static_cast<size_t>(bytes)) != 0)
            return nullptr;
        return address;
#endif
    }

    static void native_free(void *address) {
#if defined(_WIN32)
        _aligned_free(address);
#else
        std::free(address);
#endif
    }

    static mgmem_status allocate(void *context, uint64_t bytes, uint64_t alignment, void **out) {
        auto &self = *static_cast<Provider *>(context);
        *out = nullptr;
        {
            std::unique_lock<std::mutex> lock(self.mutex);
            CHECK(self.retains == 1 && self.releases == 0);
            ++self.allocation_calls;
            if (self.block_next) {
                self.block_next = false;
                self.entered = true;
                self.changed.notify_all();
                self.changed.wait(lock, [&] { return self.proceed; });
            }
            if (self.fail_next) {
                self.fail_next = false;
                return MGMEM_OUT_OF_MEMORY;
            }
            // Hostile callback fault injection verifies the C exception boundary;
            // production providers still have the documented no-throw contract.
            if (self.throw_oom_next) {
                self.throw_oom_next = false;
                throw std::bad_alloc();
            }
            if (self.throw_error_next) {
                self.throw_error_next = false;
                throw 1;
            }
        }
        void *address = native_allocate(bytes, alignment);
        if (!address) return MGMEM_OUT_OF_MEMORY;
        try {
            std::lock_guard<std::mutex> lock(self.mutex);
            CHECK(self.active.emplace(address, std::make_pair(bytes, alignment)).second);
        } catch (...) {
            native_free(address);
            return MGMEM_OUT_OF_MEMORY;
        }
        *out = address;
        return MGMEM_OK;
    }

    static void deallocate(void *context, void *address, uint64_t bytes, uint64_t alignment) {
        auto &self = *static_cast<Provider *>(context);
        {
            std::unique_lock<std::mutex> lock(self.mutex);
            CHECK(self.retains == 1 && self.releases == 0);
            const auto found = self.active.find(address);
            CHECK(found != self.active.end());
            CHECK(found->second.first == bytes && found->second.second == alignment);
            if (self.block_deallocate_next) {
                self.block_deallocate_next = false;
                self.entered_deallocate = true;
                self.changed.notify_all();
                self.changed.wait(lock, [&] { return self.proceed_deallocate; });
            }
            self.active.erase(found);
            ++self.deallocation_calls;
        }
        native_free(address);
    }

    static void retain(void *context) {
        auto &self = *static_cast<Provider *>(context);
        std::lock_guard<std::mutex> lock(self.mutex);
        CHECK(self.retains == 0 && self.releases == 0);
        ++self.retains;
    }

    static void release(void *context) {
        auto &self = *static_cast<Provider *>(context);
        std::lock_guard<std::mutex> lock(self.mutex);
        CHECK(self.retains == 1 && self.releases == 0 && self.active.empty());
        ++self.releases;
    }

    mgmem_provider_v1 table() {
        return {sizeof(mgmem_provider_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
                this, allocate, deallocate, retain, release};
    }

    void wait_for_entry() {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }

    void unblock() {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true;
        changed.notify_all();
    }

    void wait_for_deallocation() {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered_deallocate; }));
    }

    void unblock_deallocation() {
        std::lock_guard<std::mutex> lock(mutex);
        proceed_deallocate = true;
        changed.notify_all();
    }
};

void test_abi_and_arithmetic() {
    CHECK(mgmem_abi_version_v1() == ((MGMEM_ABI_MAJOR << 16) | MGMEM_ABI_MINOR));
    mgmem_domain *domain = nullptr;
    auto config = options();
    config.struct_size = 0;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_INVALID_ARGUMENT && domain == nullptr);
    config = options();
    config.abi_major += 1;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_UNSUPPORTED && domain == nullptr);
    config = options();
    config.abi_minor += 1;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_UNSUPPORTED && domain == nullptr);
    config = options();
    config.required_flags = 1;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_UNSUPPORTED && domain == nullptr);
    config = options();
    config.reserved = 1;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_INVALID_ARGUMENT);
    config = options();
    config.budget_bytes = 0;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_INVALID_ARGUMENT);
    config = options(MGMEM_ARENA);
    config.arena_chunk_bytes = 0;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_domain_create_v1(nullptr, &domain) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_domain_create_v1(&config, nullptr) == MGMEM_INVALID_ARGUMENT);

    Provider provider;
    auto table = provider.table();
    config = options();
    config.provider = &table;
    table.required_flags = 0x80000000u;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_UNSUPPORTED);
    table.required_flags = 0;
    table.deallocate = nullptr;
    CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_INVALID_ARGUMENT);
    CHECK(provider.retains == 0);

    uint64_t result = 99;
    CHECK(mgmem_checked_mul_v1(854, 480, &result) == MGMEM_OK && result == 409920);
    CHECK(mgmem_checked_mul_v1(result, 3, &result) == MGMEM_OK && result == 1229760);
    CHECK(mgmem_checked_mul_v1(std::numeric_limits<uint64_t>::max(), 2, &result) == MGMEM_OVERFLOW);
    CHECK(result == 0);
    CHECK(mgmem_checked_mul_v1(0, std::numeric_limits<uint64_t>::max(), &result) == MGMEM_OK && result == 0);
    CHECK(mgmem_checked_mul_v1(1, 2, nullptr) == MGMEM_INVALID_ARGUMENT);
    for (const uint64_t page : {4096ull, 16384ull, 65536ull}) {
        CHECK(mgmem_checked_round_up_v1(0, page, &result) == MGMEM_OK && result == 0);
        CHECK(mgmem_checked_round_up_v1(page - 1, page, &result) == MGMEM_OK && result == page);
        CHECK(mgmem_checked_round_up_v1(page, page, &result) == MGMEM_OK && result == page);
        CHECK(mgmem_checked_round_up_v1(page + 1, page, &result) == MGMEM_OK && result == page * 2);
        CHECK(mgmem_checked_round_up_v1(std::numeric_limits<uint64_t>::max(), page, &result) == MGMEM_OVERFLOW);
    }
    CHECK(mgmem_checked_round_up_v1(2, 3, &result) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_checked_round_up_v1(2, 0, &result) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_checked_round_up_v1(2, 1, &result) == MGMEM_OK && result == 2);
    CHECK(mgmem_checked_round_up_v1(2, 4, nullptr) == MGMEM_INVALID_ARGUMENT);
}

void test_system_budget_and_layout() {
    auto *domain = create(options(MGMEM_SYSTEM, 128));
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_BUSY);
    mgmem_handle_v1 failed{9, 9};
    CHECK(mgmem_allocate_v1(domain, 0, 16, &failed) == MGMEM_INVALID_ARGUMENT && failed.id == 0);
    CHECK(mgmem_allocate_v1(domain, 8, 3, &failed) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_allocate_v1(domain, 8, 0, &failed) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_allocate_v1(domain, 8, 16, nullptr) == MGMEM_INVALID_ARGUMENT);
    const auto allocation = allocate(domain, 128, 64);
    auto view = borrow(domain, allocation, 128);
    CHECK(reinterpret_cast<uintptr_t>(view.data) % 64 == 0);
    std::memset(view.data, 0xa7, 128);
    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    CHECK(mgmem_allocate_v1(domain, 1, 1, &failed) == MGMEM_OUT_OF_BUDGET && failed.id == 0);
    view = borrow(domain, allocation, 128, MGMEM_READ);
    for (unsigned i = 0; i < 128; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == 0xa7);
    auto state = snapshot(domain);
    CHECK(state.backing_bytes == 128 && state.live_bytes == 128 && state.retiring_bytes == 0);
    CHECK(state.reserved_bytes == 0 && state.peak_backing_bytes == 128);
    CHECK(state.allocation_count == 1 && state.cumulative_requested_bytes == 128);
    CHECK(state.provider_allocation_count == 1 && state.page_size > 0);
    CHECK(mgmem_arena_reset_v1(domain) == MGMEM_UNSUPPORTED);
    CHECK(mgmem_domain_trim_v1(domain) == MGMEM_BUSY);
    mgmem_snapshot_v1 undersized{};
    CHECK(mgmem_domain_snapshot_v1(domain, &undersized, sizeof(undersized) - 1) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
    CHECK(snapshot(domain).backing_bytes == 0);
    close(domain);
}

void test_provider_oom_and_pairing() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_SYSTEM, 1024);
    config.provider = &table;
    auto *domain = create(config);
    CHECK(provider.retains == 1 && provider.releases == 0);
    // The core owns a vtable copy, not this caller's stack table.
    table.allocate = nullptr;
    const auto allocation = allocate(domain, 64, 32);
    auto view = borrow(domain, allocation, 64);
    std::memset(view.data, 0x39, 64);
    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    {
        std::lock_guard<std::mutex> lock(provider.mutex);
        provider.fail_next = true;
    }
    mgmem_handle_v1 failed{7, 7};
    CHECK(mgmem_allocate_v1(domain, 128, 64, &failed) == MGMEM_OUT_OF_MEMORY);
    CHECK(failed.domain_id == 0 && failed.id == 0);
    {
        std::lock_guard<std::mutex> lock(provider.mutex);
        provider.throw_oom_next = true;
    }
    CHECK(mgmem_allocate_v1(domain, 128, 64, &failed) == MGMEM_OUT_OF_MEMORY);
    CHECK(failed.domain_id == 0 && failed.id == 0);
    {
        std::lock_guard<std::mutex> lock(provider.mutex);
        provider.throw_error_next = true;
    }
    CHECK(mgmem_allocate_v1(domain, 128, 64, &failed) == MGMEM_PROVIDER_ERROR);
    CHECK(failed.domain_id == 0 && failed.id == 0);
    auto state = snapshot(domain);
    CHECK(state.backing_bytes == 64 && state.reserved_bytes == 0 && state.live_bytes == 64);
    CHECK(state.allocation_count == 1 && state.provider_allocation_count == 1);
    view = borrow(domain, allocation, 64, MGMEM_READ);
    for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == 0x39);
    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
    CHECK(provider.deallocation_calls == 1 && provider.active.empty());
    close(domain);
    CHECK(provider.allocation_calls == 4 && provider.retains == 1 && provider.releases == 1);
}

void test_allocate_close_race() {
    Provider provider;
    provider.block_next = true;
    auto table = provider.table();
    auto config = options(MGMEM_SYSTEM, 64);
    config.provider = &table;
    auto *domain = create(config);
    mgmem_status late_status = MGMEM_OK;
    mgmem_handle_v1 late_handle{1, 1};
    std::thread allocator([&] { late_status = mgmem_allocate_v1(domain, 64, 16, &late_handle); });
    provider.wait_for_entry();
    const auto pending = snapshot(domain);
    CHECK(pending.reserved_bytes == 64 && pending.backing_bytes == 0);
    CHECK(pending.peak_backing_bytes == 64 && pending.allocation_count == 0);
    mgmem_handle_v1 rejected{};
    CHECK(mgmem_allocate_v1(domain, 1, 16, &rejected) == MGMEM_OUT_OF_BUDGET);
    // This call must finish while the provider callback is deliberately blocked.
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_TIMEOUT);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_allocate_v1(domain, 1, 16, &rejected) == MGMEM_IS_CLOSED);
    CHECK(provider.releases == 0 && provider.deallocation_calls == 0);
    provider.unblock();
    allocator.join();
    CHECK(late_status == MGMEM_IS_CLOSED && late_handle.id == 0 && late_handle.domain_id == 0);
    const auto rolled_back = snapshot(domain);
    CHECK(rolled_back.backing_bytes == 0 && rolled_back.reserved_bytes == 0);
    CHECK(rolled_back.live_bytes == 0 && rolled_back.active_allocations == 0);
    CHECK(provider.allocation_calls == 1 && provider.deallocation_calls == 1);
    CHECK(provider.active.empty() && provider.releases == 0);
    close(domain);
    CHECK(provider.releases == 1);
}

void test_borrow_rules_and_handle_identity() {
    auto *domain = create();
    auto *other = create();
    const auto allocation = allocate(domain, 128, 64);
    mgmem_borrow_v1 rejected{};
    CHECK(mgmem_borrow_acquire_v1(other, allocation, 0, 1, MGMEM_READ, &rejected) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_release_v1(other, allocation) == MGMEM_STALE_HANDLE);
    auto writer = borrow(domain, allocation, 128);
    std::memset(writer.data, 0x55, 128);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 1, MGMEM_READ, &rejected) == MGMEM_BUSY);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 1, MGMEM_WRITE, &rejected) == MGMEM_BUSY);
    CHECK(mgmem_borrow_release_v1(domain, writer.lease) == MGMEM_OK);
    auto first = borrow(domain, allocation, 32, MGMEM_READ, 16);
    auto second = borrow(domain, allocation, 64, MGMEM_READ, 32);
    CHECK(static_cast<unsigned char *>(first.data)[0] == 0x55);
    CHECK(static_cast<unsigned char *>(second.data) == static_cast<unsigned char *>(first.data) + 16);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 1, MGMEM_WRITE, &rejected) == MGMEM_BUSY);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 129, 1, MGMEM_READ, &rejected) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 127, 2, MGMEM_READ, &rejected) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, std::numeric_limits<uint64_t>::max(), 2,
                                 MGMEM_READ, &rejected) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 0, MGMEM_READ, &rejected) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 1, 2, &rejected) == MGMEM_INVALID_ARGUMENT);
    CHECK(mgmem_borrow_acquire_v1(domain, first.lease, 0, 1, MGMEM_READ, &rejected) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_borrow_release_v1(domain, allocation) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_borrow_release_v1(other, first.lease) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 1, MGMEM_READ, &rejected) == MGMEM_STALE_HANDLE);
    const auto state = snapshot(domain);
    CHECK(state.live_bytes == 0 && state.retiring_bytes == 128 && state.backing_bytes == 128);
    CHECK(state.active_leases == 2);
    CHECK(mgmem_borrow_release_v1(domain, first.lease) == MGMEM_OK);
    CHECK(snapshot(domain).backing_bytes == 128);
    CHECK(mgmem_borrow_release_v1(domain, first.lease) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_borrow_release_v1(domain, second.lease) == MGMEM_OK);
    CHECK(snapshot(domain).backing_bytes == 0);
    close(other);
    close(domain);
}

void test_late_lease_after_close() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_SYSTEM, 64);
    config.provider = &table;
    auto *domain = create(config);
    const auto allocation = allocate(domain, 64);
    auto writer = borrow(domain, allocation, 64);
    for (unsigned i = 0; i < 64; ++i) static_cast<unsigned char *>(writer.data)[i] = static_cast<unsigned char>(i);
    CHECK(mgmem_borrow_release_v1(domain, writer.lease) == MGMEM_OK);
    const auto reader = borrow(domain, allocation, 64, MGMEM_READ);
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool proceed = false;
    // The task has its lease before enqueueing. It accesses bytes only after the
    // owner has released and the domain has begun closing.
    std::thread worker([&] {
        {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate.wait(lock, [&] { return proceed; });
        }
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(reader.data)[i] == i);
        CHECK(mgmem_borrow_release_v1(domain, reader.lease) == MGMEM_OK);
    });
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 1) == MGMEM_TIMEOUT);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_BUSY);
    const auto state = snapshot(domain);
    CHECK(state.state == MGMEM_CLOSING && state.backing_bytes == 64 && state.retiring_bytes == 64);
    CHECK(provider.deallocation_calls == 0 && provider.releases == 0);
    mgmem_borrow_v1 rejected{};
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 1, MGMEM_READ, &rejected) == MGMEM_IS_CLOSED);
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        proceed = true;
        gate.notify_all();
    }
    worker.join();
    CHECK(provider.deallocation_calls == 1 && provider.releases == 0);
    close(domain);
    CHECK(provider.releases == 1);
}

void test_arena_lifetime_and_accounting() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 512, 256);
    config.provider = &table;
    auto *domain = create(config);
    const auto first = allocate(domain, 32, 16);
    const auto second = allocate(domain, 64, 16);
    auto first_view = borrow(domain, first, 32);
    auto second_view = borrow(domain, second, 64);
    const auto first_address = reinterpret_cast<uintptr_t>(first_view.data);
    const auto second_address = reinterpret_cast<uintptr_t>(second_view.data);
    CHECK(first_address + 32 <= second_address || second_address + 64 <= first_address);
    std::memset(first_view.data, 0x11, 32);
    std::memset(second_view.data, 0x22, 64);
    auto state = snapshot(domain);
    CHECK(state.backing_bytes == 256 && state.live_bytes == 96 && state.retained_bytes == 160);
    CHECK(state.provider_allocation_count == 1 && state.allocation_count == 2);
    CHECK(mgmem_arena_reset_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_domain_trim_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_borrow_release_v1(domain, second_view.lease) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, second) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, first) == MGMEM_OK);
    state = snapshot(domain);
    CHECK(state.live_bytes == 0 && state.retiring_bytes == 32 && state.retained_bytes == 224);
    CHECK(mgmem_arena_reset_v1(domain) == MGMEM_BUSY);
    for (unsigned i = 0; i < 32; ++i) CHECK(static_cast<unsigned char *>(first_view.data)[i] == 0x11);
    CHECK(mgmem_borrow_release_v1(domain, first_view.lease) == MGMEM_OK);
    state = snapshot(domain);
    CHECK(state.retained_bytes == 256 && state.active_allocations == 0 && state.active_leases == 0);
    const auto generation = state.generation;
    CHECK(mgmem_arena_reset_v1(domain) == MGMEM_OK);
    CHECK(snapshot(domain).generation == generation + 1);
    const auto reused = allocate(domain, 32, 16);
    CHECK(reused.id != first.id);
    auto reused_view = borrow(domain, reused, 32);
    CHECK(reinterpret_cast<uintptr_t>(reused_view.data) == first_address);
    CHECK(snapshot(domain).provider_allocation_count == 1);
    CHECK(mgmem_release_v1(domain, first) == MGMEM_STALE_HANDLE);
    CHECK(mgmem_borrow_release_v1(domain, reused_view.lease) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, reused) == MGMEM_OK);
    CHECK(mgmem_domain_trim_v1(domain) == MGMEM_OK);
    CHECK(snapshot(domain).backing_bytes == 0 && provider.deallocation_calls == 1);
    const auto after_trim = allocate(domain, 256);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    CHECK(mgmem_release_v1(domain, after_trim) == MGMEM_OK);
    close(domain);
    CHECK(provider.allocation_calls == 2 && provider.deallocation_calls == 2 && provider.releases == 1);
}

void test_arena_first_fit_cursor() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 512, 256);
    config.provider = &table;
    auto *domain = create(config);
    std::vector<mgmem_handle_v1> owners;
    std::vector<mgmem_borrow_v1> views;
    const auto take = [&](uint64_t bytes, uint64_t alignment) {
        const auto owner = allocate(domain, bytes, alignment);
        const auto view = borrow(domain, owner, bytes, MGMEM_READ);
        owners.push_back(owner);
        views.push_back(view);
        return reinterpret_cast<uintptr_t>(view.data);
    };
    const auto first = take(160, 16);
    const auto second = take(192, 16);
    CHECK(take(64, 16) == first + 160);
    CHECK(take(64, 16) == second + 192);
    // A different shape must revisit the earlier, smaller hole.
    CHECK(take(16, 16) == first + 224);
    CHECK(take(16, 8) == first + 240);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    CHECK(mgmem_arena_reset_v1(domain) == MGMEM_OK);
    owners.clear(); views.clear();
    CHECK(take(64, 16) == first);
    CHECK(take(64, 16) == first + 64);
    for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    CHECK(mgmem_domain_trim_v1(domain) == MGMEM_OK);
    CHECK(snapshot(domain).backing_bytes == 0);
    const auto full = allocate(domain, 256, 16);
    {
        std::lock_guard<std::mutex> lock(provider.mutex);
        provider.fail_next = true;
    }
    mgmem_handle_v1 rejected{};
    CHECK(mgmem_allocate_v1(domain, 64, 16, &rejected) == MGMEM_OUT_OF_MEMORY && rejected.id == 0);
    const auto after_failure = allocate(domain, 64, 16);
    CHECK(snapshot(domain).provider_allocation_count == 4);
    CHECK(mgmem_release_v1(domain, full) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, after_failure) == MGMEM_OK);
    close(domain);
    CHECK(provider.active.empty() && provider.allocation_calls == 5 && provider.deallocation_calls == 4);
}

void test_arena_first_fit_pending_publication() {
    Provider provider;
    provider.block_next = true;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 1024, 256);
    config.provider = &table;
    auto *domain = create(config);
    mgmem_handle_v1 pending{};
    mgmem_status pending_status = MGMEM_BUSY;
    std::thread worker([&] { pending_status = mgmem_allocate_v1(domain, 64, 16, &pending); });
    provider.wait_for_entry();
    const auto second = allocate(domain, 64, 16);
    const auto second_view = borrow(domain, second, 64, MGMEM_READ);
    const auto third = allocate(domain, 64, 16);
    const auto fourth = allocate(domain, 64, 16);
    const auto fourth_view = borrow(domain, fourth, 64, MGMEM_READ);
    CHECK(reinterpret_cast<uintptr_t>(fourth_view.data) ==
          reinterpret_cast<uintptr_t>(second_view.data) + 128);
    provider.unblock();
    worker.join();
    CHECK(pending_status == MGMEM_OK);
    const auto first_view = borrow(domain, pending, 64, MGMEM_READ);
    const auto after_publish = allocate(domain, 64, 16);
    const auto after_view = borrow(domain, after_publish, 64, MGMEM_READ);
    // Publishing the previously skipped first list node invalidates the hint.
    CHECK(reinterpret_cast<uintptr_t>(after_view.data) ==
          reinterpret_cast<uintptr_t>(first_view.data) + 64);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    for (const auto view : {second_view, fourth_view, first_view, after_view})
        CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    for (const auto owner : {pending, second, third, fourth, after_publish})
        CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    close(domain);
    CHECK(provider.active.empty() && provider.deallocation_calls == 2);
}

void test_arena_shape_retention_mixed_and_reset() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 512, 256);
    config.provider = &table;
    auto *domain = create(config);
    std::vector<mgmem_handle_v1> owners;
    std::vector<mgmem_borrow_v1> views;
    const auto take = [&](uint64_t bytes, uint64_t alignment) {
        const auto owner = allocate(domain, bytes, alignment);
        const auto view = borrow(domain, owner, bytes, MGMEM_READ);
        owners.push_back(owner); views.push_back(view);
        CHECK(reinterpret_cast<uintptr_t>(view.data) %
              (alignment < sizeof(void *) ? sizeof(void *) : alignment) == 0);
        return reinterpret_cast<uintptr_t>(view.data);
    };
    const auto release_all = [&] {
        for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
        for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
        owners.clear(); views.clear();
    };
    const auto first = take(160, 16);
    const auto second = take(192, 16);
    CHECK(take(64, 1) == first + 160);
    CHECK(take(16, 16) == first + 224);
    CHECK(take(64, 8) == second + 192);
    // An intervening shape must still use the earliest smaller hole.
    CHECK(take(8, 8) == first + 240);
    CHECK(take(8, 1) == first + 248);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    release_all();
    CHECK(mgmem_arena_reset_v1(domain) == MGMEM_OK);
    CHECK(take(32, 1) == first);
    CHECK(take(32, 8) == first + 32);
    // A long different-shape phase stays within the existing first block.
    for (uint64_t index = 0; index < 16; ++index)
        CHECK(take(8, 8) == first + 64 + index * 8);
    CHECK(take(32, 1) == first + 192);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    release_all();
    CHECK(mgmem_domain_trim_v1(domain) == MGMEM_OK);
    CHECK(snapshot(domain).backing_bytes == 0 && provider.deallocation_calls == 2);
    take(64, 1);
    CHECK(snapshot(domain).provider_allocation_count == 3);
    release_all(); close(domain);
    CHECK(provider.active.empty() && provider.deallocation_calls == 3 && provider.releases == 1);
}

void test_arena_shape_retention_end_and_budget() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 512, 256);
    config.provider = &table;
    auto *domain = create(config);
    std::vector<mgmem_handle_v1> owners;
    std::vector<mgmem_borrow_v1> views;
    const auto take = [&](uint64_t bytes, uint64_t alignment) {
        const auto owner = allocate(domain, bytes, alignment);
        const auto view = borrow(domain, owner, bytes, MGMEM_READ);
        owners.push_back(owner); views.push_back(view);
        return reinterpret_cast<uintptr_t>(view.data);
    };
    const auto first = take(192, 16);
    const auto second = take(224, 16);
    CHECK(take(64, 16) == first + 192);
    mgmem_handle_v1 denied{};
    CHECK(mgmem_allocate_v1(domain, 64, 16, &denied) == MGMEM_OUT_OF_BUDGET && denied.id == 0);
    CHECK(mgmem_domain_set_budget_v1(domain, 0) == MGMEM_OK);
    CHECK(mgmem_allocate_v1(domain, 16, 8, &denied) == MGMEM_OUT_OF_BUDGET && denied.id == 0);
    CHECK(mgmem_domain_set_budget_v1(domain, 512) == MGMEM_OK);
    CHECK(take(16, 8) == second + 224);
    CHECK(mgmem_allocate_v1(domain, 64, 16, &denied) == MGMEM_OUT_OF_BUDGET && denied.id == 0);
    CHECK(take(16, 8) == second + 240);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    CHECK(mgmem_domain_set_budget_v1(domain, 768) == MGMEM_OK);
    const auto third = take(64, 16);
    CHECK(take(64, 16) == third + 64);
    CHECK(snapshot(domain).provider_allocation_count == 3);
    for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_TIMEOUT);
    CHECK(snapshot(domain).retiring_bytes == 640);
    for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    close(domain);
    CHECK(provider.active.empty() && provider.deallocation_calls == 3 && provider.releases == 1);
}

void test_arena_shape_retention_pending_mixed() {
    Provider provider;
    provider.block_next = true;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 1024, 256);
    config.provider = &table;
    auto *domain = create(config);
    mgmem_handle_v1 pending{};
    mgmem_status pending_status = MGMEM_BUSY;
    std::thread worker([&] { pending_status = mgmem_allocate_v1(domain, 64, 16, &pending); });
    provider.wait_for_entry();
    std::vector<mgmem_handle_v1> owners;
    std::vector<mgmem_borrow_v1> views;
    const auto take = [&](uint64_t bytes, uint64_t alignment) {
        const auto owner = allocate(domain, bytes, alignment);
        const auto view = borrow(domain, owner, bytes, MGMEM_READ);
        owners.push_back(owner); views.push_back(view);
        return reinterpret_cast<uintptr_t>(view.data);
    };
    const auto second = take(64, 16);
    CHECK(take(64, 8) == second + 64);
    CHECK(take(16, 16) == second + 128);
    CHECK(take(64, 1) == second + 144);
    CHECK(take(16, 16) == second + 208);
    provider.unblock(); worker.join();
    CHECK(pending_status == MGMEM_OK);
    const auto first_view = borrow(domain, pending, 64, MGMEM_READ);
    const auto first = reinterpret_cast<uintptr_t>(first_view.data);
    CHECK(take(64, 8) == first + 64);
    CHECK(take(16, 16) == first + 128);
    CHECK(take(64, 1) == first + 144);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    CHECK(mgmem_borrow_release_v1(domain, first_view.lease) == MGMEM_OK);
    for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, pending) == MGMEM_OK);
    close(domain);
    CHECK(provider.active.empty() && provider.deallocation_calls == 2 && provider.releases == 1);
}

void test_arena_first_fit_larger_bytes_lower_alignment() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 512, 256);
    config.provider = &table;
    auto *domain = create(config);
    std::vector<mgmem_handle_v1> owners;
    std::vector<mgmem_borrow_v1> views;
    const auto take = [&](uint64_t bytes, uint64_t alignment) {
        const auto owner = allocate(domain, bytes, alignment);
        const auto view = borrow(domain, owner, bytes, MGMEM_READ);
        owners.push_back(owner); views.push_back(view);
        CHECK(reinterpret_cast<uintptr_t>(view.data) % alignment == 0);
        return reinterpret_cast<uintptr_t>(view.data);
    };
    const auto first = take(160, 16);
    const auto second = take(192, 64);
    CHECK(take(64, 64) == second + 192);
    // Larger bytes alone do not preserve the prefix proof: the earlier block
    // was skipped for alignment and remains the first fit at lower alignment.
    CHECK(take(80, 16) == first + 160);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    close(domain);
    CHECK(provider.active.empty() && provider.deallocation_calls == 2 && provider.releases == 1);
}

void test_arena_first_fit_smaller_bytes_higher_alignment() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 768, 384);
    config.provider = &table;
    auto *domain = create(config);
    std::vector<mgmem_handle_v1> owners;
    std::vector<mgmem_borrow_v1> views;
    const auto take = [&](uint64_t bytes, uint64_t alignment) {
        const auto owner = allocate(domain, bytes, alignment);
        const auto view = borrow(domain, owner, bytes, MGMEM_READ);
        owners.push_back(owner); views.push_back(view);
        CHECK(reinterpret_cast<uintptr_t>(view.data) % alignment == 0);
        return reinterpret_cast<uintptr_t>(view.data);
    };
    const auto first = take(256, 128);
    const auto second = take(160, 64);
    CHECK(take(160, 64) == second + 192);
    // Higher alignment alone does not preserve the prefix proof: the earlier
    // block's remaining 128 bytes can satisfy this smaller request.
    CHECK(take(128, 128) == first + 256);
    CHECK(snapshot(domain).provider_allocation_count == 2);
    for (const auto &view : views) CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    for (const auto owner : owners) CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    close(domain);
    CHECK(provider.active.empty() && provider.deallocation_calls == 2 && provider.releases == 1);
}

void test_growth_peak_and_failure_atomicity() {
    auto *domain = create(options(MGMEM_SYSTEM, 192));
    const auto old_allocation = allocate(domain, 64);
    auto old_view = borrow(domain, old_allocation, 64);
    std::memset(old_view.data, 0xac, 64);
    CHECK(mgmem_borrow_release_v1(domain, old_view.lease) == MGMEM_OK);
    const auto replacement = allocate(domain, 128);
    auto state = snapshot(domain);
    CHECK(state.backing_bytes == 192 && state.peak_backing_bytes == 192 && state.live_bytes == 192);
    mgmem_handle_v1 rejected{};
    CHECK(mgmem_allocate_v1(domain, 1, 16, &rejected) == MGMEM_OUT_OF_BUDGET && rejected.id == 0);
    old_view = borrow(domain, old_allocation, 64, MGMEM_READ);
    for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(old_view.data)[i] == 0xac);
    CHECK(mgmem_borrow_release_v1(domain, old_view.lease) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, old_allocation) == MGMEM_OK);
    state = snapshot(domain);
    CHECK(state.backing_bytes == 128 && state.peak_backing_bytes == 192);
    CHECK(mgmem_release_v1(domain, replacement) == MGMEM_OK);
    close(domain);
}

void test_drain_waits_for_real_completion() {
    auto *domain = create();
    const auto allocation = allocate(domain, 64);
    auto lease = borrow(domain, allocation, 64);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, std::numeric_limits<uint64_t>::max()) == MGMEM_OVERFLOW);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_TIMEOUT);
    std::thread completion([&] {
        std::memset(lease.data, 0xe2, 64);
        CHECK(mgmem_borrow_release_v1(domain, lease.lease) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    });
    CHECK(mgmem_domain_drain_v1(domain, 10000) == MGMEM_OK);
    completion.join();
    CHECK(snapshot(domain).state == MGMEM_CLOSED);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
}

void test_drain_poll_during_blocked_collection() {
    Provider provider;
    auto table = provider.table();
    auto config = options(MGMEM_ARENA, 256, 256);
    config.provider = &table;
    auto *domain = create(config);
    const auto allocation = allocate(domain, 64);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
    CHECK(snapshot(domain).retained_bytes == 256);
    provider.block_deallocate_next = true;
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(provider.deallocation_calls == 0);
    // Merely polling an idle, closing arena must not enter the provider either.
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_TIMEOUT);
    CHECK(provider.deallocation_calls == 0);
    mgmem_status collection_status = MGMEM_BUSY;
    std::thread cleanup_worker([&] { collection_status = mgmem_domain_collect_v1(domain); });
    provider.wait_for_deallocation();
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_TIMEOUT);
    CHECK(mgmem_domain_drain_v1(domain, 1) == MGMEM_TIMEOUT);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_BUSY);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_BUSY);
    const auto state = snapshot(domain);
    CHECK(state.state == MGMEM_CLOSING && state.backing_bytes == 256);
    CHECK(provider.releases == 0);
    provider.unblock_deallocation();
    cleanup_worker.join();
    CHECK(collection_status == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    CHECK(provider.deallocation_calls == 1 && provider.releases == 0);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    CHECK(provider.releases == 1);
}

void test_concurrent_stress() {
    constexpr unsigned workers = 6;
    constexpr unsigned iterations = 300;
    auto *domain = create(options(MGMEM_SYSTEM, workers * 256));
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (unsigned worker = 0; worker < workers; ++worker) {
        threads.emplace_back([&, worker] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (unsigned iteration = 0; iteration < iterations; ++iteration) {
                const unsigned bytes = 16 + ((worker * 31 + iteration * 17) % 112);
                const auto allocation = allocate(domain, bytes, 64);
                auto view = borrow(domain, allocation, bytes);
                CHECK(reinterpret_cast<uintptr_t>(view.data) % 64 == 0);
                const auto value = static_cast<unsigned char>((worker + iteration) & 255);
                std::memset(view.data, value, bytes);
                if ((iteration & 1) == 0) {
                    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
                    for (unsigned i = 0; i < bytes; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == value);
                    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
                } else {
                    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
                    view = borrow(domain, allocation, bytes, MGMEM_READ);
                    for (unsigned i = 0; i < bytes; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == value);
                    CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
                    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
                }
                if (iteration % 37 == 0) snapshot(domain);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto &thread : threads) thread.join();
    const auto state = snapshot(domain);
    CHECK(state.allocation_count == workers * iterations);
    CHECK(state.provider_allocation_count == workers * iterations);
    CHECK(state.backing_bytes == 0 && state.reserved_bytes == 0 && state.live_bytes == 0);
    CHECK(state.active_allocations == 0 && state.active_leases == 0);
    close(domain);
}

struct Test { const char *name; void (*run)(); };
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--arena-first-fit") == 0) {
        test_arena_first_fit_cursor();
        test_arena_first_fit_pending_publication();
        std::printf("{\"suite\":\"mgmem-arena-first-fit\",\"tests\":2,\"checks\":%u,\"status\":\"PASS\"}\n",
                    checks.load());
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--arena-shape-retention") == 0) {
        test_arena_shape_retention_mixed_and_reset();
        test_arena_shape_retention_end_and_budget();
        test_arena_shape_retention_pending_mixed();
        test_arena_first_fit_larger_bytes_lower_alignment();
        test_arena_first_fit_smaller_bytes_higher_alignment();
        std::printf("{\"suite\":\"mgmem-arena-shape-retention\",\"tests\":5,\"checks\":%u,\"status\":\"PASS\"}\n",
                    checks.load());
        return 0;
    }
    if (argc != 1) return 2;
    const Test tests[] = {
        {"abi_and_checked_arithmetic", test_abi_and_arithmetic},
        {"system_budget_layout_and_rollback", test_system_budget_and_layout},
        {"provider_oom_and_exact_pairing", test_provider_oom_and_pairing},
        {"allocation_reservation_close_race", test_allocate_close_race},
        {"borrow_access_and_handle_identity", test_borrow_rules_and_handle_identity},
        {"late_cross_thread_lease_after_close", test_late_lease_after_close},
        {"arena_retained_reset_and_trim", test_arena_lifetime_and_accounting},
        {"growth_coexistence_peak", test_growth_peak_and_failure_atomicity},
        {"drain_completion_and_timeout", test_drain_waits_for_real_completion},
        {"drain_poll_during_blocked_collection", test_drain_poll_during_blocked_collection},
        {"concurrent_system_stress", test_concurrent_stress}
    };
    for (const auto &test : tests) {
        test.run();
        std::printf("PASS %s\n", test.name);
    }
    std::printf("{\"suite\":\"mgmem-core-v1\",\"tests\":%u,\"checks\":%u,\"status\":\"PASS\"}\n",
                static_cast<unsigned>(sizeof(tests) / sizeof(tests[0])), checks.load());
    return 0;
}
