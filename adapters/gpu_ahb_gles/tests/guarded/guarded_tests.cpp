#include <mgmem/ahb_upload_guarded.h>
#include <meguviz/compute_android.h>
#include "platform.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failure %d: %s\n", __LINE__, #x); std::abort(); } } while (0)
struct AHardwareBuffer {};
struct MgcDomain { uint64_t id, generation; bool capability; };
struct MgcLease {
    MgcLeaseInfo info{}; AHardwareBuffer buffer{}; std::atomic<int> refs{1};
    bool reserved = false, registered = false, gate = false, failed = false;
    int fail_busy = 0; uint64_t guard = 0;
};
struct mgmem_upload_platform { bool cleaned = false; };
static int producer = 1, fake_wait_calls = 0, finish_calls = 0, probe_calls = 0, cleanup_calls = 0;
static int dup_failure = 0, register_busy = 0;
static mgmem_status cleanup_result = MGMEM_OK;
static int callback_errors = 0;
static void on_error(void *, mgmem_status) { ++callback_errors; }
extern "C" void mgc_lease_retain(MgcLease *l) { ++l->refs; }
extern "C" void mgc_lease_release(MgcLease *l) { --l->refs; }
extern "C" MgcStatus mgc_lease_info(MgcLease *l, MgcLeaseInfo *out) { *out = l->info; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_domain_identity(MgcDomain *d, uint64_t *id, uint64_t *gen) {
    *id = d->id; *gen = d->generation; return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_domain_capability(MgcDomain *d, uint32_t feature, MgcCapability *out) {
    CHECK(feature == 19);
    if (!d->capability) return MGC_INVALID_ARGUMENT;
    out->feature = feature; out->advertised = out->enabled = 1; return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_get_buffer(MgcLease *l, AHardwareBuffer **out) { *out = &l->buffer; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_release_to_external(MgcLease *l, int32_t *fd) {
    CHECK(!l->reserved); l->reserved = true; ++l->refs; *fd = 11; return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_set_acquire_fence(MgcLease *l, int32_t fd) {
    CHECK(l->reserved && fd == -1); l->reserved = false; --l->refs; return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_set_guarded_producer_fence(MgcLease *l, int32_t fd, uint64_t *generation) {
    CHECK(l->reserved && !l->registered && fd == 20);
    if (register_busy) { --register_busy; return MGC_BUSY; }
    l->registered = true; l->guard = *generation = 73;
    return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_finish_guarded_producer(MgcLease *l, uint64_t generation, MgcStatus result) {
    CHECK(l->registered && l->guard == generation); ++finish_calls;
    if (l->fail_busy) { --l->fail_busy; return MGC_BUSY; }
    if (result == -15) { l->failed = true; return MGC_SUCCESS; }
    CHECK(result == MGC_SUCCESS && producer == 0); l->gate = true; l->reserved = false; --l->refs; return MGC_SUCCESS;
}
extern "C" int mgmem_gpu_test_dup(int fd) { CHECK(fd == 20); return dup_failure ? -1 : 120; }
extern "C" void mgmem_gpu_test_close(int fd) { CHECK(fd == 11 || fd == 120); }
extern "C" int mgmem_gpu_test_wait(int, int) { ++fake_wait_calls; return -1; }
mgmem_status mgmem_upload_platform_open(mgmem_upload_platform **out, int32_t *) noexcept {
    *out = new mgmem_upload_platform{}; return MGMEM_OK;
}
mgmem_status mgmem_upload_platform_write(mgmem_upload_platform *, const mgmem_gpu_ahb_view &view,
                                          uint64_t stride, int32_t *) noexcept {
    CHECK(view.data && view.length == 64 && stride == 16); return MGMEM_OK;
}
mgmem_status mgmem_upload_platform_fence(mgmem_upload_platform *, int *fd, int32_t *) noexcept { *fd = 20; return MGMEM_OK; }
mgmem_status mgmem_upload_platform_cleanup(mgmem_upload_platform *p, int32_t *) noexcept {
    CHECK(p && !p->cleaned && producer == 0); ++cleanup_calls;
    if (cleanup_result == MGMEM_OK) p->cleaned = true;
    return cleanup_result;
}
void mgmem_upload_platform_discard(mgmem_upload_platform *p) noexcept { delete p; }
int mgmem_upload_platform_wait(int fd, int) noexcept { return fd == 11 ? 0 : producer; }
int mgmem_upload_platform_probe(int fd) noexcept { CHECK(fd == 20); ++probe_calls; return producer; }
void mgmem_upload_platform_close_fd(int fd) noexcept { CHECK(fd == 20); }

int main(int argc, char **argv) {
    CHECK(argc == 2);
    mgmem_domain_options_v1 options{}; options.struct_size = sizeof(options); options.abi_major = MGMEM_ABI_MAJOR;
    options.abi_minor = MGMEM_ABI_MINOR; options.mode = MGMEM_SYSTEM; options.budget_bytes = 64;
    mgmem_domain *memory = nullptr; CHECK(mgmem_domain_create_v1(&options, &memory) == MGMEM_OK);
    mgmem_handle_v1 owner{}; CHECK(mgmem_allocate_v1(memory, 64, 8, &owner) == MGMEM_OK);
    MgcLease lease{}; lease.info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    lease.info.domain_id = 55; lease.info.domain_generation = 4;
    lease.info.width = lease.info.height = 4; lease.info.row_stride_bytes = 16;
    lease.info.capacity_bytes = lease.info.byte_length = 64; lease.info.access = MGC_ACCESS_WRITE;
    lease.info.format = MGC_FORMAT_RGBA8;
    mgmem_ahb_upload_desc desc{}; desc.memory = memory; desc.allocation = owner; desc.length = 64;
    desc.source_stride = 16; desc.width = desc.height = 4; desc.write_lease = &lease; desc.on_error = on_error;
    auto r = mgmem_ahb_upload_submit(&desc); CHECK(r.status == MGMEM_OK && r.pending);
    MgcDomain domain{55, 4, false}, wrong{56, 4, true}; int32_t raw = 0;
    CHECK(mgmem_ahb_queue(r.pending, &wrong, &raw) == MGMEM_INVALID_ARGUMENT && raw == MGC_INCOMPATIBLE_DOMAIN);
    CHECK(mgmem_ahb_queue(r.pending, &domain, &raw) == MGMEM_UNSUPPORTED && !lease.registered);
    domain.capability = true;
    dup_failure = 1;
    CHECK(mgmem_ahb_queue(r.pending, &domain, &raw) == MGMEM_PROVIDER_ERROR && !lease.registered);
    dup_failure = 0; register_busy = 1;
    CHECK(mgmem_ahb_queue(r.pending, &domain, &raw) == MGMEM_BUSY && raw == MGC_BUSY && !lease.registered);
    CHECK(mgmem_ahb_queue(r.pending, &domain, &raw) == MGMEM_OK && lease.registered && lease.reserved);
    CHECK(mgmem_ahb_queue(r.pending, &domain, &raw) == MGMEM_BUSY);
    mgmem_borrow_v1 borrow{};
    CHECK(mgmem_borrow_acquire_v1(memory, owner, 0, 64, MGMEM_WRITE, &borrow) == MGMEM_BUSY);
    CHECK(mgmem_release_v1(memory, owner) == MGMEM_OK);
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_TIMEOUT && !lease.gate && !lease.failed);
    if (!std::strcmp(argv[1], "failure") || !std::strcmp(argv[1], "cleanup")) {
        producer = !std::strcmp(argv[1], "failure") ? -2 : 0;
        if (!std::strcmp(argv[1], "cleanup")) cleanup_result = MGMEM_PROVIDER_ERROR;
        lease.fail_busy = 1;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && r.pending);
        CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_FAIL_NOTIFY && !lease.failed && !lease.gate);
        CHECK(mgmem_ahb_upload_retry(r.pending, &raw) == MGMEM_OK && lease.failed && !lease.gate);
        CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_QUARANTINED && lease.refs == 3);
        std::puts("PASS guarded failure delivered; physical owners quarantined"); std::fflush(stdout); std::_Exit(0);
    }
    if (!std::strcmp(argv[1], "unknown")) {
        producer = -1;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && r.pending);
        CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_QUARANTINED && !lease.failed && !lease.gate);
        std::puts("PASS unknown fence never asserted as producer failure"); std::fflush(stdout); std::_Exit(0);
    }
    CHECK(!std::strcmp(argv[1], "success")); producer = 0; lease.fail_busy = 1;
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_BUSY && r.pending && !lease.gate);
    CHECK(cleanup_calls == 1 && probe_calls == 2);
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_OK && !r.pending && lease.gate && lease.refs == 1);
    CHECK(cleanup_calls == 1 && probe_calls == 2 && fake_wait_calls == 0 && finish_calls == 2 && !callback_errors);
    CHECK(mgmem_domain_begin_close_v1(memory) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(memory) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(memory) == MGMEM_OK);
    std::puts("PASS guarded queue cleanup gate and verified retirement");
}
