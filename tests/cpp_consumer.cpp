#include <mgmem/memory.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
void check(bool condition, const char *expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL cpp_consumer:%d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

struct Provider {
    std::atomic<unsigned> retained{0};
    std::atomic<unsigned> released{0};
    std::atomic<unsigned> allocated{0};
    std::atomic<unsigned> deallocated{0};
    void *address = nullptr;
    uint64_t bytes = 0;
    uint64_t alignment = 0;

    static mgmem_status allocate(void *context, uint64_t bytes, uint64_t alignment, void **out) {
        auto &self = *static_cast<Provider *>(context);
        CHECK(self.retained == 1 && self.released == 0 && self.address == nullptr);
        *out = nullptr;
#if defined(_WIN32)
        *out = _aligned_malloc(static_cast<size_t>(bytes), static_cast<size_t>(alignment));
#else
        if (posix_memalign(out, static_cast<size_t>(alignment), static_cast<size_t>(bytes)) != 0)
            *out = nullptr;
#endif
        if (!*out) return MGMEM_OUT_OF_MEMORY;
        self.address = *out;
        self.bytes = bytes;
        self.alignment = alignment;
        ++self.allocated;
        return MGMEM_OK;
    }

    static void deallocate(void *context, void *address, uint64_t bytes, uint64_t alignment) {
        auto &self = *static_cast<Provider *>(context);
        CHECK(self.retained == 1 && self.released == 0);
        CHECK(self.address == address && self.bytes == bytes && self.alignment == alignment);
#if defined(_WIN32)
        _aligned_free(address);
#else
        std::free(address);
#endif
        self.address = nullptr;
        ++self.deallocated;
    }

    static void retain(void *context) {
        auto &self = *static_cast<Provider *>(context);
        CHECK(self.retained++ == 0);
    }
    static void release(void *context) {
        auto &self = *static_cast<Provider *>(context);
        CHECK(self.address == nullptr && self.allocated == self.deallocated);
        CHECK(self.released++ == 0);
    }

    mgmem_provider_v1 table() {
        return {sizeof(mgmem_provider_v1), MGMEM_ABI_MAJOR, MGMEM_ABI_MINOR, 0,
                this, allocate, deallocate, retain, release};
    }
};

mgmem_domain_options_v1 options(uint32_t mode, const mgmem_provider_v1 *provider = nullptr) {
    mgmem_domain_options_v1 result{};
    result.struct_size = sizeof(result);
    result.abi_major = MGMEM_ABI_MAJOR;
    result.abi_minor = MGMEM_ABI_MINOR;
    result.mode = mode;
    result.budget_bytes = 128;
    result.arena_chunk_bytes = 128;
    result.provider = provider;
    return result;
}

void test_borrow_outlives_wrappers(uint32_t mode) {
    Provider provider;
    auto table = provider.table();
    mgmem::borrow escaped;
    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options(mode, &table), domain) == MGMEM_OK);
        mgmem::owned_allocation allocation;
        CHECK(domain.allocate(128, 64, allocation) == MGMEM_OK);
        mgmem::borrow writer;
        CHECK(allocation.acquire_borrow(0, 128, MGMEM_WRITE, writer) == MGMEM_OK);
        CHECK(writer.write_data() != nullptr);
        std::memset(writer.write_data(), 0x6a, 128);
        CHECK(writer.release() == MGMEM_OK);
        CHECK(allocation.acquire_borrow(0, 128, MGMEM_READ, escaped) == MGMEM_OK);
        CHECK(escaped.write_data() == nullptr);
        CHECK(domain.reset() == (mode == MGMEM_ARENA ? MGMEM_BUSY : MGMEM_UNSUPPORTED));
        CHECK(domain.begin_close() == MGMEM_OK);
        CHECK(domain.collect() == MGMEM_BUSY);
        CHECK(domain.drain(0) == MGMEM_TIMEOUT);
        // Both the domain wrapper and allocation owner leave scope, yet the
        // separately held reader keeps the native domain and backing alive.
    }
    CHECK(provider.allocated == 1 && provider.deallocated == 0 && provider.released == 0);
    CHECK(escaped && escaped.size_bytes() == 128);
    const auto *data = static_cast<const unsigned char *>(escaped.data());
    for (unsigned i = 0; i < 128; ++i) CHECK(data[i] == 0x6a);
    mgmem::borrow moved = std::move(escaped);
    CHECK(!escaped && escaped.data() == nullptr && escaped.size_bytes() == 0);
    // Final cleanup runs on this worker after its last byte access. No UI owner
    // is required to block waiting for the lease that protects those bytes.
    std::thread cleanup([view = std::move(moved)]() mutable {
        const auto *bytes = static_cast<const unsigned char *>(view.data());
        for (unsigned i = 0; i < 128; ++i) CHECK(bytes[i] == 0x6a);
        CHECK(view.release() == MGMEM_OK);
        CHECK(!view && view.data() == nullptr);
        CHECK(view.release() == MGMEM_OK);
    });
    cleanup.join();
    CHECK(provider.allocated == 1 && provider.deallocated == 1 && provider.released == 1);
    CHECK(provider.address == nullptr);
}

void test_allocation_outlives_domain() {
    Provider provider;
    auto table = provider.table();
    mgmem::owned_allocation escaped;
    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options(MGMEM_SYSTEM, &table), domain) == MGMEM_OK);
        CHECK(domain.allocate(128, 16, escaped) == MGMEM_OK);
    }
    CHECK(provider.released == 0 && provider.deallocated == 0);
    mgmem::borrow view;
    CHECK(escaped.acquire_borrow(0, 128, MGMEM_WRITE, view) == MGMEM_OK);
    std::memset(view.write_data(), 0x14, 128);
    CHECK(view.release() == MGMEM_OK);
    mgmem::owned_allocation moved = std::move(escaped);
    CHECK(!escaped && escaped.size_bytes() == 0);
    CHECK(moved.release() == MGMEM_OK);
    CHECK(moved.release() == MGMEM_OK);
    CHECK(provider.allocated == 1 && provider.deallocated == 1 && provider.released == 1);
}

void test_budget_failure_preserves_old_data() {
    mgmem::domain domain;
    CHECK(mgmem::domain::create(options(MGMEM_SYSTEM), domain) == MGMEM_OK);
    mgmem::owned_allocation old;
    CHECK(domain.allocate(128, 64, old) == MGMEM_OK);
    mgmem::borrow writer;
    CHECK(old.acquire_borrow(0, 128, MGMEM_WRITE, writer) == MGMEM_OK);
    std::memset(writer.write_data(), 0xd3, 128);
    CHECK(writer.release() == MGMEM_OK);
    mgmem::owned_allocation failed;
    CHECK(domain.allocate(1, 16, failed) == MGMEM_OUT_OF_BUDGET);
    CHECK(!failed && old && old.size_bytes() == 128);
    CHECK(domain.allocate(1, 16, old) == MGMEM_INVALID_ARGUMENT && old.size_bytes() == 128);
    mgmem::borrow reader;
    CHECK(old.acquire_borrow(0, 128, MGMEM_READ, reader) == MGMEM_OK);
    CHECK(reader.write_data() == nullptr);
    const auto *bytes = static_cast<const unsigned char *>(reader.data());
    for (unsigned i = 0; i < 128; ++i) CHECK(bytes[i] == 0xd3);
    mgmem::borrow rejected;
    CHECK(old.acquire_borrow(0, 1, MGMEM_WRITE, rejected) == MGMEM_BUSY && !rejected);
    CHECK(old.acquire_borrow(0, 1, MGMEM_READ, reader) == MGMEM_INVALID_ARGUMENT);
    CHECK(reader.release() == MGMEM_OK);
    CHECK(old.release() == MGMEM_OK);
    mgmem_snapshot_v1 state{};
    CHECK(domain.snapshot(state) == MGMEM_OK && state.backing_bytes == 0 && state.active_leases == 0);
    mgmem::domain moved = std::move(domain);
    CHECK(!domain && moved);
    CHECK(domain.begin_close() == MGMEM_INVALID_ARGUMENT);
    CHECK(moved.begin_close() == MGMEM_OK);
    CHECK(moved.collect() == MGMEM_OK && moved.drain(0) == MGMEM_OK);
}

void test_empty_and_invalid_wrappers() {
    mgmem::domain domain;
    mgmem::owned_allocation allocation;
    mgmem::borrow view;
    CHECK(!domain && !allocation && !view);
    CHECK(view.data() == nullptr && view.write_data() == nullptr && view.size_bytes() == 0);
    CHECK(allocation.release() == MGMEM_OK && view.release() == MGMEM_OK);
    CHECK(domain.allocate(1, 1, allocation) == MGMEM_INVALID_ARGUMENT);
    CHECK(allocation.acquire_borrow(0, 1, MGMEM_READ, view) == MGMEM_INVALID_ARGUMENT);
    auto bad = options(MGMEM_SYSTEM);
    bad.required_flags = 1;
    CHECK(mgmem::domain::create(bad, domain) == MGMEM_UNSUPPORTED && !domain);
    CHECK(mgmem::domain::create(options(MGMEM_ARENA), domain) == MGMEM_OK);
    CHECK(domain.reset() == MGMEM_OK && domain.trim() == MGMEM_OK);
    CHECK(mgmem::domain::create(options(MGMEM_SYSTEM), domain) == MGMEM_INVALID_ARGUMENT);
}

void test_scoped_allocation(uint32_t mode) {
    mgmem::domain domain;
    unsigned calls = 0;
    auto body = [&](void *data, uint64_t bytes) {
        ++calls;
        CHECK(data && bytes == 64);
        CHECK((reinterpret_cast<uintptr_t>(data) & (alignof(std::max_align_t) - 1)) == 0);
        std::memset(data, 0x7b, static_cast<size_t>(bytes));
        mgmem_snapshot_v1 state{};
        CHECK(domain.snapshot(state) == MGMEM_OK);
        CHECK(state.active_allocations == 1 && state.active_leases == 1 && state.live_bytes == bytes);
        CHECK(domain.reset() == (mode == MGMEM_ARENA ? MGMEM_BUSY : MGMEM_UNSUPPORTED));
    };
    CHECK(domain.with_allocation(64, body) == MGMEM_INVALID_ARGUMENT && calls == 0);
    auto config = options(mode);
    config.budget_bytes = 1024;
    CHECK(mgmem::domain::create(config, domain) == MGMEM_OK);
    CHECK(domain.with_allocation(64, body) == MGMEM_OK && calls == 1);
    mgmem_snapshot_v1 state{};
    CHECK(domain.snapshot(state) == MGMEM_OK && state.active_allocations == 0 && state.active_leases == 0);
    CHECK(domain.with_allocation(0, body) == MGMEM_INVALID_ARGUMENT && calls == 1);
    CHECK(domain.with_allocation(64, 3, body) == MGMEM_INVALID_ARGUMENT && calls == 1);
    CHECK(domain.with_allocation(2048, body) == MGMEM_OUT_OF_BUDGET && calls == 1);
    CHECK(domain.with_allocation(64, 64, [&](void *data, uint64_t bytes) -> mgmem_status {
        ++calls;
        CHECK(data && bytes == 64 && (reinterpret_cast<uintptr_t>(data) & 63) == 0);
        return MGMEM_BUSY;
    }) == MGMEM_BUSY && calls == 2);
    CHECK(domain.snapshot(state) == MGMEM_OK && state.active_allocations == 0 && state.active_leases == 0);

    struct CallbackError {} marker;
    bool caught = false;
    try {
        (void)domain.with_allocation(32, [&](void *data, uint64_t bytes) {
            CHECK(data && bytes == 32);
            throw &marker;
        });
    } catch (CallbackError *error) {
        caught = error == &marker;
    }
    CHECK(caught);
    CHECK(domain.snapshot(state) == MGMEM_OK && state.active_allocations == 0 && state.active_leases == 0);
    CHECK(domain.with_allocation(32, [&](void *outer, uint64_t) -> mgmem_status {
        return domain.with_allocation(32, [&](void *inner, uint64_t bytes) {
            CHECK(inner != outer && bytes == 32);
            CHECK(domain.snapshot(state) == MGMEM_OK && state.active_allocations == 2 && state.active_leases == 2);
        });
    }) == MGMEM_OK);
    CHECK(domain.snapshot(state) == MGMEM_OK && state.active_allocations == 0 && state.active_leases == 0);
    if (mode == MGMEM_ARENA) CHECK(domain.reset() == MGMEM_OK);
    CHECK(domain.begin_close() == MGMEM_OK);
    CHECK(domain.with_allocation(64, body) == MGMEM_IS_CLOSED && calls == 2);
}

void test_scoped_allocation_keeps_domain_alive() {
    Provider provider;
    auto table = provider.table();
    mgmem::domain domain;
    CHECK(mgmem::domain::create(options(MGMEM_SYSTEM, &table), domain) == MGMEM_OK);
    CHECK(domain.with_allocation(128, 64, [&](void *data, uint64_t bytes) {
        // The scoped owner/borrow hold the native domain through callback return.
        domain = mgmem::domain{};
        CHECK(!domain && provider.released == 0 && provider.deallocated == 0);
        std::memset(data, 0x5c, static_cast<size_t>(bytes));
    }) == MGMEM_OK);
    CHECK(provider.allocated == 1 && provider.deallocated == 1 && provider.released == 1);
}

void test_convenient_creators(uint32_t mode) {
    Provider provider;
    auto table = provider.table();
    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create_system(0, domain, &table) == MGMEM_INVALID_ARGUMENT && !domain);
        CHECK(mgmem::domain::create_arena(128, 0, domain, &table) == MGMEM_INVALID_ARGUMENT && !domain);
        auto invalid_provider = table;
        invalid_provider.required_flags = 1;
        CHECK(mgmem::domain::create_system(128, domain, &invalid_provider) == MGMEM_UNSUPPORTED && !domain);
        CHECK(provider.retained == 0 && provider.released == 0);
        const auto created = mode == MGMEM_SYSTEM
            ? mgmem::domain::create_system(128, domain, &table)
            : mgmem::domain::create_arena(128, 128, domain, &table);
        CHECK(created == MGMEM_OK && domain && provider.retained == 1);
        mgmem_snapshot_v1 initial{};
        CHECK(domain.snapshot(initial) == MGMEM_OK && initial.budget_bytes == 128);
        CHECK(initial.state == MGMEM_OPEN && initial.active_allocations == 0 && initial.active_leases == 0);
        CHECK(domain.reset() == (mode == MGMEM_ARENA ? MGMEM_OK : MGMEM_UNSUPPORTED));
        CHECK(mgmem::domain::create_system(256, domain, &table) == MGMEM_INVALID_ARGUMENT);
        CHECK(mgmem::domain::create_arena(256, 256, domain, &table) == MGMEM_INVALID_ARGUMENT);
        mgmem_snapshot_v1 unchanged{};
        CHECK(domain.snapshot(unchanged) == MGMEM_OK);
        CHECK(unchanged.domain_id == initial.domain_id && unchanged.budget_bytes == 128 && provider.retained == 1);
        mgmem::owned_allocation owner;
        CHECK(domain.allocate(32, owner) == MGMEM_OK && owner.size_bytes() == 32);
        CHECK(provider.alignment == alignof(std::max_align_t));
        CHECK(provider.bytes == (mode == MGMEM_ARENA ? 128u : 32u));
        CHECK(domain.allocate(16, owner) == MGMEM_INVALID_ARGUMENT && owner.size_bytes() == 32);
        mgmem::owned_allocation rejected;
        CHECK(domain.allocate(129, rejected) == MGMEM_OUT_OF_BUDGET && !rejected);
        CHECK(owner.with_write([](void *data, uint64_t bytes) {
            CHECK(data && bytes == 32);
            CHECK(reinterpret_cast<uintptr_t>(data) % alignof(std::max_align_t) == 0);
            std::memset(data, 0xa3, static_cast<size_t>(bytes));
        }) == MGMEM_OK);
    }
    CHECK(provider.retained == 1 && provider.released == 1 && provider.allocated == 1 && provider.deallocated == 1);
}

void test_scoped_borrow(uint32_t mode) {
    mgmem::domain domain;
    CHECK((mode == MGMEM_SYSTEM ? mgmem::domain::create_system(512, domain)
        : mgmem::domain::create_arena(512, 256, domain)) == MGMEM_OK);
    mgmem::owned_allocation owner;
    unsigned calls = 0;
    auto rejected_read = [&](const void *, uint64_t) { ++calls; };
    auto rejected_write = [&](void *, uint64_t) { ++calls; };
    CHECK(owner.with_read(rejected_read) == MGMEM_INVALID_ARGUMENT && calls == 0);
    CHECK(owner.with_write(rejected_write) == MGMEM_INVALID_ARGUMENT && calls == 0);
    CHECK(domain.allocate(128, owner) == MGMEM_OK);
    CHECK(owner.with_write([](void *data, uint64_t bytes) {
        static_assert(std::is_same<decltype(data), void *>::value, "WRITE pointer must be mutable");
        CHECK(data && bytes == 128);
        auto *out = static_cast<unsigned char *>(data);
        for (unsigned i = 0; i < 128; ++i) out[i] = static_cast<unsigned char>(i);
    }) == MGMEM_OK);
    CHECK(owner.with_write(16, 32, [&](void *data, uint64_t bytes) -> mgmem_status {
        CHECK(bytes == 32);
        std::memset(data, 0x88, static_cast<size_t>(bytes));
        CHECK(owner.with_read(rejected_read) == MGMEM_BUSY && calls == 0);
        CHECK(owner.with_write(rejected_write) == MGMEM_BUSY && calls == 0);
        return MGMEM_BUSY;
    }) == MGMEM_BUSY);
    CHECK(owner.with_read([&](const void *data, uint64_t bytes) -> mgmem_status {
        static_assert(std::is_same<decltype(data), const void *>::value, "READ pointer must be const");
        CHECK(data && bytes == 128);
        const auto *in = static_cast<const unsigned char *>(data);
        for (unsigned i = 0; i < 128; ++i)
            CHECK(in[i] == (i >= 16 && i < 48 ? 0x88 : i));
        CHECK(owner.with_read(16, 32, [&](const void *part, uint64_t length) {
            CHECK(part == in + 16 && length == 32);
            mgmem_snapshot_v1 state{};
            CHECK(domain.snapshot(state) == MGMEM_OK && state.active_leases == 2 && state.active_allocations == 1);
        }) == MGMEM_OK);
        CHECK(owner.with_write(rejected_write) == MGMEM_BUSY && calls == 0);
        return MGMEM_OUT_OF_BUDGET;
    }) == MGMEM_OUT_OF_BUDGET);
    CHECK(owner.with_read(128, 1, rejected_read) == MGMEM_INVALID_ARGUMENT && calls == 0);
    CHECK(owner.with_read(UINT64_MAX, 1, rejected_read) == MGMEM_INVALID_ARGUMENT && calls == 0);
    CHECK(owner.with_write(0, 0, rejected_write) == MGMEM_INVALID_ARGUMENT && calls == 0);
    struct BodyError {} marker;
    unsigned caught = 0;
    auto throws = [&](auto *, uint64_t) { throw &marker; };
    for (unsigned direction = 0; direction < 2; ++direction) {
        try {
            if (direction != 0) (void)owner.with_write(throws);
            else (void)owner.with_read(throws);
        } catch (BodyError *error) {
            CHECK(error == &marker);
            ++caught;
        }
        mgmem_snapshot_v1 state{};
        CHECK(domain.snapshot(state) == MGMEM_OK && state.active_leases == 0 && state.active_allocations == 1);
    }
    CHECK(caught == 2);
    struct MoveOnlyRead {
        unsigned *calls;
        explicit MoveOnlyRead(unsigned &value) : calls(&value) {}
        MoveOnlyRead(const MoveOnlyRead &) = delete;
        MoveOnlyRead(MoveOnlyRead &&) = default;
        void operator()(const void *data, uint64_t bytes) && {
            CHECK(data && bytes == 128);
            ++*calls;
        }
    };
    CHECK(owner.with_read(MoveOnlyRead{calls}) == MGMEM_OK && calls == 1);
    CHECK(domain.begin_close() == MGMEM_OK);
    CHECK(owner.with_read(rejected_read) == MGMEM_IS_CLOSED && calls == 1);
    CHECK(owner.with_write(rejected_write) == MGMEM_IS_CLOSED && calls == 1);
}

void test_scoped_borrow_keeps_domain_alive(uint32_t mode) {
    for (unsigned action = 0; action < 3; ++action) {
        Provider provider;
        auto table = provider.table();
        mgmem::domain domain;
        CHECK((mode == MGMEM_SYSTEM ? mgmem::domain::create_system(128, domain, &table)
            : mgmem::domain::create_arena(128, 128, domain, &table)) == MGMEM_OK);
        mgmem::owned_allocation owner, moved;
        CHECK(domain.allocate(128, owner) == MGMEM_OK);
        CHECK(owner.with_write([](void *data, uint64_t bytes) {
            CHECK(bytes == 128);
            std::memset(data, 0x5e, static_cast<size_t>(bytes));
        }) == MGMEM_OK);
        if (action == 0) {
            CHECK(owner.with_read([&](const void *data, uint64_t bytes) {
                moved = std::move(owner);
                domain = mgmem::domain{};
                CHECK(!owner && !domain && moved && provider.released == 0 && provider.deallocated == 0);
                for (uint64_t i = 0; i < bytes; ++i)
                    CHECK(static_cast<const unsigned char *>(data)[i] == 0x5e);
            }) == MGMEM_OK);
            CHECK(provider.released == 0 && provider.deallocated == 0);
            CHECK(moved.release() == MGMEM_OK);
        } else {
            CHECK(owner.with_write([&](void *data, uint64_t bytes) -> mgmem_status {
                if (action == 1) CHECK(owner.release() == MGMEM_OK);
                else owner = mgmem::owned_allocation{};
                CHECK(!owner && provider.deallocated == 0);
                mgmem_snapshot_v1 state{};
                CHECK(domain.snapshot(state) == MGMEM_OK && state.live_bytes == 0 && state.retiring_bytes == 128);
                CHECK(state.active_allocations == 1 && state.active_leases == 1);
                domain = mgmem::domain{};
                CHECK(provider.released == 0 && provider.deallocated == 0);
                std::memset(data, 0xa1, static_cast<size_t>(bytes));
                return MGMEM_BUSY;
            }) == MGMEM_BUSY);
        }
        CHECK(provider.retained == 1 && provider.released == 1 && provider.allocated == 1 && provider.deallocated == 1);
        CHECK(provider.address == nullptr);
    }
}
} // namespace

static_assert(!std::is_copy_constructible<mgmem::domain>::value, "domain must be move-only");
static_assert(!std::is_copy_constructible<mgmem::owned_allocation>::value, "owner must be move-only");
static_assert(!std::is_copy_constructible<mgmem::borrow>::value, "borrow must be move-only");
static_assert(std::is_nothrow_move_constructible<mgmem::domain>::value, "domain move is noexcept");
static_assert(std::is_nothrow_move_constructible<mgmem::owned_allocation>::value, "owner move is noexcept");
static_assert(std::is_nothrow_move_constructible<mgmem::borrow>::value, "borrow move is noexcept");
static_assert(std::is_same<decltype(std::declval<const mgmem::borrow &>().data()), const void *>::value,
              "read access must be const");

int main() {
    test_borrow_outlives_wrappers(MGMEM_SYSTEM);
    test_borrow_outlives_wrappers(MGMEM_ARENA);
    test_allocation_outlives_domain();
    test_budget_failure_preserves_old_data();
    test_empty_and_invalid_wrappers();
    test_scoped_allocation(MGMEM_SYSTEM);
    test_scoped_allocation(MGMEM_ARENA);
    test_scoped_allocation_keeps_domain_alive();
    test_convenient_creators(MGMEM_SYSTEM);
    test_convenient_creators(MGMEM_ARENA);
    test_scoped_borrow(MGMEM_SYSTEM);
    test_scoped_borrow(MGMEM_ARENA);
    test_scoped_borrow_keeps_domain_alive(MGMEM_SYSTEM);
    test_scoped_borrow_keeps_domain_alive(MGMEM_ARENA);
    std::puts("{\"suite\":\"mgmem-cpp-raii\",\"cases\":14,\"status\":\"PASS\"}");
    return 0;
}
