#include <mgmem/ahb_upload.h>
#include <meguviz/compute_android.h>
#include "platform.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
struct AHardwareBuffer {};
struct MgcLease {
    MgcLeaseInfo info{};
    AHardwareBuffer buffer{};
    std::atomic<int> refs{1};
    bool reserved = false;
    bool return_busy = true;
};
struct mgmem_upload_platform { bool cleaned = false; };
static std::string events;
static unsigned char copied[64]{};
static bool complete = false;
static int producer_result = 1;
static unsigned errors = 0;
static void on_error(void *, mgmem_status) { ++errors; }

extern "C" void mgc_lease_retain(MgcLease *l) { ++l->refs; }
extern "C" void mgc_lease_release(MgcLease *l) { --l->refs; }
extern "C" MgcStatus mgc_lease_info(MgcLease *l, MgcLeaseInfo *out) { *out = l->info; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_get_buffer(MgcLease *l, AHardwareBuffer **out) { *out = &l->buffer; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_release_to_external(MgcLease *l, int32_t *fd) {
    CHECK(!l->reserved); l->reserved = true; ++l->refs; *fd = 11; events += 'B'; return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_set_acquire_fence(MgcLease *l, int32_t fd) {
    CHECK(l->reserved && fd == -1); events += 'P';
    if (l->return_busy) return MGC_BUSY;
    l->reserved = false; --l->refs; return MGC_SUCCESS;
}
extern "C" int mgmem_gpu_test_dup(int) { std::abort(); }
extern "C" void mgmem_gpu_test_close(int fd) {
    if (fd == 11) events += 'r'; else { CHECK(fd == 120); events += 'd'; }
}
extern "C" int mgmem_gpu_test_wait(int, int) { std::abort(); }

mgmem_status mgmem_upload_platform_open(mgmem_upload_platform **out, int32_t *) noexcept {
    events += 'O'; *out = new mgmem_upload_platform{}; return MGMEM_OK;
}
mgmem_status mgmem_upload_platform_write(mgmem_upload_platform *, const mgmem_gpu_ahb_view &v, uint64_t stride, int32_t *) noexcept {
    CHECK(v.data && v.length == 64 && v.buffer && stride == 16); events += 'W'; std::memcpy(copied, v.data, 64); return MGMEM_OK;
}
mgmem_status mgmem_upload_platform_fence(mgmem_upload_platform *, int *fd, int32_t *) noexcept { events += 'F'; *fd = 20; return MGMEM_OK; }
mgmem_status mgmem_upload_platform_cleanup(mgmem_upload_platform *p, int32_t *) noexcept {
    CHECK(complete && p && !p->cleaned); p->cleaned = true; events += 'C'; return MGMEM_OK;
}
void mgmem_upload_platform_discard(mgmem_upload_platform *p) noexcept { CHECK(p && p->cleaned); events += 'x'; delete p; }
int mgmem_upload_platform_wait(int fd, int timeout) noexcept {
    CHECK(timeout == 0);
    if (fd == 11) { events += 'R'; return 0; }
    CHECK(fd == 20); events += 'S'; return producer_result;
}
int mgmem_upload_platform_probe(int fd) noexcept {
    CHECK(fd == 20); events += 'S'; return producer_result;
}
void mgmem_upload_platform_close_fd(int fd) noexcept { CHECK(fd == 20); events += 'f'; }

int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && (!std::strcmp(argv[1], "--failed") || !std::strcmp(argv[1], "--unknown") ||
                                     !std::strcmp(argv[1], "--unverified"))));
    mgmem_domain_options_v1 options{};
    options.struct_size = sizeof(options); options.abi_major = MGMEM_ABI_MAJOR; options.abi_minor = MGMEM_ABI_MINOR;
    options.mode = MGMEM_SYSTEM; options.budget_bytes = 64;
    mgmem_domain *domain = nullptr; CHECK(mgmem_domain_create_v1(&options, &domain) == MGMEM_OK);
    mgmem_handle_v1 owner{}; CHECK(mgmem_allocate_v1(domain, 64, 8, &owner) == MGMEM_OK);
    mgmem_borrow_v1 view{}; CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &view) == MGMEM_OK);
    std::memset(view.data, 0x5a, 64); CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
    MgcLease lease{}; lease.info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    lease.info.width = 4; lease.info.height = 4; lease.info.row_stride_bytes = 32;
    lease.info.capacity_bytes = lease.info.byte_length = 64; lease.info.access = MGC_ACCESS_WRITE; lease.info.format = MGC_FORMAT_RGBA8;
    mgmem_ahb_upload_desc d{}; d.memory = domain; d.allocation = owner; d.length = 64; d.source_stride = 16;
    d.width = d.height = 4; d.write_lease = &lease; d.on_error = on_error;
    auto result = mgmem_ahb_upload_submit(&d);
    CHECK(result.status == MGMEM_OK && result.pending && result.phase == MGMEM_UPLOAD_PRODUCING && lease.refs == 3);
    CHECK(events == "OBRWF");
    for (auto b : copied) CHECK(b == 0x5a);
    CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &view) == MGMEM_BUSY);
    CHECK(mgmem_release_v1(domain, owner) == MGMEM_OK);
    mgmem_snapshot_v1 snapshot{};
    CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
    CHECK(snapshot.retiring_bytes == 64 && snapshot.active_leases == 1);
    if (argc == 2 && std::strcmp(argv[1], "--unverified")) {
        producer_result = !std::strcmp(argv[1], "--failed") ? -1 : -2;
        CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_PROVIDER_ERROR && result.pending);
        CHECK(mgmem_ahb_upload_phase(result.pending) == MGMEM_UPLOAD_QUARANTINED);
        CHECK(lease.reserved && lease.refs == 3 && events == "OBRWFS");
        CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
        CHECK(snapshot.retiring_bytes == 64 && snapshot.active_leases == 1 && !errors);
        CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_PROVIDER_ERROR && events == "OBRWFS");
        std::puts("PASS producer error or unqueryable status retains real Memory borrow");
        std::fflush(stdout); std::_Exit(0);
    }
    if (argc == 2) {
        producer_result = 2;
        CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_TIMEOUT && result.pending);
        CHECK(mgmem_ahb_upload_phase(result.pending) == MGMEM_UPLOAD_PRODUCING && lease.reserved && lease.refs == 3);
        CHECK(events == "OBRWFS");
        CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
        CHECK(snapshot.retiring_bytes == 64 && snapshot.active_leases == 1);
        complete = true; producer_result = 0; lease.return_busy = false;
        CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_OK && !result.pending && lease.refs == 1);
        CHECK(events == "OBRWFSSCPrfx");
        CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
        CHECK(!snapshot.retiring_bytes && !snapshot.active_leases);
        CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
        std::puts("PASS interrupted fence status is retryable and keeps real Memory borrowed");
        return 0;
    }
    CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_TIMEOUT && result.pending);
    CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
    CHECK(snapshot.retiring_bytes == 64 && snapshot.active_leases == 1);
    complete = true; producer_result = 0;
    CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_BUSY && result.pending && lease.reserved);
    CHECK(mgmem_ahb_upload_phase(result.pending) == MGMEM_UPLOAD_PUBLISH && events == "OBRWFSSCP");
    CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
    CHECK(snapshot.retiring_bytes == 64 && snapshot.active_leases == 1);
    lease.return_busy = false;
    int32_t origin = 0;
    CHECK(mgmem_ahb_upload_retry(result.pending, &origin) == MGMEM_OK && lease.refs == 2 && !lease.reserved);
    CHECK(events == "OBRWFSSCPPr");
    CHECK(mgmem_ahb_upload_wait(&result.pending, 0) == MGMEM_OK && !result.pending && lease.refs == 1);
    CHECK(events == "OBRWFSSCPPrfx");
    CHECK(mgmem_domain_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
    CHECK(snapshot.retiring_bytes == 0 && snapshot.active_leases == 0 && !errors);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    std::puts("PASS actual Memory owner lifetime across GPU upload fence");
}
