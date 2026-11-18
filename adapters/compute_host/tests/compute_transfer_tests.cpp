#include <mgmem/compute_transfer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
template <typename T> T init() { T value{}; value.header = MGC_HEADER_INIT(T); return value; }
struct Gate { std::mutex mutex; std::condition_variable cv; bool entered = false, resume = false; MgcCompletion result{}; };
void complete(void *opaque, const MgcCompletion *completion) {
    auto &gate = *static_cast<Gate *>(opaque);
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.result = *completion;
    gate.entered = true;
    gate.cv.notify_all();
    CHECK(gate.cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate.resume; }));
}
void on_error(void *opaque, mgmem_status) { ++*static_cast<std::atomic<unsigned> *>(opaque); }

int main() {
    auto runtime_desc = init<MgcRuntimeDesc>();
    runtime_desc.provider = MGC_PROVIDER_CPU_REFERENCE;
    runtime_desc.memory_budget_bytes = 4096;
    MgcRuntime *runtime = nullptr;
    CHECK(mgc_runtime_create(&runtime_desc, &runtime) == MGC_SUCCESS);
    auto compute_desc = init<MgcDomainDesc>();
    MgcDomain *compute = nullptr;
    CHECK(mgc_domain_acquire(runtime, &compute_desc, &compute) == MGC_SUCCESS);
    auto session_desc = init<MgcSessionDesc>();
    session_desc.max_in_flight = 1;
    MgcSession *session = nullptr;
    CHECK(mgc_session_create(compute, &session_desc, &session) == MGC_SUCCESS);

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
        mgmem_handle_v1 allocation{};
        CHECK(mgmem_allocate_v1(domain, 64, 8, &allocation) == MGMEM_OK);
        mgmem_borrow_v1 view{};
        CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &view) == MGMEM_OK);
        for (unsigned i = 0; i < 64; ++i) static_cast<unsigned char *>(view.data)[i] = static_cast<unsigned char>(i);
        CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);

        auto resource_desc = init<MgcResourceDesc>();
        resource_desc.capacity_bytes = 64;
        resource_desc.format = MGC_FORMAT_BYTES;
        MgcResource *resource = nullptr;
        CHECK(mgc_resource_create(compute, &resource_desc, &resource) == MGC_SUCCESS);
        auto lease_desc = init<MgcLeaseDesc>();
        lease_desc.content_id = 1;
        lease_desc.byte_length = 64;
        lease_desc.access = MGC_ACCESS_WRITE;
        MgcLease *write = nullptr;
        CHECK(mgc_lease_create(resource, &lease_desc, &write) == MGC_SUCCESS);
        std::atomic<unsigned> errors{0};
        Gate gate;
        mgmem_transfer transfer{};
        transfer.memory = domain;
        transfer.allocation = allocation;
        transfer.bytes = 64;
        transfer.compute = compute;
        transfer.session = session;
        transfer.device = write;
        transfer.completion = complete;
        transfer.context = &gate;
        transfer.on_error = on_error;
        transfer.error_context = &errors;
        auto up = mgmem_upload(&transfer);
        CHECK(up.status == MGMEM_OK && up.origin_status == MGC_ACCEPTED && up.ticket);
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            CHECK(gate.cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate.entered; }));
            CHECK(gate.result.terminal == MGC_TERMINAL_SUCCESS && gate.result.execution_retired && gate.result.host_finalized);
            auto queried = init<MgcCompletion>();
            CHECK(mgc_ticket_query(up.ticket, &queried) == MGC_SUCCESS && queried.terminal == MGC_TERMINAL_SUCCESS);
            CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &view) == MGMEM_BUSY);
            gate.resume = true;
            gate.cv.notify_all();
        }
        CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS);
        mgc_ticket_release(up.ticket);
        CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &view) == MGMEM_OK);
        for (unsigned i = 0; i < 64; ++i) static_cast<unsigned char *>(view.data)[i] = 0;
        CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
        MgcLease *read = nullptr;
        CHECK(mgc_lease_publish_read(write, &read) == MGC_SUCCESS);
        mgc_lease_release(write);
        transfer.device = read;
        transfer.completion = nullptr;
        transfer.context = nullptr;
        auto down = mgmem_download(&transfer);
        CHECK(down.status == MGMEM_OK && down.origin_status == MGC_ACCEPTED && down.ticket);
        CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS);
        auto done = init<MgcCompletion>();
        CHECK(mgc_ticket_query(down.ticket, &done) == MGC_SUCCESS && done.terminal == MGC_TERMINAL_SUCCESS);
        mgc_ticket_release(down.ticket);
        CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_READ, &view) == MGMEM_OK);
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == i);
        CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);

        /* Direction mismatch rejects submission, but caller's lease survives. */
        auto rejected = mgmem_upload(&transfer);
        CHECK(rejected.status == MGMEM_INVALID_ARGUMENT && rejected.origin_status == MGC_INVALID_ARGUMENT && !rejected.ticket);
        auto info = init<MgcLeaseInfo>();
        CHECK(mgc_lease_info(read, &info) == MGC_SUCCESS && info.access == MGC_ACCESS_READ);
        CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &view) == MGMEM_OK);
        CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
        transfer.offset = 1;
        auto overflow = mgmem_download(&transfer);
        CHECK(overflow.status != MGMEM_OK && !overflow.ticket);
        CHECK(!errors.load());
        mgc_lease_release(read);
        mgc_resource_release(resource);
        CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
        CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
        CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    }
    CHECK(mgc_session_close(session) == MGC_SUCCESS);
    mgc_domain_release(compute);
    CHECK(mgc_runtime_close(runtime) == MGC_SUCCESS);
    mgc_runtime_release(runtime);
    std::puts("PASS Memory transfer ownership and host COPY");
}
