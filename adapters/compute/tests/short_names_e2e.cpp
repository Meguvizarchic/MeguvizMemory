#include <mgmem/compute_read.h>
#include <meguviz/compute.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
template<class T> static T init() { T value{}; value.header = MGC_HEADER_INIT(T); return value; }
struct Errors { std::atomic<int> count{0}; };
static void on_error(void *context, int32_t) { ++static_cast<Errors *>(context)->count; }

static void close_domain(mgmem_domain *domain) {
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
}
static void verify_bytes(mgmem_domain *domain, mgmem_handle_v1 owner) {
    mgmem_borrow_v1 read{};
    CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &read) != MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_READ, &read) == MGMEM_OK);
    for (uint64_t i = 0; i < read.length; ++i) CHECK(static_cast<unsigned char *>(read.data)[i] == i);
    CHECK(mgmem_borrow_release_v1(domain, read.lease) == MGMEM_OK);
}

int main() {
    auto rd = init<MgcRuntimeDesc>(); rd.provider = MGC_PROVIDER_CPU_REFERENCE; rd.memory_budget_bytes = 4096;
    MgcRuntime *runtime = nullptr; CHECK(mgc_runtime_create(&rd, &runtime) == MGC_SUCCESS);
    auto dd = init<MgcDomainDesc>(); MgcDomain *compute = nullptr;
    CHECK(mgc_domain_acquire(runtime, &dd, &compute) == MGC_SUCCESS);
    auto resource_desc = init<MgcResourceDesc>(); resource_desc.allocation_id = 1; resource_desc.allocation_generation = 1;
    resource_desc.capacity_bytes = 64; resource_desc.format = MGC_FORMAT_BYTES;
    MgcResource *resource = nullptr; CHECK(mgc_resource_create(compute, &resource_desc, &resource) == MGC_SUCCESS);
    auto lease_desc = init<MgcLeaseDesc>(); lease_desc.content_id = 1; lease_desc.byte_length = 64; lease_desc.access = MGC_ACCESS_WRITE;
    MgcLease *writer = nullptr; CHECK(mgc_lease_create(resource, &lease_desc, &writer) == MGC_SUCCESS);
    void *pointer = nullptr; uint64_t length = 0;
    CHECK(mgc_lease_map(writer, &pointer, &length) == MGC_SUCCESS && length == 64);
    for (uint64_t i = 0; i < length; ++i) static_cast<unsigned char *>(pointer)[i] = static_cast<unsigned char>(i);
    CHECK(mgc_lease_unmap(writer) == MGC_SUCCESS);

    mgmem_domain_options_v1 options{}; options.struct_size = sizeof(options); options.abi_major = MGMEM_ABI_MAJOR;
    options.abi_minor = MGMEM_ABI_MINOR; options.mode = MGMEM_SYSTEM; options.budget_bytes = 64;
    mgmem_domain *system = nullptr; CHECK(mgmem_domain_create_v1(&options, &system) == MGMEM_OK);
    Errors errors;

    // Short C++ name, failure: a WRITE lease is rejected with a zero owner.
    auto rejected = mgmem::compute::read(system, writer, 101, on_error, &errors);
    CHECK(rejected.status == MGMEM_UNSUPPORTED && rejected.owner.id == 0 && rejected.origin_status == MGC_SUCCESS);
    mgc_lease_release(writer); writer = nullptr;

    lease_desc.access = MGC_ACCESS_READ;
    MgcLease *reader = nullptr; CHECK(mgc_lease_create(resource, &lease_desc, &reader) == MGC_SUCCESS);

    // Short C name, failure: budget admission fails and returns the map.
    CHECK(mgmem_domain_set_budget_v1(system, 63) == MGMEM_OK);
    auto denied = mgmem_compute_read(system, reader, 102, on_error, &errors);
    CHECK(denied.status == MGMEM_OUT_OF_BUDGET && denied.owner.id == 0 && denied.origin_status == MGC_SUCCESS);
    CHECK(mgc_lease_map(reader, &pointer, &length) == MGC_SUCCESS);
    CHECK(mgc_lease_unmap(reader) == MGC_SUCCESS);
    CHECK(mgmem_domain_set_budget_v1(system, 64) == MGMEM_OK);

    // Short C++ name, success on SYSTEM with real bytes.
    auto imported = mgmem::compute::read(system, reader, 103, on_error, &errors);
    CHECK(imported.status == MGMEM_OK && imported.owner.id != 0 && imported.origin_status == MGC_SUCCESS);
    verify_bytes(system, imported.owner);
    CHECK(mgmem_release_v1(system, imported.owner) == MGMEM_OK);
    CHECK(mgc_lease_map(reader, &pointer, &length) == MGC_SUCCESS);
    CHECK(mgc_lease_unmap(reader) == MGC_SUCCESS);
    close_domain(system);

    // Short C arena name, success on an Arena domain with the same lease.
    options.mode = MGMEM_ARENA; options.budget_bytes = 128; options.arena_chunk_bytes = 64;
    mgmem_domain *arena = nullptr; CHECK(mgmem_domain_create_v1(&options, &arena) == MGMEM_OK);
    auto arena_imported = mgmem_compute_read_arena(arena, reader, 104, on_error, &errors);
    CHECK(arena_imported.status == MGMEM_OK && arena_imported.owner.id != 0 && arena_imported.origin_status == MGC_SUCCESS);
    verify_bytes(arena, arena_imported.owner);
    CHECK(mgmem_release_v1(arena, arena_imported.owner) == MGMEM_OK);
    close_domain(arena);

    CHECK(errors.count.load() == 0);
    mgc_lease_release(reader); mgc_resource_release(resource); mgc_domain_release(compute);
    CHECK(mgc_runtime_close(runtime) == MGC_SUCCESS); mgc_runtime_release(runtime);
    std::puts("PASS short-name real-SDK success and failure paths");
}
