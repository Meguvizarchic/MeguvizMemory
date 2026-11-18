#include <mgmem/compute_stage.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
template<class T> T init() { T value{}; value.header = MGC_HEADER_INIT(T); return value; }
struct Gate { std::mutex mutex; std::condition_variable cv; bool entered = false, proceed = false; unsigned retains = 0, releases = 0; };
static void fail(void *ctx, mgmem_status) { ++*static_cast<std::atomic<unsigned> *>(ctx); }
static void retain(void *ctx) { ++static_cast<Gate *>(ctx)->retains; }
static void release(void *ctx) { ++static_cast<Gate *>(ctx)->releases; }
static void done(void *ctx, const MgcCompletion *result) {
    auto &gate = *static_cast<Gate *>(ctx);
    CHECK(result->execution_retired && result->host_finalized);
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.entered = true; gate.cv.notify_all();
    CHECK(gate.cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate.proceed; }));
}
int main() {
    auto rd = init<MgcRuntimeDesc>(); rd.provider = MGC_PROVIDER_CPU_REFERENCE; rd.memory_budget_bytes = 4096;
    MgcRuntime *runtime = nullptr; CHECK(mgc_runtime_create(&rd, &runtime) == MGC_SUCCESS);
    auto dd = init<MgcDomainDesc>(); MgcDomain *compute = nullptr; CHECK(mgc_domain_acquire(runtime, &dd, &compute) == MGC_SUCCESS);
    auto sd = init<MgcSessionDesc>(); sd.max_in_flight = 1; MgcSession *session = nullptr;
    CHECK(mgc_session_create(compute, &sd, &session) == MGC_SUCCESS);
    for (uint32_t mode = MGMEM_SYSTEM; mode <= MGMEM_ARENA; ++mode) {
        mgmem_domain_options_v1 opts{}; opts.struct_size = sizeof(opts); opts.abi_major = MGMEM_ABI_MAJOR;
        opts.abi_minor = MGMEM_ABI_MINOR; opts.mode = mode; opts.budget_bytes = 128; opts.arena_chunk_bytes = 64;
        mgmem_domain *memory = nullptr; CHECK(mgmem_domain_create_v1(&opts, &memory) == MGMEM_OK);
        mgmem_handle_v1 owner{}; CHECK(mgmem_allocate_v1(memory, 64, 8, &owner) == MGMEM_OK);
        mgmem_borrow_v1 view{}; CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_WRITE, &view) == MGMEM_OK);
        std::memset(view.data, 0x7b, 64); CHECK(mgmem_borrow_release_v1(memory, view.lease) == MGMEM_OK);
        auto desc = init<MgcResourceDesc>(); desc.capacity_bytes = 64; desc.format = MGC_FORMAT_BYTES;
        MgcResource *device = nullptr; CHECK(mgc_resource_create(compute, &desc, &device) == MGC_SUCCESS);
        auto ld = init<MgcLeaseDesc>(); ld.content_id = 1; ld.byte_length = 64; ld.access = MGC_ACCESS_WRITE;
        MgcLease *writer = nullptr; CHECK(mgc_lease_create(device, &ld, &writer) == MGC_SUCCESS);
        std::atomic<unsigned> errors{0}; mgmem_transfer t{}; t.memory = memory; t.allocation = owner; t.bytes = 64;
        t.compute = compute; t.session = session; t.device = writer; t.on_error = fail; t.error_context = &errors;
        auto prepared = mgmem_prepare_upload(&t); CHECK(prepared.status == MGMEM_OK && prepared.stage);
        mgmem_stage *stage = prepared.stage;
        CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_WRITE, &view) == MGMEM_BUSY);
        void *data = nullptr; uint64_t size = 0;
        CHECK(mgmem_stage_map(stage, &data, &size) == MGC_UNSUPPORTED_CAPABILITY);
        Gate gate; t.completion = done; t.context = &gate; t.retain_context = retain; t.release_context = release;
        auto first = mgmem_stage_send(stage, &t); CHECK(first.status == MGMEM_OK && first.ticket);
        CHECK(mgc_ticket_cancel(first.ticket) == MGC_SUCCESS);
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            CHECK(gate.cv.wait_for(lock, std::chrono::seconds(10), [&] { return gate.entered; }));
            CHECK(mgmem_stage_close(&stage) == MGMEM_BUSY && stage);
            CHECK(mgmem_stage_send(stage, &t).status == MGMEM_BUSY);
            gate.proceed = true; gate.cv.notify_all();
        }
        CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS); mgc_ticket_release(first.ticket);
        CHECK(gate.retains == 1 && gate.releases == 1);
        t.completion = nullptr; t.context = nullptr; t.retain_context = nullptr; t.release_context = nullptr;
        for (unsigned i = 0; i < 16; ++i) {
            auto sent = mgmem::compute::send(stage, t); CHECK(sent.status == MGMEM_OK && sent.ticket);
            CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS);
            auto result = init<MgcCompletion>(); CHECK(mgc_ticket_query(sent.ticket, &result) == MGC_SUCCESS && result.terminal == MGC_TERMINAL_SUCCESS);
            mgc_ticket_release(sent.ticket);
        }
        CHECK(mgmem_stage_close(&stage) == MGMEM_OK && !stage);
        CHECK(mgc_lease_map(writer, &data, &size) == MGC_SUCCESS && size == 64);
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(data)[i] == 0x7b);
        CHECK(mgc_lease_unmap(writer) == MGC_SUCCESS);
        MgcLease *reader = nullptr; CHECK(mgc_lease_publish_read(writer, &reader) == MGC_SUCCESS); mgc_lease_release(writer);
        t.device = reader;
        auto download = mgmem::compute::prepare_download(t); CHECK(download.status == MGMEM_OK && download.stage);
        stage = download.stage;
        CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_READ, &view) == MGMEM_BUSY);
        CHECK(mgmem_stage_map(stage, &data, &size) == MGC_SUCCESS && size == 64);
        std::memset(data, 0, 64);
        CHECK(mgmem_stage_close(&stage) == MGMEM_BUSY && mgmem_stage_send(stage, &t).status == MGMEM_BUSY);
        CHECK(mgmem_stage_unmap(stage) == MGC_SUCCESS);
        for (unsigned i = 0; i < 16; ++i) {
            auto sent = mgmem_stage_send(stage, &t); CHECK(sent.status == MGMEM_OK && sent.ticket);
            CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS); mgc_ticket_release(sent.ticket);
        }
        CHECK(mgmem_stage_map(stage, &data, &size) == MGC_SUCCESS && size == 64);
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(data)[i] == 0x7b);
        CHECK(mgmem_stage_unmap(stage) == MGC_SUCCESS);
        CHECK(mgmem_stage_close(&stage) == MGMEM_OK);
        CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_READ, &view) == MGMEM_OK);
        CHECK(mgmem_borrow_release_v1(memory, view.lease) == MGMEM_OK);
        /* A stage retains its borrow after the Memory owner is released. */
        download = mgmem_prepare_download(&t); CHECK(download.status == MGMEM_OK && download.stage);
        stage = download.stage;
        CHECK(mgmem_release_v1(memory, owner) == MGMEM_OK);
        auto last = mgmem_stage_send(stage, &t); CHECK(last.status == MGMEM_OK && last.ticket);
        CHECK(mgc_session_drain(session, 5000000000ULL) == MGC_SUCCESS); mgc_ticket_release(last.ticket);
        CHECK(mgmem_stage_map(stage, &data, &size) == MGC_SUCCESS && size == 64);
        for (unsigned i = 0; i < 64; ++i) CHECK(static_cast<unsigned char *>(data)[i] == 0x7b);
        CHECK(mgmem_stage_unmap(stage) == MGC_SUCCESS && mgmem_stage_close(&stage) == MGMEM_OK);
        CHECK(!errors.load()); mgc_lease_release(reader); mgc_resource_release(device);
        CHECK(mgmem_domain_begin_close_v1(memory) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(memory) == MGMEM_OK); CHECK(mgmem_domain_destroy_v1(memory) == MGMEM_OK);
    }
    CHECK(mgc_session_close(session) == MGC_SUCCESS); mgc_domain_release(compute);
    CHECK(mgc_runtime_close(runtime) == MGC_SUCCESS); mgc_runtime_release(runtime);
    std::puts("PASS prepared stage lifetime, cancel, callbacks and repeated COPY");
}
