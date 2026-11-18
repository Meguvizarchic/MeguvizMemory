#include <mgmem/compute_host_bridge.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
template <typename T> T init() { T v{}; v.header = MGC_HEADER_INIT(T); return v; }
void on_error(void *context, mgmem_status) { ++*static_cast<std::atomic<unsigned> *>(context); }

int main() {
    auto runtime_desc = init<MgcRuntimeDesc>();
    runtime_desc.provider = MGC_PROVIDER_CPU_REFERENCE;
    runtime_desc.memory_budget_bytes = 4096;
    MgcRuntime *runtime = nullptr;
    CHECK(mgc_runtime_create(&runtime_desc, &runtime) == MGC_SUCCESS);
    auto compute_desc = init<MgcDomainDesc>();
    MgcDomain *compute = nullptr;
    CHECK(mgc_domain_acquire(runtime, &compute_desc, &compute) == MGC_SUCCESS);
    auto capability = init<MgcCapability>();
    CHECK(mgc_domain_capability(compute, MGC_CAP_EXTERNAL_HOST_MEMORY, &capability) == MGC_SUCCESS);
    CHECK(capability.advertised && capability.enabled);

    for (uint32_t mode : {MGMEM_SYSTEM, MGMEM_ARENA}) {
        mgmem_domain_options_v1 options{};
        options.struct_size = sizeof(options);
        options.abi_major = MGMEM_ABI_MAJOR;
        options.abi_minor = MGMEM_ABI_MINOR;
        options.mode = mode;
        options.budget_bytes = 128;
        options.arena_chunk_bytes = 64;
        mgmem_domain *domain = nullptr;
        CHECK(mgmem_domain_create_v1(&options, &domain) == MGMEM_OK);
        mgmem_handle_v1 owner{};
        CHECK(mgmem_allocate_v1(domain, 64, 8, &owner) == MGMEM_OK);
        mgmem_borrow_v1 initial{};
        CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &initial) == MGMEM_OK);
        for (unsigned i = 0; i < 64; ++i) static_cast<unsigned char *>(initial.data)[i] = static_cast<unsigned char>(i);
        CHECK(mgmem_borrow_release_v1(domain, initial.lease) == MGMEM_OK);

        auto desc = init<MgcResourceDesc>();
        desc.capacity_bytes = 64;
        desc.format = MGC_FORMAT_BYTES;
        std::atomic<unsigned> errors{0};
        int32_t origin_status = -1;
        MgcResource *resource = nullptr;
        CHECK(mgmem_compute_create_host_resource_v1(domain, owner, 0, compute, &desc, MGC_ACCESS_READ,
              on_error, &errors, &resource, &origin_status) == MGMEM_OK);
        CHECK(resource && origin_status == MGC_SUCCESS);
        auto lease_desc = init<MgcLeaseDesc>();
        lease_desc.content_id = 1;
        lease_desc.byte_length = 64;
        lease_desc.access = MGC_ACCESS_WRITE;
        MgcLease *lease = nullptr;
        CHECK(mgc_lease_create(resource, &lease_desc, &lease) == MGC_UNSUPPORTED_CAPABILITY);
        lease_desc.access = MGC_ACCESS_READ;
        CHECK(mgc_lease_create(resource, &lease_desc, &lease) == MGC_SUCCESS);
        CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &initial) == MGMEM_BUSY);

        mgmem_handle_v1 output_owner{};
        CHECK(mgmem_allocate_v1(domain, 64, 8, &output_owner) == MGMEM_OK);
        MgcResource *output_resource = nullptr;
        desc.allocation_id = 0;
        CHECK(mgmem_compute_create_host_resource_v1(domain, output_owner, 0, compute, &desc, MGC_ACCESS_WRITE,
              on_error, &errors, &output_resource, &origin_status) == MGMEM_OK);
        CHECK(mgmem_borrow_acquire_v1(domain, output_owner, 0, 64, MGMEM_READ, &initial) == MGMEM_BUSY);
        auto output_desc = init<MgcLeaseDesc>();
        output_desc.content_id = 1;
        output_desc.byte_length = 64;
        output_desc.access = MGC_ACCESS_WRITE;
        MgcLease *output_lease = nullptr;
        CHECK(mgc_lease_create(output_resource, &output_desc, &output_lease) == MGC_SUCCESS);
        auto session_desc = init<MgcSessionDesc>();
        session_desc.max_in_flight = 1;
        MgcSession *session = nullptr;
        CHECK(mgc_session_create(compute, &session_desc, &session) == MGC_SUCCESS);
        auto job = init<MgcSubmitDesc>();
        job.request_id = 1;
        job.operation = MGC_OPERATION_COPY;
        job.input = lease;
        job.output = output_lease;
        job.byte_count = 64;
        MgcTicket *ticket = nullptr;
        mgc_lease_retain(lease);
        mgc_lease_retain(output_lease);
        CHECK(mgc_submit(session, &job, &ticket) == MGC_ACCEPTED);
        CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS);
        auto done = init<MgcCompletion>();
        CHECK(mgc_ticket_query(ticket, &done) == MGC_SUCCESS && done.terminal == MGC_TERMINAL_SUCCESS);
        mgc_ticket_release(ticket);
        CHECK(mgc_session_close(session) == MGC_SUCCESS);
        mgc_lease_release(output_lease);
        mgc_resource_release(output_resource);
        CHECK(mgmem_borrow_acquire_v1(domain, output_owner, 0, 64, MGMEM_READ, &initial) == MGMEM_OK);
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(initial.data)[i] == i);
        CHECK(mgmem_borrow_release_v1(domain, initial.lease) == MGMEM_OK);
        CHECK(mgmem_release_v1(domain, output_owner) == MGMEM_OK);
        CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
        CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
        mgc_resource_release(resource);
        void *data = nullptr;
        uint64_t size = 0;
        CHECK(mgc_lease_map(lease, &data, &size) == MGC_SUCCESS && size == 64);
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(data)[i] == i);
        CHECK(mgc_lease_unmap(lease) == MGC_SUCCESS);
        std::thread last([&] { mgc_lease_release(lease); });
        last.join();
        CHECK(errors.load() == 0);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
        CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    }
    mgc_domain_release(compute);
    CHECK(mgc_runtime_close(runtime) == MGC_SUCCESS);
    mgc_runtime_release(runtime);
    std::puts("PASS Memory-backed Compute host resource");
}
