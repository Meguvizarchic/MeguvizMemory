#include <mgmem/compute_read_bridge.h>
#include <meguviz/compute.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (0)
template<class T> static T init() { T value{}; value.header = MGC_HEADER_INIT(T); return value; }
struct Errors { std::atomic<int> count{0}; };
static void on_error(void *context, int32_t) { ++static_cast<Errors *>(context)->count; }

int main(int argc, char **argv) {
    const bool violate = argc == 2 && std::strcmp(argv[1], "--violate-unmap") == 0;
    CHECK(argc == 1 || violate);
    auto runtime_desc = init<MgcRuntimeDesc>();
    runtime_desc.provider = MGC_PROVIDER_CPU_REFERENCE;
    runtime_desc.memory_budget_bytes = 4096;
    MgcRuntime *runtime = nullptr;
    CHECK(mgc_runtime_create(&runtime_desc, &runtime) == MGC_SUCCESS);
    auto domain_desc = init<MgcDomainDesc>();
    MgcDomain *compute = nullptr;
    CHECK(mgc_domain_acquire(runtime, &domain_desc, &compute) == MGC_SUCCESS);
    auto resource_desc = init<MgcResourceDesc>();
    resource_desc.allocation_id = 1;
    resource_desc.allocation_generation = 1;
    resource_desc.capacity_bytes = 64;
    resource_desc.format = MGC_FORMAT_BYTES;
    MgcResource *resource = nullptr;
    CHECK(mgc_resource_create(compute, &resource_desc, &resource) == MGC_SUCCESS);
    auto lease_desc = init<MgcLeaseDesc>();
    lease_desc.content_id = 1;
    lease_desc.byte_length = 64;
    lease_desc.access = MGC_ACCESS_READ;
    MgcLease *origin = nullptr;
    CHECK(mgc_lease_create(resource, &lease_desc, &origin) == MGC_SUCCESS);

    mgmem_domain_options_v1 options{};
    options.struct_size = sizeof(options);
    options.abi_major = MGMEM_ABI_MAJOR;
    options.abi_minor = MGMEM_ABI_MINOR;
    options.mode = MGMEM_ARENA;
    options.budget_bytes = 128;
    options.arena_chunk_bytes = 64;
    mgmem_domain *arena = nullptr;
    CHECK(mgmem_domain_create_v1(&options, &arena) == MGMEM_OK);
    Errors errors;
    mgmem_handle_v1 owner{};
    int32_t origin_status = -1;
    CHECK(mgmem_domain_set_budget_v1(arena, 63) == MGMEM_OK);
    CHECK(mgmem_compute_import_arena_read_v1(arena, origin, 1, on_error, &errors, &owner, &origin_status) == MGMEM_OUT_OF_BUDGET);
    CHECK(owner.id == 0 && origin_status == MGC_SUCCESS);
    void *mapped = nullptr;
    uint64_t length = 0;
    CHECK(mgc_lease_map(origin, &mapped, &length) == MGC_SUCCESS);
    CHECK(mgc_lease_unmap(origin) == MGC_SUCCESS);
    CHECK(mgmem_domain_set_budget_v1(arena, 128) == MGMEM_OK);
    CHECK(mgmem_compute_import_read_v1(arena, origin, 2, on_error, &errors, &owner, &origin_status) == MGMEM_UNSUPPORTED);
    CHECK(mgmem_compute_import_arena_read_v1(arena, origin, 3, on_error, &errors, &owner, &origin_status) == MGMEM_OK);
    mgmem_borrow_v1 read{};
    CHECK(mgmem_borrow_acquire_v1(arena, owner, 0, 64, MGMEM_WRITE, &read) != MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(arena, owner, 0, 64, MGMEM_READ, &read) == MGMEM_OK);
    mgmem_handle_v1 allocation{};
    CHECK(mgmem_allocate_v1(arena, 64, 1, &allocation) == MGMEM_OK);
    mgmem_snapshot_v1 snapshot{};
    CHECK(mgmem_domain_snapshot_v1(arena, &snapshot, sizeof(snapshot)) == MGMEM_OK);
    CHECK(snapshot.backing_bytes == 128);
    CHECK(mgmem_release_v1(arena, allocation) == MGMEM_OK);
    if (violate) {
        CHECK(mgc_lease_unmap(origin) == MGC_SUCCESS);
        CHECK(mgmem_release_v1(arena, owner) == MGMEM_OK);
        CHECK(mgmem_borrow_release_v1(arena, read.lease) == MGMEM_PROVIDER_ERROR);
        CHECK(errors.count.load() == 1);
        CHECK(mgmem_domain_begin_close_v1(arena) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(arena) == MGMEM_PROVIDER_ERROR);
        std::puts("PASS arena checked-release quarantine");
        std::fflush(stdout);
        std::_Exit(0);
    }
    CHECK(mgmem_release_v1(arena, owner) == MGMEM_OK);
    CHECK(mgmem_domain_begin_close_v1(arena) == MGMEM_OK);
    CHECK(mgc_lease_map(origin, &mapped, &length) == MGC_BUSY);
    std::thread last([&] { CHECK(mgmem_borrow_release_v1(arena, read.lease) == MGMEM_OK); });
    last.join();
    CHECK(mgc_lease_map(origin, &mapped, &length) == MGC_SUCCESS);
    CHECK(mgc_lease_unmap(origin) == MGC_SUCCESS);
    CHECK(mgmem_domain_collect_v1(arena) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(arena, 0) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(arena) == MGMEM_OK);
    CHECK(errors.count.load() == 0);
    mgc_lease_release(origin);
    mgc_resource_release(resource);
    mgc_domain_release(compute);
    CHECK(mgc_runtime_close(runtime) == MGC_SUCCESS);
    mgc_runtime_release(runtime);
    std::puts("PASS arena CPU READ import");
}
