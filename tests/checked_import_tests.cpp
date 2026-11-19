#include <mgmem/mgmem.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); std::abort(); } } while (false)

namespace {
struct Owner {
    alignas(64) unsigned char data[64]{};
    std::atomic<unsigned> retains{0}, releases{0};
    std::atomic<bool> legacy_storage_released{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool block_retain = false, block_release = false, entered = false, proceed = false;
    bool release_throws = false;
    mgmem_status retain_status = MGMEM_OK, release_status = MGMEM_OK;

    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; changed.notify_all();
        CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return proceed; }));
    }
    void wait_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
    }
    void unblock() {
        std::lock_guard<std::mutex> lock(mutex);
        proceed = true; changed.notify_all();
    }
    static mgmem_status retain(void *p) {
        auto &o = *static_cast<Owner *>(p); ++o.retains;
        if (o.block_retain) o.block();
        return o.retain_status;
    }
    static mgmem_status release_checked(void *p) {
        auto &o = *static_cast<Owner *>(p); ++o.releases;
        if (o.block_release) o.block();
        if (o.release_throws) throw std::runtime_error("controlled callback violation");
        return o.release_status;
    }
    static void release_legacy(void *p) {
        auto &o = *static_cast<Owner *>(p); ++o.releases;
        // Logical ownership of this single pool slot transfers before the
        // callback's unrelated remaining work has finished.
        o.legacy_storage_released.store(true, std::memory_order_release);
        if (o.block_release) o.block();
    }
    mgmem_external_buffer_checked_v1 checked() {
        return {sizeof(mgmem_external_buffer_checked_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
            data, sizeof(data), alignof(Owner), 1234, MGMEM_READ, 0, this,
            retain, release_checked, mgmem_runtime_origin_v1()};
    }
    mgmem_external_buffer_v1 legacy() {
        return {sizeof(mgmem_external_buffer_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
            data, sizeof(data), alignof(Owner), 1234, MGMEM_READ, 0, this,
            retain, release_legacy, mgmem_runtime_origin_v1()};
    }
};

mgmem_domain *create(const mgmem_provider_v1 *provider = nullptr) {
    mgmem_domain_options_v1 o{}; o.struct_size = sizeof(o); o.abi_major = MGMEM_ABI_MAJOR;
    o.abi_minor = MGMEM_ABI_MINOR; o.mode = MGMEM_SYSTEM; o.budget_bytes = 128; o.provider = provider;
    mgmem_domain *d = nullptr; CHECK(mgmem_domain_create_v1(&o, &d) == MGMEM_OK); return d;
}
void finish(mgmem_domain *d) {
    CHECK(mgmem_domain_begin_close_v1(d) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(d) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(d, 0) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(d) == MGMEM_OK);
}
void quarantined(mgmem_domain *d) {
    mgmem_snapshot_v1 s{}; mgmem_import_snapshot_v1 i{};
    CHECK(mgmem_domain_snapshot_v1(d, &s, sizeof(s)) == MGMEM_OK);
    CHECK(mgmem_domain_import_snapshot_v1(d, &i, sizeof(i)) == MGMEM_OK);
    CHECK(s.backing_bytes == 64 && s.reserved_bytes == 0 && s.active_allocations == 0 && s.active_leases == 0);
    CHECK(i.poisoned == 1 && i.external_backing_bytes == 64 && i.external_backing_count == 1);
    CHECK(mgmem_domain_begin_close_v1(d) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(d) == MGMEM_PROVIDER_ERROR);
    CHECK(mgmem_domain_drain_v1(d, 0) == MGMEM_PROVIDER_ERROR);
    CHECK(mgmem_domain_destroy_v1(d) == MGMEM_BUSY);
}
struct AliasProvider {
    void *address; std::atomic<unsigned> deallocations{0};
    static mgmem_status allocate(void *p, uint64_t, uint64_t, void **out) {
        *out = static_cast<AliasProvider *>(p)->address; return MGMEM_OK;
    }
    static void deallocate(void *p, void *, uint64_t, uint64_t) { ++static_cast<AliasProvider *>(p)->deallocations; }
    static void retain(void *) {} static void release(void *) {}
    mgmem_provider_v1 table() {
        return {sizeof(mgmem_provider_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
            this, allocate, deallocate, retain, release};
    }
};
// One controlled slot: the old origin relinquishes it during legacy release;
// a different provider may then acquire it. No allocator is asked to free the
// stack storage, and the new provider may release only its own acquired slot.
struct RecyclingProvider {
    Owner *origin; std::atomic<bool> leased{false}; std::atomic<unsigned> deallocations{0};
    static mgmem_status allocate(void *p, uint64_t bytes, uint64_t alignment, void **out) {
        auto &self = *static_cast<RecyclingProvider *>(p); *out = nullptr;
        if (!self.origin->legacy_storage_released.load(std::memory_order_acquire) ||
            bytes != sizeof(self.origin->data) || alignment > alignof(Owner)) return MGMEM_OUT_OF_MEMORY;
        bool expected = false;
        if (!self.leased.compare_exchange_strong(expected, true)) return MGMEM_OUT_OF_MEMORY;
        *out = self.origin->data; return MGMEM_OK;
    }
    static void deallocate(void *p, void *address, uint64_t bytes, uint64_t alignment) {
        auto &self = *static_cast<RecyclingProvider *>(p);
        CHECK(address == self.origin->data && bytes == sizeof(self.origin->data) && alignment <= alignof(Owner));
        CHECK(self.leased.exchange(false)); ++self.deallocations;
    }
    static void retain(void *) {} static void release(void *) {}
    mgmem_provider_v1 table() {
        return {sizeof(mgmem_provider_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
            this, allocate, deallocate, retain, release};
    }
};
void check_claim(Owner &o) {
    auto *other = create(); auto b = o.checked(); mgmem_handle_v1 h{};
    CHECK(mgmem_import_external_checked_v1(other, &b, &h) == MGMEM_BUSY && h.id == 0);
    finish(other);
}
void normal() {
    CHECK((mgmem_capabilities_v1() & MGMEM_CAP_CHECKED_EXTERNAL_RELEASE) != 0);
    CHECK(mgmem_abi_version_v1() == ((1u << 16) | 1u));
    Owner o; auto *d = create(); auto b = o.checked(); mgmem_handle_v1 h{1,1,1};
    auto bad = b; bad.struct_size = 0;
    CHECK(mgmem_import_external_checked_v1(d, &bad, &h) == MGMEM_INVALID_ARGUMENT && h.id == 0);
    bad = b; bad.release_checked = nullptr;
    CHECK(mgmem_import_external_checked_v1(d, &bad, &h) == MGMEM_INVALID_ARGUMENT && h.id == 0);
    bad = b; bad.abi_minor += 1;
    CHECK(mgmem_import_external_checked_v1(d, &bad, &h) == MGMEM_UNSUPPORTED && h.id == 0);
    bad = b; bad.managing_runtime_origin = 0;
    CHECK(mgmem_import_external_checked_v1(d, &bad, &h) == MGMEM_STALE_HANDLE && h.id == 0);
    o.retain_status = MGMEM_OUT_OF_MEMORY;
    CHECK(mgmem_import_external_checked_v1(d, &b, &h) == MGMEM_OUT_OF_MEMORY && o.releases == 0);
    o.retain_status = MGMEM_OK;
    CHECK(mgmem_import_external_checked_v1(d, &b, &h) == MGMEM_OK);
    mgmem_borrow_v1 lease{}, denied{};
    CHECK(mgmem_borrow_acquire_v1(d, h, 0, 64, MGMEM_WRITE, &denied) == MGMEM_UNSUPPORTED);
    CHECK(mgmem_borrow_acquire_v1(d, h, 0, 64, MGMEM_READ, &lease) == MGMEM_OK);
    CHECK(mgmem_release_v1(d, h) == MGMEM_OK && o.releases == 0);
    CHECK(mgmem_domain_begin_close_v1(d) == MGMEM_OK && mgmem_domain_collect_v1(d) == MGMEM_BUSY);
    CHECK(mgmem_borrow_release_v1(d, lease.lease) == MGMEM_OK && o.releases == 1);
    CHECK(mgmem_borrow_release_v1(d, lease.lease) == MGMEM_STALE_HANDLE && o.releases == 1);
    finish(d);
    auto *again = create(); CHECK(mgmem_import_external_checked_v1(again, &b, &h) == MGMEM_OK);
    CHECK(mgmem_release_v1(again, h) == MGMEM_OK && o.releases == 2); finish(again);
}
void failure(bool lease, bool throws) {
    Owner o; o.release_status = MGMEM_BUSY; o.release_throws = throws;
    auto *d = create(); auto b = o.checked(); mgmem_handle_v1 h{};
    CHECK(mgmem_import_external_checked_v1(d, &b, &h) == MGMEM_OK);
    if (lease) {
        mgmem_borrow_v1 view{}; CHECK(mgmem_borrow_acquire_v1(d, h, 0, 64, MGMEM_READ, &view) == MGMEM_OK);
        CHECK(mgmem_release_v1(d, h) == MGMEM_OK);
        CHECK(mgmem_borrow_release_v1(d, view.lease) == MGMEM_PROVIDER_ERROR);
        CHECK(mgmem_borrow_release_v1(d, view.lease) == MGMEM_STALE_HANDLE);
    } else CHECK(mgmem_release_v1(d, h) == MGMEM_PROVIDER_ERROR);
    CHECK(mgmem_release_v1(d, h) == MGMEM_STALE_HANDLE && o.releases == 1);
    check_claim(o); quarantined(d);
}
void rollback() {
    Owner o; o.block_retain = true; o.release_status = MGMEM_BUSY;
    auto *d = create(); auto b = o.checked(); mgmem_handle_v1 h{}; mgmem_status result = MGMEM_OK;
    std::thread worker([&] { result = mgmem_import_external_checked_v1(d, &b, &h); });
    o.wait_entered(); CHECK(mgmem_domain_begin_close_v1(d) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(d) == MGMEM_BUSY);
    auto drain = std::async(std::launch::async, [&] { return mgmem_domain_drain_v1(d, 15000); });
    CHECK(drain.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
    o.unblock(); worker.join();
    CHECK(drain.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(drain.get() == MGMEM_PROVIDER_ERROR);
    CHECK(result == MGMEM_PROVIDER_ERROR && h.id == 0 && o.retains == 1 && o.releases == 1);
    check_claim(o); quarantined(d);
}
void race_checked() {
    Owner o; o.block_release = true; auto *d = create(); mgmem_handle_v1 h{};
    auto cb = o.checked();
    CHECK(mgmem_import_external_checked_v1(d, &cb, &h) == MGMEM_OK);
    mgmem_status result = MGMEM_BUSY;
    std::thread worker([&] { result = mgmem_release_v1(d, h); });
    o.wait_entered(); check_claim(o);
    AliasProvider alias{o.data}; auto p = alias.table(); auto *other = create(&p); mgmem_handle_v1 denied{};
    CHECK(mgmem_allocate_v1(other, 64, 64, &denied) == MGMEM_PROVIDER_ERROR);
    CHECK(denied.id == 0 && alias.deallocations == 0);
    o.unblock(); worker.join(); CHECK(result == MGMEM_OK && o.releases == 1); finish(d);
    CHECK(mgmem_domain_begin_close_v1(other) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(other) == MGMEM_PROVIDER_ERROR && mgmem_domain_destroy_v1(other) == MGMEM_BUSY);
}
void race_legacy_reuse() {
    Owner o; o.block_release = true; auto *d = create(); mgmem_handle_v1 h{};
    auto descriptor = o.legacy();
    CHECK(mgmem_import_external_v1(d, &descriptor, &h) == MGMEM_OK);
    mgmem_status result = MGMEM_BUSY;
    std::thread worker([&] { result = mgmem_release_v1(d, h); });
    o.wait_entered(); CHECK(o.legacy_storage_released.load(std::memory_order_acquire));
    // A fresh import is still rejected while the old external callback is in
    // flight, whereas an allocator may legitimately reuse the freed slot.
    check_claim(o);
    RecyclingProvider recycled{&o}; auto p = recycled.table(); auto *other = create(&p); mgmem_handle_v1 new_owner{};
    CHECK(mgmem_allocate_v1(other, 64, 64, &new_owner) == MGMEM_OK);
    mgmem_borrow_v1 new_view{};
    CHECK(mgmem_borrow_acquire_v1(other, new_owner, 0, 64, MGMEM_WRITE, &new_view) == MGMEM_OK);
    std::memset(new_view.data, 0x5a, 64);
    o.unblock(); worker.join(); CHECK(result == MGMEM_OK && o.releases == 1);
    CHECK(static_cast<unsigned char *>(new_view.data)[63] == 0x5a);
    CHECK(mgmem_release_v1(other, new_owner) == MGMEM_OK && recycled.deallocations == 0);
    CHECK(mgmem_borrow_release_v1(other, new_view.lease) == MGMEM_OK && recycled.deallocations == 1);
    CHECK(!recycled.leased); finish(other); finish(d);
}
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "normal")) normal();
    else if (!std::strcmp(argv[1], "failure-owner")) failure(false, false);
    else if (!std::strcmp(argv[1], "failure-lease")) failure(true, false);
    else if (!std::strcmp(argv[1], "exception")) failure(false, true);
    else if (!std::strcmp(argv[1], "rollback")) rollback();
    else if (!std::strcmp(argv[1], "race-checked")) race_checked();
    else if (!std::strcmp(argv[1], "race-legacy")) race_legacy_reuse();
    else return 2;
    std::printf("{\"suite\":\"checked-import\",\"case\":\"%s\",\"status\":\"PASS\"}\n", argv[1]);
    std::fflush(stdout);
    // Poisoned-domain cases intentionally preserve uncertain claims/owners.
    // Isolated processes do not pretend these resources were normally freed.
    if (std::strcmp(argv[1], "normal") && std::strcmp(argv[1], "race-legacy")) std::_Exit(0);
    return 0;
}
