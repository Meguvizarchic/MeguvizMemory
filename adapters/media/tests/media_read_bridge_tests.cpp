#include <mgmem/media_read_bridge.h>
#include <meguviz/media.h>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "failed %s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
template<class T> static T init() { T value{}; value.header = MGC_HEADER_INIT(T); return value; }
static void unexpected_error(void *, int32_t) { std::abort(); }

static MgmFrame *make_frame(MgcLease *lease) {
    auto desc = init<MgmFrameDesc>();
    desc.content = lease;
    desc.timebase = {1, 1000};
    desc.frame_kind = MGM_FRAME_ORIGINAL;
    desc.color = init<MgmColor>();
    desc.color.bit_depth = 8;
    desc.color.transfer = MGM_TRANSFER_SDR;
    desc.color.range = MGM_RANGE_FULL;
    MgmFrame *frame = nullptr;
    CHECK(mgm_frame_create(&desc, &frame) == MGC_SUCCESS);
    return frame;
}

int main() {
    auto rd = init<MgcRuntimeDesc>();
    rd.provider = MGC_PROVIDER_CPU_REFERENCE;
    rd.memory_budget_bytes = 4096;
    MgcRuntime *runtime = nullptr;
    CHECK(mgc_runtime_create(&rd, &runtime) == MGC_SUCCESS);
    auto dd = init<MgcDomainDesc>();
    MgcDomain *compute = nullptr;
    CHECK(mgc_domain_acquire(runtime, &dd, &compute) == MGC_SUCCESS);
    auto rdesc = init<MgcResourceDesc>();
    rdesc.allocation_id = 1;
    rdesc.capacity_bytes = 64;
    rdesc.format = MGC_FORMAT_BYTES;
    MgcResource *resource = nullptr;
    CHECK(mgc_resource_create(compute, &rdesc, &resource) == MGC_SUCCESS);
    auto ld = init<MgcLeaseDesc>();
    ld.content_id = 1;
    ld.byte_length = 64;
    ld.access = MGC_ACCESS_WRITE;
    MgcLease *writer = nullptr;
    CHECK(mgc_lease_create(resource, &ld, &writer) == MGC_SUCCESS);
    void *data = nullptr;
    uint64_t bytes = 0;
    CHECK(mgc_lease_map(writer, &data, &bytes) == MGC_SUCCESS && bytes == 64);
    for (uint64_t i = 0; i < bytes; ++i) static_cast<unsigned char *>(data)[i] = static_cast<unsigned char>(i);
    CHECK(mgc_lease_unmap(writer) == MGC_SUCCESS);
    mgc_lease_release(writer);
    ld.access = MGC_ACCESS_READ;
    MgcLease *reader = nullptr;
    CHECK(mgc_lease_create(resource, &ld, &reader) == MGC_SUCCESS);
    MgmFrame *frame = make_frame(reader);

#if defined(MGMEM_MEDIA_TEST_ARENA)
    const uint32_t modes[] = {MGMEM_SYSTEM, MGMEM_ARENA};
#else
    const uint32_t modes[] = {MGMEM_SYSTEM};
#endif
    for (uint32_t mode : modes) {
        mgmem_domain_options_v1 options{};
        options.struct_size = sizeof(options);
        options.abi_major = MGMEM_ABI_MAJOR;
        options.abi_minor = MGMEM_ABI_MINOR;
        options.mode = mode;
        options.budget_bytes = 128;
        options.arena_chunk_bytes = 64;
        mgmem_domain *memory = nullptr;
        CHECK(mgmem_domain_create_v1(&options, &memory) == MGMEM_OK);
        mgmem_handle_v1 owner{};
        int32_t status = -1;
        CHECK(mgmem_media_import_read_v1(memory, nullptr, 1, unexpected_error, nullptr, &owner, &status) == MGMEM_INVALID_ARGUMENT);
        CHECK(owner.id == 0 && status == MGC_SUCCESS);
        auto import = mgmem_media_import_read_v1;
#if defined(MGMEM_MEDIA_TEST_ARENA)
        if (mode == MGMEM_ARENA) import = mgmem_media_import_arena_read_v1;
#endif
        CHECK(import(memory, frame, 2 + mode, unexpected_error, nullptr, &owner, &status) == MGMEM_OK);
        CHECK(status == MGC_SUCCESS);
        mgmem_borrow_v1 view{};
        CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_WRITE, &view) != MGMEM_OK);
        CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_READ, &view) == MGMEM_OK);
        CHECK(mgmem_release_v1(memory, owner) == MGMEM_OK);
        CHECK(mgmem_domain_begin_close_v1(memory) == MGMEM_OK);
        for (uint64_t i = 0; i < view.length; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == i);
        CHECK(mgc_lease_map(reader, &data, &bytes) == MGC_BUSY);
        std::thread last([&] { CHECK(mgmem_borrow_release_v1(memory, view.lease) == MGMEM_OK); });
        last.join();
        CHECK(mgc_lease_map(reader, &data, &bytes) == MGC_SUCCESS);
        CHECK(mgc_lease_unmap(reader) == MGC_SUCCESS);
        CHECK(mgmem_domain_collect_v1(memory) == MGMEM_OK);
        CHECK(mgmem_domain_drain_v1(memory, 0) == MGMEM_OK);
        CHECK(mgmem_domain_destroy_v1(memory) == MGMEM_OK);
    }
    // A frame can be released while the imported Memory borrow remains active.
    mgmem_domain_options_v1 opt{};
    opt.struct_size = sizeof(opt);
    opt.abi_major = MGMEM_ABI_MAJOR;
    opt.abi_minor = MGMEM_ABI_MINOR;
    opt.mode = MGMEM_SYSTEM;
    opt.budget_bytes = 64;
    mgmem_domain *memory = nullptr;
    CHECK(mgmem_domain_create_v1(&opt, &memory) == MGMEM_OK);
    mgmem_handle_v1 owner{};
    int32_t status = -1;
    CHECK(mgmem_media_import_read_v1(memory, frame, 40, unexpected_error, nullptr, &owner, &status) == MGMEM_OK);
    mgmem_borrow_v1 view{};
    CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_READ, &view) == MGMEM_OK);
    mgm_frame_release(frame);
    mgc_lease_release(reader);
    mgc_resource_release(resource);
    for (uint64_t i = 0; i < view.length; ++i) CHECK(static_cast<unsigned char *>(view.data)[i] == i);
    CHECK(mgmem_release_v1(memory, owner) == MGMEM_OK);
    CHECK(mgmem_borrow_release_v1(memory, view.lease) == MGMEM_OK);
    CHECK(mgmem_domain_begin_close_v1(memory) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(memory) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(memory) == MGMEM_OK);
    mgc_domain_release(compute);
    CHECK(mgc_runtime_close(runtime) == MGC_SUCCESS);
    mgc_runtime_release(runtime);
    std::puts("PASS Media frame SYSTEM and Arena READ bridge");
}
