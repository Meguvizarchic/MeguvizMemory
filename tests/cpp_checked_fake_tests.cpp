#include <mgmem/memory.hpp>

#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)

// A controlled fake ABI exposes a failure before handle consumption.  The
// wrapper must not assume that PROVIDER_ERROR proves consumption or retry it.
struct mgmem_domain { bool closed = false; };
namespace {
mgmem_domain *allocated = nullptr;
mgmem_status owner_result = MGMEM_OK, lease_result = MGMEM_OK;
mgmem_status import_result = MGMEM_OK, snapshot_result = MGMEM_OK;
mgmem_status allocate_result = MGMEM_OK, acquire_result = MGMEM_OK;
uint64_t pending_import_bytes = 0;
unsigned owner_calls = 0, lease_calls = 0, destroy_calls = 0;
bool owner_live = false, lease_live = false;
unsigned char data[8]{};
}

extern "C" {
uint64_t mgmem_capabilities_v1() { return MGMEM_CAP_CHECKED_EXTERNAL_RELEASE; }
mgmem_status mgmem_domain_create_v1(const mgmem_domain_options_v1 *, mgmem_domain **out) {
    allocated = new mgmem_domain{}; *out = allocated; return MGMEM_OK;
}
mgmem_status mgmem_allocate_v1(mgmem_domain *, uint64_t, uint64_t, mgmem_handle_v1 *out) {
    *out = {};
    if (allocate_result != MGMEM_OK) return allocate_result;
    owner_live = true; *out = {1, 1, 1}; return MGMEM_OK;
}
mgmem_status mgmem_import_external_checked_v1(mgmem_domain *, const mgmem_external_buffer_checked_v1 *, mgmem_handle_v1 *out) {
    if (import_result != MGMEM_OK) { *out = {}; return import_result; }
    owner_live = true; *out = {1, 1, 1}; return MGMEM_OK;
}
mgmem_status mgmem_domain_import_snapshot_v1(mgmem_domain *, mgmem_import_snapshot_v1 *out, uint32_t) {
    if (snapshot_result != MGMEM_OK) return snapshot_result;
    *out = {}; out->pending_import_bytes = pending_import_bytes; return MGMEM_OK;
}
mgmem_status mgmem_release_v1(mgmem_domain *, mgmem_handle_v1) {
    ++owner_calls;
    if (owner_result == MGMEM_OK) owner_live = false;
    return owner_result;
}
mgmem_status mgmem_borrow_acquire_v1(mgmem_domain *, mgmem_handle_v1, uint64_t, uint64_t,
                                      uint32_t, mgmem_borrow_v1 *out) {
    *out = {};
    if (acquire_result != MGMEM_OK) return acquire_result;
    lease_live = true; *out = {{1, 2, 1}, data, sizeof(data)}; return MGMEM_OK;
}
mgmem_status mgmem_borrow_release_v1(mgmem_domain *, mgmem_handle_v1) {
    ++lease_calls;
    if (lease_result == MGMEM_OK) lease_live = false;
    return lease_result;
}
mgmem_status mgmem_domain_begin_close_v1(mgmem_domain *domain) { domain->closed = true; return MGMEM_OK; }
mgmem_status mgmem_domain_collect_v1(mgmem_domain *) { return MGMEM_OK; }
mgmem_status mgmem_domain_drain_v1(mgmem_domain *, uint64_t) { return MGMEM_OK; }
mgmem_status mgmem_domain_destroy_v1(mgmem_domain *domain) {
    ++destroy_calls; delete domain; allocated = nullptr; return MGMEM_OK;
}
}

int main() {
    mgmem_domain_options_v1 options{};
    mgmem_external_buffer_checked_v1 input{};
    input.capacity_bytes = sizeof(data);

    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options, domain) == MGMEM_OK);
        mgmem::owned_allocation owner;
        CHECK(domain.import_external_checked(input, owner) == MGMEM_OK);
        owner_result = MGMEM_BUSY;
        CHECK(owner.release() == MGMEM_BUSY && owner && !domain.quarantined());
        owner_result = MGMEM_OK;
        CHECK(owner.release() == MGMEM_OK && !owner && owner_calls == 2);
    }
    CHECK(destroy_calls == 1 && allocated == nullptr);

    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options, domain) == MGMEM_OK);
        mgmem::owned_allocation owner;
        CHECK(domain.import_external_checked(input, owner) == MGMEM_OK);
        owner_result = MGMEM_PROVIDER_ERROR; // fake did not consume owner
        const auto prior = owner_calls;
        CHECK(owner.release() == MGMEM_PROVIDER_ERROR && !owner && domain.quarantined());
        CHECK(owner.release() == MGMEM_OK && owner_calls == prior + 1 && owner_live);
        CHECK(domain.collect() == MGMEM_PROVIDER_ERROR);
    }
    CHECK(destroy_calls == 1 && allocated != nullptr && owner_live);
    delete allocated; allocated = nullptr; owner_live = false; owner_result = MGMEM_OK;

    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options, domain) == MGMEM_OK);
        mgmem::owned_allocation owner;
        CHECK(domain.import_external_checked(input, owner) == MGMEM_OK);
        mgmem::borrow lease;
        CHECK(owner.acquire_borrow(0, sizeof(data), MGMEM_READ, lease) == MGMEM_OK);
        CHECK(owner.release() == MGMEM_OK);
        lease_result = MGMEM_PROVIDER_ERROR; // fake did not consume lease
        CHECK(lease.release() == MGMEM_PROVIDER_ERROR && !lease && domain.quarantined());
        CHECK(lease.release() == MGMEM_OK && lease_calls == 1 && lease_live);
    }
    CHECK(destroy_calls == 1 && allocated != nullptr && lease_live);
    // Fake handles have no actual callback or foreign state; clean the fake
    // heap only after asserting that the wrapper intentionally retained it.
    delete allocated; allocated = nullptr; lease_live = false;

    for (int scenario = 0; scenario < 2; ++scenario) {
        {
            mgmem::domain domain;
            CHECK(mgmem::domain::create(options, domain) == MGMEM_OK);
            mgmem::owned_allocation owner;
            import_result = MGMEM_PROVIDER_ERROR;
            snapshot_result = scenario ? MGMEM_PROVIDER_ERROR : MGMEM_OK;
            pending_import_bytes = scenario ? 0 : 8;
            CHECK(domain.import_external_checked(input, owner) == MGMEM_PROVIDER_ERROR);
            CHECK(!owner && domain.quarantined());
        }
        CHECK(destroy_calls == 1 && allocated != nullptr);
        delete allocated; allocated = nullptr;
    }
    {
        mgmem::domain domain;
        CHECK(mgmem::domain::create(options, domain) == MGMEM_OK);
        unsigned body_calls = 0;
        auto body = [&](void *, uint64_t) { ++body_calls; };
        const auto prior_owners = owner_calls, prior_leases = lease_calls;
        allocate_result = MGMEM_OUT_OF_MEMORY;
        CHECK(domain.with_allocation(sizeof(data), body) == MGMEM_OUT_OF_MEMORY && body_calls == 0);
        CHECK(owner_calls == prior_owners && lease_calls == prior_leases && !owner_live && !lease_live);
        allocate_result = MGMEM_OK;
        acquire_result = MGMEM_OUT_OF_MEMORY;
        CHECK(domain.with_allocation(sizeof(data), body) == MGMEM_OUT_OF_MEMORY && body_calls == 0);
        CHECK(owner_calls == prior_owners + 1 && lease_calls == prior_leases && !owner_live && !lease_live);
        acquire_result = MGMEM_OK;
    }
    CHECK(destroy_calls == 2 && allocated == nullptr);
    std::puts("{\"suite\":\"cpp-checked-fake-unconsumed\",\"status\":\"PASS\"}");
    return 0;
}
