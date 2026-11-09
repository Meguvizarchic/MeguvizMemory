#include <mgmem/memory.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

namespace {
struct Origin {
    alignas(64) unsigned char bytes[64]{};
    std::atomic<int> retains{0}, releases{0};
    std::atomic<mgmem_status> retain_status{MGMEM_OK}, release_status{MGMEM_OK};
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false, entered = false, proceed = false;

    static mgmem_status retain(void *opaque) {
        auto &self = *static_cast<Origin *>(opaque);
        ++self.retains;
        std::unique_lock<std::mutex> lock(self.mutex);
        if (self.block) {
            self.entered = true;
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return self.proceed; });
        }
        return self.retain_status.load();
    }
    static mgmem_status release(void *opaque) {
        auto &self = *static_cast<Origin *>(opaque);
        ++self.releases;
        return self.release_status.load();
    }
    mgmem_external_buffer_checked_v1 descriptor(uint64_t id) {
        return {sizeof(mgmem_external_buffer_checked_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
                bytes, sizeof(bytes), 64, id, MGMEM_READ, 0, this,
                retain, release, mgmem_runtime_origin_v1()};
    }
};

mgmem_domain_options_v1 options() {
    mgmem_domain_options_v1 value{};
    value.struct_size = sizeof(value);
    value.abi_major = MGMEM_ABI_MAJOR;
    value.abi_minor = MGMEM_ABI_MINOR;
    value.mode = MGMEM_SYSTEM;
    value.budget_bytes = 1024;
    return value;
}

void assert_quarantine(mgmem::domain &domain, uint64_t expected_bytes) {
    CHECK(domain.quarantined());
    mgmem_import_snapshot_v1 imports{};
    CHECK(domain.import_snapshot(imports) == MGMEM_OK);
    CHECK(imports.poisoned == 1 && imports.external_backing_bytes == expected_bytes);
    mgmem::owned_allocation denied;
    CHECK(domain.allocate(1, 1, denied) == MGMEM_PROVIDER_ERROR && !denied);
    CHECK(domain.set_budget(1) == MGMEM_PROVIDER_ERROR);
    CHECK(domain.begin_close() == MGMEM_OK);
    CHECK(domain.collect() == MGMEM_PROVIDER_ERROR);
    CHECK(domain.drain(0) == MGMEM_PROVIDER_ERROR);
}

void normal_and_failed_retain() {
    Origin origin;
    mgmem::domain domain;
    CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
    mgmem::owned_allocation allocation;
    auto input = origin.descriptor(1001);
    origin.retain_status = MGMEM_PROVIDER_ERROR;
    CHECK(domain.import_external_checked(input, allocation) == MGMEM_PROVIDER_ERROR);
    CHECK(!allocation && !domain.quarantined() && origin.releases == 0);
    origin.retain_status = MGMEM_OK;
    CHECK(domain.import_external_checked(input, allocation) == MGMEM_OK);
    CHECK(domain.import_external_checked(input, allocation) == MGMEM_INVALID_ARGUMENT && allocation);
    mgmem::borrow view;
    CHECK(allocation.acquire_borrow(0, 64, MGMEM_READ, view) == MGMEM_OK);
    CHECK(!view.write_data());
    CHECK(allocation.release() == MGMEM_OK);
    CHECK(domain.begin_close() == MGMEM_OK && domain.collect() == MGMEM_BUSY);
    CHECK(view.release() == MGMEM_OK && origin.releases == 1);
    CHECK(domain.collect() == MGMEM_OK && domain.drain(0) == MGMEM_OK);
    CHECK(!domain.quarantined());
}

void owner_failure() {
    static Origin origin;
    origin.release_status = MGMEM_BUSY;
    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
        mgmem::owned_allocation allocation;
        auto input = origin.descriptor(1002);
        CHECK(domain.import_external_checked(input, allocation) == MGMEM_OK);
        CHECK(allocation.release() == MGMEM_PROVIDER_ERROR && !allocation);
        CHECK(allocation.release() == MGMEM_OK && origin.releases == 1);
        assert_quarantine(domain, 64);
    }
    CHECK(origin.releases == 1); // domain destructor did not retry or terminate
}

void borrow_failure() {
    static Origin origin;
    origin.release_status = MGMEM_BUSY;
    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
        mgmem::owned_allocation allocation;
        auto input = origin.descriptor(1003);
        CHECK(domain.import_external_checked(input, allocation) == MGMEM_OK);
        mgmem::borrow view;
        CHECK(allocation.acquire_borrow(0, 64, MGMEM_READ, view) == MGMEM_OK);
        CHECK(allocation.release() == MGMEM_OK && origin.releases == 0);
        CHECK(view.release() == MGMEM_PROVIDER_ERROR && !view);
        CHECK(view.release() == MGMEM_OK && origin.releases == 1);
        assert_quarantine(domain, 64);
    }
    CHECK(origin.releases == 1);
}

void destructor_failure() {
    static Origin origin;
    origin.release_status = MGMEM_BUSY;
    {
        mgmem::owned_allocation escaped;
        {
            mgmem::domain domain;
            CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
            auto input = origin.descriptor(1004);
            CHECK(domain.import_external_checked(input, escaped) == MGMEM_OK);
        }
        // The last wrapper destructor must neither retry nor terminate.
    }
    CHECK(origin.releases == 1);
}

void borrow_destructor_failure() {
    static Origin origin;
    origin.release_status = MGMEM_BUSY;
    mgmem::domain domain;
    CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
    mgmem::owned_allocation allocation;
    auto input = origin.descriptor(1008);
    CHECK(domain.import_external_checked(input, allocation) == MGMEM_OK);
    {
        mgmem::borrow view;
        CHECK(allocation.acquire_borrow(0, 64, MGMEM_READ, view) == MGMEM_OK);
        CHECK(allocation.release() == MGMEM_OK);
        // The automatic last-lease failure becomes visible through domain.
    }
    CHECK(origin.releases == 1);
    assert_quarantine(domain, 64);
}

void move_assignment_failure() {
    static Origin first, second;
    first.release_status = MGMEM_BUSY;
    mgmem::domain a, b;
    CHECK(mgmem::domain::create(options(), a) == MGMEM_OK);
    CHECK(mgmem::domain::create(options(), b) == MGMEM_OK);
    mgmem::owned_allocation first_owner, second_owner;
    auto first_input = first.descriptor(1009), second_input = second.descriptor(1010);
    CHECK(a.import_external_checked(first_input, first_owner) == MGMEM_OK);
    CHECK(b.import_external_checked(second_input, second_owner) == MGMEM_OK);
    first_owner = std::move(second_owner);
    CHECK(first_owner && !second_owner && first.releases == 1 && a.quarantined());
    CHECK(first_owner.release() == MGMEM_OK && second.releases == 1 && !b.quarantined());
    assert_quarantine(a, 64);
    CHECK(b.begin_close() == MGMEM_OK && b.collect() == MGMEM_OK && b.drain(0) == MGMEM_OK);
}

void rollback_failure() {
    static Origin origin;
    origin.block = true;
    origin.release_status = MGMEM_BUSY;
    mgmem::domain domain;
    CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
    mgmem::owned_allocation allocation;
    auto input = origin.descriptor(1005);
    mgmem_status result = MGMEM_OK;
    std::thread worker([&] { result = domain.import_external_checked(input, allocation); });
    {
        std::unique_lock<std::mutex> lock(origin.mutex);
        origin.changed.wait(lock, [&] { return origin.entered; });
    }
    CHECK(domain.begin_close() == MGMEM_OK && domain.collect() == MGMEM_BUSY);
    {
        std::lock_guard<std::mutex> lock(origin.mutex);
        origin.proceed = true;
    }
    origin.changed.notify_all();
    worker.join();
    CHECK(result == MGMEM_PROVIDER_ERROR && !allocation && origin.releases == 1);
    assert_quarantine(domain, 64);
}

void concurrent_releases() {
    static Origin first, second;
    first.release_status = second.release_status = MGMEM_BUSY;
    mgmem::domain domain;
    CHECK(mgmem::domain::create(options(), domain) == MGMEM_OK);
    mgmem::owned_allocation first_owner, second_owner;
    auto a = first.descriptor(1006), b = second.descriptor(1007);
    CHECK(domain.import_external_checked(a, first_owner) == MGMEM_OK);
    CHECK(domain.import_external_checked(b, second_owner) == MGMEM_OK);
    std::atomic<unsigned> failures{0};
    std::thread x([owner = std::move(first_owner), &failures]() mutable {
        if (owner.release() == MGMEM_PROVIDER_ERROR && !owner) ++failures;
    });
    std::thread y([owner = std::move(second_owner), &failures]() mutable {
        if (owner.release() == MGMEM_PROVIDER_ERROR && !owner) ++failures;
    });
    x.join(); y.join();
    CHECK(failures == 2 && first.releases == 1 && second.releases == 1);
    assert_quarantine(domain, 128);
}

void arena_normal() {
    Origin origin, plain;
    auto config = options(); config.mode = MGMEM_ARENA; config.arena_chunk_bytes = 128;
    mgmem::domain domain;
    CHECK(mgmem::domain::create(config, domain) == MGMEM_OK);
    auto input = origin.descriptor(1011);
    mgmem::owned_allocation imported, workspace, plain_owner;
    CHECK(domain.import_external_arena_checked(input, imported) == MGMEM_OK);
    mgmem_external_buffer_v1 plain_input{sizeof(mgmem_external_buffer_v1), MGMEM_ABI_MAJOR,
        MGMEM_ABI_MINOR, 0, plain.bytes, sizeof(plain.bytes), 64, 1013, MGMEM_READ, 0,
        &plain, Origin::retain, [](void *p) { (void)Origin::release(p); }, mgmem_runtime_origin_v1()};
    CHECK(domain.import_external_arena(plain_input, plain_owner) == MGMEM_OK);
    CHECK(plain_owner.release() == MGMEM_OK && plain.releases == 1);
    CHECK(domain.allocate(64, 16, workspace) == MGMEM_OK);
    mgmem::borrow input_view, workspace_view;
    CHECK(imported.acquire_borrow(0, 64, MGMEM_READ, input_view) == MGMEM_OK);
    CHECK(workspace.acquire_borrow(0, 64, MGMEM_WRITE, workspace_view) == MGMEM_OK);
    CHECK(input_view.data() == origin.bytes && workspace_view.write_data() != origin.bytes);
    CHECK(imported.release() == MGMEM_OK && workspace.release() == MGMEM_OK);
    CHECK(domain.begin_close() == MGMEM_OK && domain.collect() == MGMEM_BUSY);
    CHECK(input_view.release() == MGMEM_OK && origin.releases == 1);
    CHECK(workspace_view.release() == MGMEM_OK);
    CHECK(domain.collect() == MGMEM_OK && domain.drain(0) == MGMEM_OK);
    mgmem_snapshot_v1 snapshot{};
    CHECK(domain.snapshot(snapshot) == MGMEM_OK && snapshot.backing_bytes == 0);
}

void arena_failure() {
    static Origin origin;
    origin.release_status = MGMEM_BUSY;
    auto config = options(); config.mode = MGMEM_ARENA; config.arena_chunk_bytes = 128;
    mgmem::domain domain;
    CHECK(mgmem::domain::create(config, domain) == MGMEM_OK);
    mgmem::owned_allocation owner;
    auto input = origin.descriptor(1012);
    CHECK(domain.import_external_arena_checked(input, owner) == MGMEM_OK);
    mgmem::borrow view;
    CHECK(owner.acquire_borrow(0, 64, MGMEM_READ, view) == MGMEM_OK);
    CHECK(owner.release() == MGMEM_OK && origin.releases == 0);
    CHECK(view.release() == MGMEM_PROVIDER_ERROR && !view && origin.releases == 1);
    assert_quarantine(domain, 64);
}
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "normal")) normal_and_failed_retain();
    else if (!std::strcmp(argv[1], "owner-failure")) owner_failure();
    else if (!std::strcmp(argv[1], "borrow-failure")) borrow_failure();
    else if (!std::strcmp(argv[1], "destructor-failure")) destructor_failure();
    else if (!std::strcmp(argv[1], "borrow-destructor")) borrow_destructor_failure();
    else if (!std::strcmp(argv[1], "move-assignment")) move_assignment_failure();
    else if (!std::strcmp(argv[1], "rollback-failure")) rollback_failure();
    else if (!std::strcmp(argv[1], "concurrent")) concurrent_releases();
    else if (!std::strcmp(argv[1], "arena-normal")) arena_normal();
    else if (!std::strcmp(argv[1], "arena-failure")) arena_failure();
    else return 2;
    std::printf("{\"suite\":\"cpp-checked-import\",\"case\":\"%s\",\"status\":\"PASS\"}\n", argv[1]);
    std::fflush(stdout);
    // Uncertain resources intentionally remain quarantined; an isolated
    // process exits without claiming successful release or leak freedom.
    if (std::strcmp(argv[1], "normal") && std::strcmp(argv[1], "arena-normal")) std::_Exit(0);
    return 0;
}
