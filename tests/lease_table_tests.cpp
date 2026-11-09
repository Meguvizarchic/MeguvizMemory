#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace lease_table_test_heap {
std::atomic<bool> fail_next_array{false};
std::atomic<size_t> array_attempts{0};
std::atomic<size_t> array_allocations{0};
std::atomic<size_t> array_deallocations{0};
std::atomic<size_t> live_arrays{0};
std::atomic<size_t> failures{0};
std::atomic<size_t> last_array_bytes{0};
}

// This executable owns its one-shot array-allocation failure seam. Scalar
// allocations made by the independent std::map oracle remain unaffected.
void *operator new[](std::size_t bytes) {
    using namespace lease_table_test_heap;
    array_attempts.fetch_add(1, std::memory_order_relaxed);
    last_array_bytes.store(bytes, std::memory_order_relaxed);
    if (fail_next_array.exchange(false, std::memory_order_relaxed)) {
        failures.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    void *result = ::operator new(bytes);
    array_allocations.fetch_add(1, std::memory_order_relaxed);
    live_arrays.fetch_add(1, std::memory_order_relaxed);
    return result;
}

void operator delete[](void *address) noexcept {
    if (!address) return;
    ::operator delete(address);
    lease_table_test_heap::array_deallocations.fetch_add(1, std::memory_order_relaxed);
    lease_table_test_heap::live_arrays.fetch_sub(1, std::memory_order_relaxed);
}

void operator delete[](void *address, std::size_t) noexcept {
    ::operator delete[](address);
}

// Test the actual private container in a single translation unit. Do not link
// this target to mgmem: the C API definitions below belong only to this test.
#include "../src/mgmem.cpp"

namespace {
unsigned long long table_checks = 0;

[[noreturn]] void table_failure(const char *expression, int line) {
    std::fprintf(stderr, "FAIL lease_table_tests.cpp:%d: %s\n", line, expression);
    std::abort();
}

#define TABLE_CHECK(expression) do { \
    ++table_checks; \
    if (!(expression)) table_failure(#expression, __LINE__); \
} while (false)

using Oracle = std::map<uint64_t, Lease>;

Lease lease_value(uint64_t id) {
    return Lease{id ^ 0xd1b54a32d192ed03ull, (id & 1) != 0};
}

void check_table(LeaseTable &table, const Oracle &expected) {
    TABLE_CHECK(table.size() == expected.size());
    TABLE_CHECK(table.empty() == expected.empty());
    TABLE_CHECK(table.find(0) == nullptr);
    for (const auto &entry : expected) {
        auto *actual = table.find(entry.first);
        TABLE_CHECK(actual != nullptr);
        TABLE_CHECK(actual->id == entry.first);
        TABLE_CHECK(actual->value.allocation == entry.second.allocation);
        TABLE_CHECK(actual->value.writer == entry.second.writer);
    }
}

void insert_value(LeaseTable &table, Oracle &expected, uint64_t id) {
    const auto value = lease_value(id);
    TABLE_CHECK(id != 0 && expected.find(id) == expected.end());
    table.insert_fresh(id, value);
    expected.emplace(id, value);
    check_table(table, expected);
}

void erase_value(LeaseTable &table, Oracle &expected, uint64_t id, bool found_path) {
    TABLE_CHECK(expected.find(id) != expected.end());
    if (found_path) {
        auto *slot = table.find(id);
        TABLE_CHECK(slot != nullptr);
        table.erase_found(slot);
    } else {
        table.erase(id);
    }
    expected.erase(id);
    TABLE_CHECK(table.find(id) == nullptr);
    check_table(table, expected);
}

size_t reference_home(uint64_t id, size_t mask) {
    // Select controlled probe homes only. The correctness oracle is std::map;
    // neither deletion nor insertion is reproduced in the test.
    id = (id ^ (id >> 30)) * 0xbf58476d1ce4e5b9ull;
    id = (id ^ (id >> 27)) * 0x94d049bb133111ebull;
    return static_cast<size_t>(id ^ (id >> 31)) & mask;
}

uint64_t next_home(uint64_t &cursor, size_t home) {
    do { ++cursor; } while (reference_home(cursor, 7) != home);
    return cursor;
}

void test_inline_churn_and_empty_release() {
    using namespace lease_table_test_heap;
    const auto original_live = live_arrays.load();
    const auto original_attempts = array_attempts.load();
    LeaseTable table;
    uint64_t id = 1;
    for (unsigned iteration = 0; iteration < 16384; ++iteration) {
        id += 0x100000001ull;
        const auto value = lease_value(id);
        table.insert_fresh(id, value);
        TABLE_CHECK(table.size() == 1 && !table.empty());
        auto *slot = table.find(id);
        TABLE_CHECK(slot != nullptr && slot->id == id);
        TABLE_CHECK(slot->value.allocation == value.allocation && slot->value.writer == value.writer);
        table.erase_found(slot);
        TABLE_CHECK(table.empty() && table.size() == 0 && table.find(id) == nullptr);
    }
    TABLE_CHECK(array_attempts.load() == original_attempts);
    TABLE_CHECK(live_arrays.load() == original_live);

    Oracle expected;
    std::vector<uint64_t> batch;
    for (unsigned index = 0; index < 33; ++index) {
        id += 131;
        batch.push_back(id);
        insert_value(table, expected, id);
    }
    TABLE_CHECK(live_arrays.load() == original_live + 1);
    for (unsigned index = 0; index < 33; ++index)
        erase_value(table, expected, batch[(index * 17) % 33], index % 2 != 0);
    TABLE_CHECK(live_arrays.load() == original_live);

    const auto after_batch_attempts = array_attempts.load();
    ++id;
    table.insert_fresh(id, lease_value(id));
    TABLE_CHECK(table.find(id) != nullptr);
    TABLE_CHECK(array_attempts.load() == after_batch_attempts);
    table.erase(id);
    table.release_storage();
    TABLE_CHECK(table.empty() && live_arrays.load() == original_live);
}

void test_scan_past_at_home_resident() {
    LeaseTable table;
    Oracle expected;
    uint64_t cursor = 0;
    const auto a = next_home(cursor, 0);
    const auto b = next_home(cursor, 0);
    const auto c = next_home(cursor, 2);
    const auto d = next_home(cursor, 0);
    insert_value(table, expected, a);
    insert_value(table, expected, b);
    insert_value(table, expected, c);
    insert_value(table, expected, d); // Four live records require capacity 8.
    // The cluster is A0/B1/C2/D3, with C at home. Deleting B must keep scanning
    // past C and make D reachable through the hole at 1.
    erase_value(table, expected, b, true);
    TABLE_CHECK(table.find(d) != nullptr);
    erase_value(table, expected, c, false);
    erase_value(table, expected, a, true);
    erase_value(table, expected, d, false);
}

void test_wraparound_cluster() {
    LeaseTable table;
    Oracle expected;
    uint64_t cursor = 0;
    const auto a = next_home(cursor, 0);
    const auto b = next_home(cursor, 1);
    const auto c = next_home(cursor, 2);
    const auto anchor = next_home(cursor, 3);
    for (const auto id : {a, b, c, anchor}) insert_value(table, expected, id);
    erase_value(table, expected, a, false);
    erase_value(table, expected, b, true);
    erase_value(table, expected, c, false); // Keep the array live with anchor.
    const auto x = next_home(cursor, 6);
    const auto y = next_home(cursor, 6);
    const auto z = next_home(cursor, 6);
    for (const auto id : {x, y, z}) insert_value(table, expected, id);
    // The 6-home cluster occupies 6/7/0 and deletion crosses the mask boundary.
    erase_value(table, expected, x, true);
    TABLE_CHECK(table.find(y) != nullptr && table.find(z) != nullptr);
    erase_value(table, expected, z, false);
    erase_value(table, expected, anchor, true);
    erase_value(table, expected, y, false);
}

void expect_array_failure(LeaseTable &table, const Oracle &expected, uint64_t id) {
    using namespace lease_table_test_heap;
    const auto live_before = live_arrays.load();
    const auto size_before = table.size();
    bool threw = false;
    fail_next_array.store(true);
    try { table.insert_fresh(id, lease_value(id)); }
    catch (const std::bad_alloc &) { threw = true; }
    TABLE_CHECK(threw && !fail_next_array.load());
    TABLE_CHECK(table.size() == size_before && table.find(id) == nullptr);
    TABLE_CHECK(live_arrays.load() == live_before);
    check_table(table, expected);
}

void test_growth_failure_atomicity() {
    LeaseTable table;
    Oracle expected;
    insert_value(table, expected, 1);
    expect_array_failure(table, expected, 2); // Inline to four-slot array.
    insert_value(table, expected, 3);        // Failed ID need not be recycled.
    insert_value(table, expected, 4);
    expect_array_failure(table, expected, 5); // Three-quarter-full four slots.
    insert_value(table, expected, 6);
    TABLE_CHECK(table.find(2) == nullptr && table.find(5) == nullptr);
    for (const auto id : {4ull, 1ull, 6ull, 3ull}) erase_value(table, expected, id, (id & 1) != 0);
}

uint64_t random_word(uint64_t &state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545f4914f6cdd1dull;
}

void test_seeded_oracle_and_large_id_gaps() {
    LeaseTable table;
    Oracle expected;
    std::vector<uint64_t> stale;
    uint64_t state = 0x6a09e667f3bcc909ull;
    uint64_t next = 0x100000000ull;
    const auto original_live = lease_table_test_heap::live_arrays.load();
    for (unsigned step = 0; step < 20000; ++step) {
        const auto choice = random_word(state);
        if (expected.empty() || (expected.size() < 128 && choice % 100 < 57)) {
            next += 1 + (random_word(state) & 0xfffff);
            insert_value(table, expected, next);
        } else {
            auto selected = expected.begin();
            std::advance(selected, static_cast<std::ptrdiff_t>(random_word(state) % expected.size()));
            const auto id = selected->first;
            erase_value(table, expected, id, (choice & 1) != 0);
            stale.push_back(id);
        }
        if (!stale.empty()) {
            const auto id = stale[static_cast<size_t>(random_word(state) % stale.size())];
            TABLE_CHECK(table.find(id) == nullptr);
            table.erase(id);
            check_table(table, expected);
        }
        if (step % 257 == 256) {
            while (!expected.empty()) {
                const auto id = expected.begin()->first;
                erase_value(table, expected, id, (id & 1) != 0);
                stale.push_back(id);
            }
            TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
            const auto attempts = lease_table_test_heap::array_attempts.load();
            ++next;
            table.insert_fresh(next, lease_value(next));
            TABLE_CHECK(lease_table_test_heap::array_attempts.load() == attempts);
            auto *slot = table.find(next);
            TABLE_CHECK(slot != nullptr);
            table.erase_found(slot);
            TABLE_CHECK(table.empty() && lease_table_test_heap::live_arrays.load() == original_live);
        }
    }
    while (!expected.empty()) erase_value(table, expected, expected.begin()->first, true);
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
}

void test_c_api_borrow_growth_oom_contract() {
    mgmem_domain_options_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = MGMEM_ABI_MAJOR;
    config.abi_minor = MGMEM_ABI_MINOR;
    config.mode = MGMEM_ARENA;
    config.budget_bytes = 256;
    config.arena_chunk_bytes = 256;
    mgmem_domain *domain = nullptr;
    TABLE_CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_OK && domain != nullptr);
    mgmem_handle_v1 owner{};
    TABLE_CHECK(mgmem_allocate_v1(domain, 64, 16, &owner) == MGMEM_OK);
    mgmem_borrow_v1 initial_writer{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &initial_writer) == MGMEM_OK);
    for (unsigned index = 0; index < 64; ++index)
        static_cast<unsigned char *>(initial_writer.data)[index] = static_cast<unsigned char>(0x4d + index);
    TABLE_CHECK(mgmem_borrow_release_v1(domain, initial_writer.lease) == MGMEM_OK);
    mgmem_borrow_v1 reader{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_READ, &reader) == MGMEM_OK);

    mgmem_borrow_v1 rejected{{1, 1, 1}, reinterpret_cast<void *>(uintptr_t{1}), 1};
    const auto live_before = lease_table_test_heap::live_arrays.load();
    const auto failures_before = lease_table_test_heap::failures.load();
    lease_table_test_heap::fail_next_array.store(true);
    const auto failed_status = mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_READ, &rejected);
    TABLE_CHECK(failed_status == MGMEM_OUT_OF_MEMORY);
    TABLE_CHECK(!lease_table_test_heap::fail_next_array.load());
    TABLE_CHECK(lease_table_test_heap::failures.load() == failures_before + 1);
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == live_before);
    TABLE_CHECK(rejected.data == nullptr && rejected.length == 0 && rejected.lease.domain_id == 0 &&
                rejected.lease.id == 0 && rejected.lease.runtime_origin == 0);
    mgmem_snapshot_v1 state{};
    TABLE_CHECK(mgmem_domain_snapshot_v1(domain, &state, sizeof(state)) == MGMEM_OK);
    TABLE_CHECK(state.active_leases == 1 && state.active_allocations == 1 &&
                state.live_bytes == 64 && state.retiring_bytes == 0 && state.reserved_bytes == 0);
    for (unsigned index = 0; index < 64; ++index)
        TABLE_CHECK(static_cast<unsigned char *>(reader.data)[index] == static_cast<unsigned char>(0x4d + index));
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &rejected) == MGMEM_BUSY);
    TABLE_CHECK(rejected.data == nullptr && rejected.lease.id == 0);

    TABLE_CHECK(mgmem_borrow_release_v1(domain, reader.lease) == MGMEM_OK);
    mgmem_borrow_v1 writer{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &writer) == MGMEM_OK);
    TABLE_CHECK(writer.lease.id > reader.lease.id);
    TABLE_CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    TABLE_CHECK(mgmem_borrow_release_v1(domain, writer.lease) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_snapshot_v1(domain, &state, sizeof(state)) == MGMEM_OK);
    TABLE_CHECK(state.state == MGMEM_CLOSED && state.active_leases == 0 && state.active_allocations == 0 &&
                state.live_bytes == 0 && state.retiring_bytes == 0 && state.backing_bytes == 0 &&
                state.reserved_bytes == 0);
    TABLE_CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == live_before);
}

void check_owner_table(AllocationTable &table, const std::map<uint64_t, Allocation> &expected) {
    TABLE_CHECK(table.size() == expected.size() && table.empty() == expected.empty());
    TABLE_CHECK(table.find(0) == nullptr);
    for (const auto &entry : expected) {
        auto *actual = table.find(entry.first);
        TABLE_CHECK(actual != nullptr && actual->id == entry.first);
        const auto &a = actual->value;
        const auto &b = entry.second;
        TABLE_CHECK(a.backing == b.backing && a.offset == b.offset && a.bytes == b.bytes);
        TABLE_CHECK(a.readers == b.readers && a.writer == b.writer &&
                    a.owner == b.owner && a.pending == b.pending);
    }
}

void test_owner_table_record_relocation_oracle() {
    std::array<Backing, 8> origins{};
    AllocationTable table;
    std::map<uint64_t, Allocation> expected;
    std::vector<uint64_t> ids;
    const auto original_live = lease_table_test_heap::live_arrays.load();
    uint64_t next = 0x1000000000000ull;
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
        next += 0x100000001ull + iteration;
        Allocation value{&origins[iteration % origins.size()], iteration * 17ull,
                         1 + iteration * 31ull, iteration % 11, (iteration & 1) != 0,
                         (iteration & 2) != 0, (iteration & 4) != 0};
        auto &inserted = table.insert_fresh(next, value);
        expected.emplace(next, value);
        if (iteration % 5 == 0) {
            // The returned reference is consumed before the next possible grow.
            inserted.pending = !inserted.pending;
            inserted.owner = !inserted.pending;
            expected.at(next).pending = inserted.pending;
            expected.at(next).owner = inserted.owner;
        }
        ids.push_back(next);
        check_owner_table(table, expected);
    }
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live + 1);
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
        const auto id = ids[(iteration * 73) % ids.size()];
        if (iteration & 1) {
            auto *slot = table.find(id);
            TABLE_CHECK(slot != nullptr);
            table.erase_found(slot);
        } else table.erase(id);
        expected.erase(id);
        TABLE_CHECK(table.find(id) == nullptr);
        check_owner_table(table, expected);
    }
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
    const auto attempts = lease_table_test_heap::array_attempts.load();
    const Allocation final_value{&origins[0], 7, 19, 2, false, true, false};
    ++next;
    table.insert_fresh(next, final_value);
    expected.emplace(next, final_value);
    check_owner_table(table, expected);
    TABLE_CHECK(lease_table_test_heap::array_attempts.load() == attempts);
    table.erase(next);
    expected.clear();
    check_owner_table(table, expected);
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
}

void test_c_api_owner_growth_oom_keeps_arena_cursor() {
    mgmem_domain_options_v1 config{};
    config.struct_size = sizeof(config);
    config.abi_major = MGMEM_ABI_MAJOR;
    config.abi_minor = MGMEM_ABI_MINOR;
    config.mode = MGMEM_ARENA;
    config.budget_bytes = 256;
    config.arena_chunk_bytes = 256;
    mgmem_domain *domain = nullptr;
    TABLE_CHECK(mgmem_domain_create_v1(&config, &domain) == MGMEM_OK && domain != nullptr);
    mgmem_handle_v1 first{};
    TABLE_CHECK(mgmem_allocate_v1(domain, 64, 16, &first) == MGMEM_OK);
    mgmem_borrow_v1 initial{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, first, 0, 64, MGMEM_WRITE, &initial) == MGMEM_OK);
    for (unsigned index = 0; index < 64; ++index)
        static_cast<unsigned char *>(initial.data)[index] = static_cast<unsigned char>(0x31 + index);
    TABLE_CHECK(mgmem_borrow_release_v1(domain, initial.lease) == MGMEM_OK);
    mgmem_borrow_v1 reader{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, first, 0, 64, MGMEM_READ, &reader) == MGMEM_OK);

    mgmem_handle_v1 failed{1, 1, 1};
    const auto original_live = lease_table_test_heap::live_arrays.load();
    const auto original_failures = lease_table_test_heap::failures.load();
    lease_table_test_heap::fail_next_array.store(true);
    TABLE_CHECK(mgmem_allocate_v1(domain, 64, 16, &failed) == MGMEM_OUT_OF_MEMORY);
    TABLE_CHECK(!lease_table_test_heap::fail_next_array.load());
    TABLE_CHECK(lease_table_test_heap::failures.load() == original_failures + 1);
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
    TABLE_CHECK(failed.domain_id == 0 && failed.id == 0 && failed.runtime_origin == 0);
    mgmem_snapshot_v1 state{};
    TABLE_CHECK(mgmem_domain_snapshot_v1(domain, &state, sizeof(state)) == MGMEM_OK);
    TABLE_CHECK(state.active_allocations == 1 && state.active_leases == 1 && state.allocation_count == 1 &&
                state.provider_allocation_count == 1 && state.cumulative_requested_bytes == 64 &&
                state.live_bytes == 64 && state.retiring_bytes == 0 && state.reserved_bytes == 0);
    for (unsigned index = 0; index < 64; ++index)
        TABLE_CHECK(static_cast<unsigned char *>(reader.data)[index] == static_cast<unsigned char>(0x31 + index));
    mgmem_borrow_v1 rejected{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, first, 0, 1, MGMEM_WRITE, &rejected) == MGMEM_BUSY);

    mgmem_handle_v1 second{};
    TABLE_CHECK(mgmem_allocate_v1(domain, 64, 16, &second) == MGMEM_OK);
    mgmem_borrow_v1 writer{};
    TABLE_CHECK(mgmem_borrow_acquire_v1(domain, second, 0, 64, MGMEM_WRITE, &writer) == MGMEM_OK);
    // The failed metadata insertion must not consume payload cursor space.
    TABLE_CHECK(static_cast<unsigned char *>(writer.data) == static_cast<unsigned char *>(reader.data) + 64);
    for (unsigned index = 0; index < 64; ++index) {
        static_cast<unsigned char *>(writer.data)[index] = static_cast<unsigned char>(0x89 + index);
        TABLE_CHECK(static_cast<unsigned char *>(reader.data)[index] == static_cast<unsigned char>(0x31 + index));
    }
    TABLE_CHECK(mgmem_borrow_release_v1(domain, reader.lease) == MGMEM_OK);
    TABLE_CHECK(mgmem_release_v1(domain, first) == MGMEM_OK);
    TABLE_CHECK(mgmem_release_v1(domain, second) == MGMEM_OK);
    for (unsigned index = 0; index < 64; ++index)
        TABLE_CHECK(static_cast<unsigned char *>(writer.data)[index] == static_cast<unsigned char>(0x89 + index));
    TABLE_CHECK(mgmem_borrow_release_v1(domain, writer.lease) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    TABLE_CHECK(mgmem_domain_snapshot_v1(domain, &state, sizeof(state)) == MGMEM_OK);
    TABLE_CHECK(state.state == MGMEM_CLOSED && state.active_allocations == 0 && state.active_leases == 0 &&
                state.live_bytes == 0 && state.retiring_bytes == 0 && state.backing_bytes == 0 && state.reserved_bytes == 0);
    TABLE_CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
}

void check_prediction_record(const Lease &actual, const Lease &expected) {
    TABLE_CHECK(actual.allocation == expected.allocation && actual.writer == expected.writer);
}

void check_prediction_record(const Allocation &actual, const Allocation &expected) {
    TABLE_CHECK(actual.backing == expected.backing && actual.offset == expected.offset &&
                actual.bytes == expected.bytes && actual.readers == expected.readers &&
                actual.writer == expected.writer && actual.owner == expected.owner &&
                actual.pending == expected.pending);
}

Allocation prediction_owner_value(uint64_t id) {
    static Backing origin{};
    return Allocation{&origin, id * 17, id + 31, id % 11,
                      (id & 1) != 0, (id & 2) != 0, (id & 4) != 0};
}

template<class Table, class Record>
void check_capacity_prediction(Record (*make_value)(uint64_t)) {
    using namespace lease_table_test_heap;
    Table table;
    const auto original_live = live_arrays.load();
    const auto slot_bytes = sizeof(*table.find(1));
    const auto before_warm = array_attempts.load();
    for (uint64_t index = 0; index < 64; ++index)
        table.insert_fresh(100 + index, make_value(100 + index));
    TABLE_CHECK(array_attempts.load() > before_warm + 1);
    TABLE_CHECK(last_array_bytes.load() == slot_bytes * 128);
    for (uint64_t index = 0; index < 64; ++index) {
        const auto id = 100 + (index * 17) % 64;
        auto *slot = table.find(id);
        TABLE_CHECK(slot != nullptr);
        check_prediction_record(slot->value, make_value(id));
        table.erase_found(slot);
    }
    TABLE_CHECK(table.empty() && live_arrays.load() == original_live);

    const auto before_inline = array_attempts.load();
    table.insert_fresh(1000, make_value(1000));
    TABLE_CHECK(table.size() == 1 && array_attempts.load() == before_inline);
    TABLE_CHECK(live_arrays.load() == original_live);
    const auto before_failure = failures.load();
    fail_next_array.store(true);
    bool threw = false;
    try { table.insert_fresh(1001, make_value(1001)); }
    catch (const std::bad_alloc &) { threw = true; }
    TABLE_CHECK(threw && !fail_next_array.load() && failures.load() == before_failure + 1);
    TABLE_CHECK(table.size() == 1 && table.find(1001) == nullptr);
    TABLE_CHECK(live_arrays.load() == original_live);
    auto *inline_slot = table.find(1000);
    TABLE_CHECK(inline_slot != nullptr);
    check_prediction_record(inline_slot->value, make_value(1000));

    const auto before_predicted = array_attempts.load();
    for (uint64_t index = 1; index < 64; ++index)
        table.insert_fresh(1001 + index, make_value(1001 + index));
    TABLE_CHECK(table.size() == 64 && array_attempts.load() == before_predicted + 1);
    TABLE_CHECK(last_array_bytes.load() == slot_bytes * 128);
    TABLE_CHECK(live_arrays.load() == original_live + 1);
    table.erase(1000);
    for (uint64_t index = 1; index < 64; ++index) {
        const auto id = 1001 + index;
        auto *slot = table.find(id);
        TABLE_CHECK(slot != nullptr);
        check_prediction_record(slot->value, make_value(id));
        table.erase_found(slot);
    }
    TABLE_CHECK(table.empty() && live_arrays.load() == original_live);

    table.release_storage();
    const auto before_reset_growth = array_attempts.load();
    table.insert_fresh(2000, make_value(2000));
    table.insert_fresh(2001, make_value(2001));
    TABLE_CHECK(array_attempts.load() == before_reset_growth + 1);
    TABLE_CHECK(last_array_bytes.load() == slot_bytes * 4);
    table.erase(2001);
    table.erase(2000);
    TABLE_CHECK(table.empty() && live_arrays.load() == original_live);

    // A larger historical phase must not dictate an unbounded later request.
    for (uint64_t index = 0; index < 200; ++index)
        table.insert_fresh(3000 + index, make_value(3000 + index));
    TABLE_CHECK(last_array_bytes.load() > slot_bytes * 128);
    for (uint64_t index = 0; index < 200; ++index) table.erase(3000 + index);
    TABLE_CHECK(table.empty() && live_arrays.load() == original_live);
    const auto before_bounded = array_attempts.load();
    table.insert_fresh(4000, make_value(4000));
    TABLE_CHECK(array_attempts.load() == before_bounded);
    table.insert_fresh(4001, make_value(4001));
    TABLE_CHECK(array_attempts.load() == before_bounded + 1);
    TABLE_CHECK(last_array_bytes.load() == slot_bytes * 128);
    auto *first_bounded = table.find(4000);
    auto *second_bounded = table.find(4001);
    TABLE_CHECK(first_bounded != nullptr && second_bounded != nullptr);
    check_prediction_record(first_bounded->value, make_value(4000));
    check_prediction_record(second_bounded->value, make_value(4001));
    table.erase(4000);
    table.erase(4001);
    TABLE_CHECK(table.empty() && live_arrays.load() == original_live);
}

void test_lease_capacity_prediction_lifecycle() {
    check_capacity_prediction<LeaseTable>(lease_value);
}

void test_owner_capacity_prediction_lifecycle() {
    check_capacity_prediction<AllocationTable>(prediction_owner_value);
}

void check_backing_list(BackingList &list, const std::vector<Backing *> &expected) {
    TABLE_CHECK(list.empty() == expected.empty());
    TABLE_CHECK((list.begin() == list.end()) == expected.empty());
    auto actual = list.begin();
    for (size_t index = 0; index < expected.size(); ++index) {
        TABLE_CHECK(actual != list.end());
        TABLE_CHECK(&*actual == expected[index]);
        TABLE_CHECK(actual->previous == (index ? expected[index - 1] : nullptr));
        TABLE_CHECK(actual->next == (index + 1 < expected.size() ? expected[index + 1] : nullptr));
        TABLE_CHECK(actual->bytes == 1000 + actual->used);
        ++actual;
    }
    TABLE_CHECK(actual == list.end());
    TABLE_CHECK(list.last().node == (expected.empty() ? nullptr : expected.back()));
    if (!expected.empty()) TABLE_CHECK(&list.front() == expected.front());
}

void test_backing_list_identity_splice_and_erase() {
    TABLE_CHECK(sizeof(BackingList) == 2 * sizeof(Backing *));
    TABLE_CHECK(sizeof(BackingList::iterator) == sizeof(Backing *));
    BackingList left, right;
    std::vector<Backing *> first, second;
    check_backing_list(left, first);
    check_backing_list(right, second);
    left.splice_all(right);
    left.splice_all(left);
    check_backing_list(left, first);
    for (uint64_t index = 0; index < 8; ++index) {
        left.emplace_back(Backing{nullptr, 1000 + index, 16, index});
        first.push_back(left.last().node);
        check_backing_list(left, first);
    }
    const auto stable = left.begin();
    const std::array<size_t, 4> transfer{{3, 0, 7, 4}};
    for (const auto value : transfer) {
        const auto selected = std::find_if(first.begin(), first.end(), [&](Backing *node) {
            return node->used == value;
        });
        TABLE_CHECK(selected != first.end());
        Backing *node = *selected;
        right.splice_one(left, node);
        first.erase(selected);
        second.push_back(node);
        check_backing_list(left, first);
        check_backing_list(right, second);
        TABLE_CHECK(stable->used == 0 && stable->bytes == 1000);
    }
    right.erase(right.begin());
    second.erase(second.begin());
    right.erase(right.last());
    second.pop_back();
    check_backing_list(right, second);
    left.splice_all(right);
    first.insert(first.end(), second.begin(), second.end());
    second.clear();
    check_backing_list(left, first);
    check_backing_list(right, second);
    // The original first node kept its identity through detachment and rejoin.
    TABLE_CHECK(stable->used == 0 && stable->bytes == 1000);
    while (!first.empty()) {
        const auto index = first.size() / 2;
        left.erase(BackingList::iterator{first[index]});
        first.erase(first.begin() + static_cast<std::ptrdiff_t>(index));
        check_backing_list(left, first);
    }
    // Exercise empty-target whole-list transfer, a one-node unlink, then leave
    // owned nodes for the container destructor and sanitizer leak checks.
    right.emplace_back(Backing{nullptr, 1009, 16, 9});
    second.push_back(right.last().node);
    left.splice_all(right);
    first = second;
    second.clear();
    check_backing_list(left, first);
    check_backing_list(right, second);
    right.splice_one(left, first.front());
    second = first;
    first.clear();
    check_backing_list(left, first);
    check_backing_list(right, second);
    left.emplace_back(Backing{nullptr, 1010, 16, 10});
    first.push_back(left.last().node);
    check_backing_list(left, first);
}
}

int main() {
    const auto original_live = lease_table_test_heap::live_arrays.load();
    const auto original_failures = lease_table_test_heap::failures.load();
    const struct Test { const char *name; void (*run)(); } tests[] = {
        {"inline_churn_and_empty_release", test_inline_churn_and_empty_release},
        {"scan_past_at_home_resident", test_scan_past_at_home_resident},
        {"wraparound_cluster", test_wraparound_cluster},
        {"growth_failure_atomicity", test_growth_failure_atomicity},
        {"seeded_oracle_and_large_id_gaps", test_seeded_oracle_and_large_id_gaps},
        {"c_api_borrow_growth_oom_contract", test_c_api_borrow_growth_oom_contract},
        {"owner_table_record_relocation_oracle", test_owner_table_record_relocation_oracle},
        {"c_api_owner_growth_oom_keeps_arena_cursor", test_c_api_owner_growth_oom_keeps_arena_cursor},
        {"lease_capacity_prediction_lifecycle", test_lease_capacity_prediction_lifecycle},
        {"owner_capacity_prediction_lifecycle", test_owner_capacity_prediction_lifecycle},
        {"backing_list_identity_splice_and_erase", test_backing_list_identity_splice_and_erase}
    };
    for (const auto &test : tests) {
        test.run();
        TABLE_CHECK(lease_table_test_heap::live_arrays.load() == original_live);
        std::printf("PASS %s\n", test.name);
    }
    const auto injected_failures = lease_table_test_heap::failures.load() - original_failures;
    TABLE_CHECK(injected_failures == 6);
    std::printf("{\"suite\":\"mgmem-lease-table\",\"tests\":%zu,\"checks\":%llu,\"injected_array_failures\":%zu,\"status\":\"PASS\"}\n",
                sizeof(tests) / sizeof(tests[0]), table_checks, injected_failures);
    return 0;
}
