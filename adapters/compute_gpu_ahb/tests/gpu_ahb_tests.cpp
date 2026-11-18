#include <mgmem/gpu_ahb.h>
#include <meguviz/compute_android.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)
struct AHardwareBuffer {};
struct MgcLease {
    MgcLeaseInfo info{};
    AHardwareBuffer buffer{};
    std::atomic<int> refs{1};
    MgcStatus reserve_status{MGC_SUCCESS};
    MgcStatus return_status{MGC_SUCCESS};
    bool reserved{false};
    int returned_fd{-10};
};
static int wait_result = 0, dup_result = 120, last_closed = -1, errors = 0;
static int closed[8]{}, close_count = 0, external_releases = 0;
extern "C" int mgmem_gpu_test_dup(int fd) { CHECK(fd == 20); return dup_result; }
extern "C" void mgmem_gpu_test_close(int fd) { CHECK(close_count < 8); closed[close_count++] = fd; last_closed = fd; }
extern "C" int mgmem_gpu_test_wait(int fd, int) { CHECK(fd == 120); return wait_result; }
extern "C" void mgc_lease_retain(MgcLease *l) { ++l->refs; }
extern "C" void mgc_lease_release(MgcLease *l) { --l->refs; }
extern "C" MgcStatus mgc_lease_info(MgcLease *l, MgcLeaseInfo *info) { *info = l->info; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_get_buffer(MgcLease *l, AHardwareBuffer **out) { *out = &l->buffer; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_release_to_external(MgcLease *l, int32_t *fd) {
    *fd = -1;
    if (l->reserve_status != MGC_SUCCESS) return l->reserve_status;
    if (l->reserved) return MGC_BUSY;
    l->reserved = true; ++l->refs; *fd = 11; return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_set_acquire_fence(MgcLease *l, int32_t fd) {
    CHECK(l->reserved); l->returned_fd = fd;
    if (l->return_status != MGC_SUCCESS) return l->return_status;
    l->reserved = false; --l->refs; return MGC_SUCCESS;
}
void fault(void *, mgmem_status) { ++errors; }
mgmem_status retain_external(void *) { return MGMEM_OK; }
mgmem_status fail_external_release(void *) { ++external_releases; return MGMEM_PROVIDER_ERROR; }

int main(int argc, char **argv) {
    const bool hard = argc == 2 && std::strcmp(argv[1], "--hard") == 0;
    const bool release_failure = argc == 2 && std::strcmp(argv[1], "--release-failure") == 0;
    mgmem_domain_options_v1 options{};
    options.struct_size = sizeof(options); options.abi_major = MGMEM_ABI_MAJOR;
    options.abi_minor = MGMEM_ABI_MINOR; options.mode = MGMEM_SYSTEM; options.budget_bytes = 64;
    mgmem_domain *domain = nullptr;
    CHECK(mgmem_domain_create_v1(&options, &domain) == MGMEM_OK);
    mgmem_handle_v1 allocation{};
    CHECK(mgmem_allocate_v1(domain, 64, 8, &allocation) == MGMEM_OK);
    MgcLease lease{};
    lease.info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    lease.info.width = 4; lease.info.height = 4; lease.info.row_stride_bytes = 16;
    lease.info.capacity_bytes = lease.info.byte_length = 64;
    lease.info.access = MGC_ACCESS_WRITE; lease.info.format = MGC_FORMAT_RGBA8;
    if (release_failure) {
        CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK);
        unsigned char data[64]{};
        mgmem_external_buffer_checked_v1 external{};
        external.struct_size = sizeof(external); external.abi_major = MGMEM_ABI_MAJOR;
        external.abi_minor = MGMEM_ABI_MINOR; external.data = data; external.capacity_bytes = sizeof(data);
        external.alignment = 1; external.external_id = 1; external.access = MGMEM_READ;
        external.retain = retain_external; external.release_checked = fail_external_release;
        external.managing_runtime_origin = mgmem_runtime_origin_v1();
        CHECK(mgmem_import_external_checked_v1(domain, &external, &allocation) == MGMEM_OK);
        auto result = mgmem_gpu_ahb_begin(domain, allocation, 0, 64, &lease, fault, nullptr);
        CHECK(result.status == MGMEM_OK && result.pending && lease.refs == 3);
        CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK && !external_releases);
        int32_t compute = -1;
        CHECK(mgmem_gpu_ahb_publish(result.pending, 20, &compute) == MGMEM_OK);
        CHECK(close_count == 1 && closed[0] == 11 && lease.refs == 2);
        CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_PROVIDER_ERROR && result.pending);
        CHECK(errors == 1 && external_releases == 1 && lease.refs == 2);
        CHECK(close_count == 1 && closed[0] == 11); // uncertain token keeps completion fd
        CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_PROVIDER_ERROR && errors == 1);
        CHECK(mgmem_gpu_ahb_cancel(&result.pending, &compute) == MGMEM_PROVIDER_ERROR);
        CHECK(close_count == 1 && external_releases == 1);
        mgmem_import_snapshot_v1 snapshot{};
        CHECK(mgmem_domain_import_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
        CHECK(snapshot.poisoned && snapshot.external_backing_bytes == 64);
        std::puts("PASS final Memory release failure quarantines token, lease and completion fd");
        std::fflush(stdout); std::_Exit(0);
    }
    if (hard) {
        lease.reserve_status = MGC_INTERNAL_ERROR;
        auto result = mgmem_gpu_ahb_begin(domain, allocation, 0, 64, &lease, fault, nullptr);
        CHECK(result.status == MGMEM_PROVIDER_ERROR && result.pending && lease.refs == 2);
        mgmem_borrow_v1 write{};
        CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_BUSY);
        CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_PROVIDER_ERROR && result.pending);
        int32_t compute = 0;
        CHECK(mgmem_gpu_ahb_cancel(&result.pending, &compute) == MGMEM_PROVIDER_ERROR && result.pending);
        std::puts("PASS hard failure retains uncertain lease and Memory borrow");
        std::fflush(stdout); std::_Exit(0);
    }
    lease.reserve_status = MGC_BUSY;
    auto busy = mgmem_gpu_ahb_begin(domain, allocation, 0, 64, &lease, fault, nullptr);
    CHECK(busy.status == MGMEM_BUSY && !busy.pending && lease.refs == 1);
    lease.reserve_status = MGC_SUCCESS;
    auto result = mgmem::gpu_ahb::begin(domain, allocation, 0, 64, &lease, fault, nullptr);
    CHECK(result.status == MGMEM_OK && result.pending && result.view.buffer == &lease.buffer && result.view.ready_fd == 11);
    CHECK(result.view.length == 64 && lease.reserved && lease.refs == 3);
    mgmem_borrow_v1 write{};
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_BUSY);
    int32_t compute = -1;
    dup_result = -1;
    CHECK(mgmem_gpu_ahb_publish(result.pending, 20, &compute) == MGMEM_PROVIDER_ERROR && lease.reserved);
    CHECK(close_count == 0);
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_BUSY && result.pending);
    dup_result = 120;
    lease.return_status = MGC_BUSY;
    CHECK(mgmem_gpu_ahb_publish(result.pending, 20, &compute) == MGMEM_BUSY && compute == MGC_BUSY && last_closed == 120);
    CHECK(close_count == 1 && closed[0] == 120);
    CHECK(lease.reserved && result.pending && mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_BUSY);
    lease.return_status = MGC_SUCCESS;
    CHECK(mgmem_gpu_ahb_publish(result.pending, 20, &compute) == MGMEM_OK && lease.returned_fd == 20 && !lease.reserved);
    CHECK(close_count == 2 && closed[1] == 11);
    CHECK(mgmem_gpu_ahb_cancel(&result.pending, &compute) == MGMEM_BUSY && result.pending);
    wait_result = 1;
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_TIMEOUT && result.pending);
    CHECK(close_count == 2);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_BUSY);
    wait_result = 2; /* poll ready but sync-file status was interrupted */
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_TIMEOUT && result.pending);
    CHECK(close_count == 2 && lease.refs == 2);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_BUSY);
    wait_result = -1;
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_PROVIDER_ERROR && result.pending);
    CHECK(close_count == 2);
    /* A signaled error and unavailable/unknown status are not retirement proof.
     * Neither consumes the token, closes its fd nor releases the source lease. */
    wait_result = -2;
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_PROVIDER_ERROR && result.pending);
    CHECK(close_count == 2 && lease.refs == 2);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_BUSY);
    wait_result = -3;
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_PROVIDER_ERROR && result.pending);
    CHECK(close_count == 2 && lease.refs == 2);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_BUSY);
    wait_result = 0;
    CHECK(mgmem_gpu_ahb_retire(&result.pending, 0) == MGMEM_OK && !result.pending && lease.refs == 1 && last_closed == 120);
    CHECK(close_count == 3 && closed[2] == 120);
    CHECK(mgmem_borrow_acquire_v1(domain, allocation, 0, 64, MGMEM_WRITE, &write) == MGMEM_OK);
    CHECK(mgmem_borrow_release_v1(domain, write.lease) == MGMEM_OK);
    auto cancel = mgmem_gpu_ahb_begin(domain, allocation, 0, 64, &lease, fault, nullptr);
    CHECK(cancel.status == MGMEM_OK);
    CHECK(mgmem_gpu_ahb_cancel(&cancel.pending, &compute) == MGMEM_OK && !cancel.pending && lease.returned_fd == 11);
    CHECK(close_count == 4 && closed[3] == 11);
    CHECK(mgmem_release_v1(domain, allocation) == MGMEM_OK && !errors);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    std::puts("PASS GPU AHB reservation and fence lifetime");
}
