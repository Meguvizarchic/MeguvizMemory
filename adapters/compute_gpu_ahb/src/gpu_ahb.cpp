#include <mgmem/gpu_ahb.h>
#include <meguviz/compute_android.h>
#include <new>

#if defined(MGMEM_GPU_AHB_TESTING)
extern "C" int mgmem_gpu_test_dup(int);
extern "C" void mgmem_gpu_test_close(int);
extern "C" int mgmem_gpu_test_wait(int, int);
#else
#include "../../common/sync_file_wait.hpp"
#include <unistd.h>
#endif

struct mgmem_gpu_ahb {
    mgmem_domain *domain{};
    mgmem_borrow_v1 view{};
    MgcLease *lease{};
    mgmem_gpu_ahb_error_fn on_error{};
    void *context{};
    int ready_fd{-1};
    int completion_fd{-1};
    /* Only the private guarded registration can populate this. The caller
     * retains the original fd; completion_fd is its dup of the same file. */
    int guarded_source_fd{-1};
    enum { reserved, published, uncertain } state{reserved};
};

namespace {
int copy_fd(int fd) noexcept {
#if defined(MGMEM_GPU_AHB_TESTING)
    return mgmem_gpu_test_dup(fd);
#else
    return dup(fd);
#endif
}
void close_fd(int fd) noexcept {
    if (fd < 0) return;
#if defined(MGMEM_GPU_AHB_TESTING)
    mgmem_gpu_test_close(fd);
#else
    (void)close(fd);
#endif
}
int wait_fd(int fd, int timeout_ms) noexcept {
    if (fd < 0) return 0;
#if defined(MGMEM_GPU_AHB_TESTING)
    return mgmem_gpu_test_wait(fd, timeout_ms);
#else
    return mgmem_detail::sync_file_wait(fd, timeout_ms);
#endif
}
mgmem_status translate(MgcStatus status) noexcept {
    switch (status) {
    case MGC_SUCCESS: return MGMEM_OK;
    case MGC_INVALID_ARGUMENT: return MGMEM_INVALID_ARGUMENT;
    case MGC_BUSY: case MGC_BACKPRESSURE: return MGMEM_BUSY;
    case MGC_OUT_OF_MEMORY: return MGMEM_OUT_OF_MEMORY;
    case MGC_TIMEOUT: return MGMEM_TIMEOUT;
    case MGC_UNSUPPORTED_ABI: case MGC_UNSUPPORTED_CAPABILITY: return MGMEM_UNSUPPORTED;
    default: return MGMEM_PROVIDER_ERROR;
    }
}
mgmem_status finish(mgmem_gpu_ahb **pending) noexcept {
    auto *p = *pending;
    const auto status = mgmem_borrow_release_v1(p->domain, p->view.lease);
    if (status != MGMEM_OK) {
        p->state = mgmem_gpu_ahb::uncertain;
        p->on_error(p->context, status);
        return MGMEM_PROVIDER_ERROR;
    }
    close_fd(p->ready_fd);
    close_fd(p->completion_fd);
    mgc_lease_release(p->lease);
    delete p;
    *pending = nullptr;
    return MGMEM_OK;
}
}

extern "C" mgmem_gpu_ahb_result mgmem_gpu_ahb_begin(mgmem_domain *domain,
    mgmem_handle_v1 allocation, uint64_t offset, uint64_t length,
    MgcLease *lease, mgmem_gpu_ahb_error_fn on_error, void *context) {
    mgmem_gpu_ahb_result result{};
    result.status = MGMEM_INVALID_ARGUMENT;
    result.view.ready_fd = -1;
    if (!domain || !lease || !length || !on_error) return result;
    MgcLeaseInfo info{};
    info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    result.compute_status = mgc_lease_info(lease, &info);
    if (result.compute_status != MGC_SUCCESS) { result.status = translate(result.compute_status); return result; }
    if (info.header.struct_size < sizeof(info) || info.header.abi_major != MGC_ABI_MAJOR ||
        info.access != MGC_ACCESS_WRITE || info.format != MGC_FORMAT_RGBA8 ||
        !info.width || !info.height || !info.capacity_bytes || info.byte_offset ||
        info.byte_length != info.capacity_bytes) { result.status = MGMEM_UNSUPPORTED; return result; }
    AHardwareBuffer *buffer = nullptr;
    result.compute_status = mgc_android_get_buffer(lease, &buffer);
    if (result.compute_status != MGC_SUCCESS || !buffer) {
        result.status = result.compute_status == MGC_SUCCESS ? MGMEM_PROVIDER_ERROR : translate(result.compute_status);
        return result;
    }
    auto *p = new (std::nothrow) mgmem_gpu_ahb{};
    if (!p) { result.status = MGMEM_OUT_OF_MEMORY; return result; }
    p->domain = domain; p->lease = lease; p->on_error = on_error; p->context = context;
    result.status = mgmem_borrow_acquire_v1(domain, allocation, offset, length, MGMEM_READ, &p->view);
    if (result.status != MGMEM_OK) { delete p; return result; }
    mgc_lease_retain(lease);
    result.compute_status = mgc_android_release_to_external(lease, &p->ready_fd);
    if (result.compute_status != MGC_SUCCESS) {
        result.status = translate(result.compute_status);
        if (result.compute_status != MGC_BUSY && result.compute_status != MGC_BACKPRESSURE) {
            // Compute may have issued an ownership transition before a hard
            // failure. Its public API does not prove physical retirement.
            p->state = mgmem_gpu_ahb::uncertain;
            result.pending = p;
        } else if (finish(&p) != MGMEM_OK) {
            result.status = MGMEM_PROVIDER_ERROR;
            result.pending = p;
        }
        return result;
    }
    result.status = MGMEM_OK;
    result.pending = p;
    result.view = {p->view.data, p->view.length, buffer, info.row_stride_bytes, info.width, info.height, p->ready_fd};
    return result;
}

extern "C" mgmem_status mgmem_gpu_ahb_publish(mgmem_gpu_ahb *p, int32_t fd, int32_t *out) {
    if (out) *out = MGC_SUCCESS;
    if (!p || !out || fd < -1 || p->state != mgmem_gpu_ahb::reserved) return MGMEM_INVALID_ARGUMENT;
    const int retained = fd < 0 ? -1 : copy_fd(fd);
    if (fd >= 0 && retained < 0) return MGMEM_PROVIDER_ERROR;
    *out = mgc_android_set_acquire_fence(p->lease, fd);
    if (*out != MGC_SUCCESS) { close_fd(retained); return translate(*out); }
    close_fd(p->ready_fd); p->ready_fd = -1;
    p->completion_fd = retained;
    p->state = mgmem_gpu_ahb::published;
    return MGMEM_OK;
}

/* Private bridge hook: only the optional guarded uploader supplies a Pack
 * registration function. The old SDK path never references a new Pack symbol. */
#if defined(__GNUC__)
#define MGMEM_GUARD_PRIVATE __attribute__((visibility("hidden")))
#else
#define MGMEM_GUARD_PRIVATE
#endif
extern "C" MGMEM_GUARD_PRIVATE mgmem_status mgmem_gpu_ahb_guard_register_internal(mgmem_gpu_ahb *p, int32_t fd,
    int32_t (*register_guard)(MgcLease *, int32_t, uint64_t *), uint64_t *generation, int32_t *out) {
    if (out) *out = MGC_SUCCESS;
    if (generation) *generation = 0;
    if (!p || !out || !generation || !register_guard || fd < 0 || p->state != mgmem_gpu_ahb::reserved)
        return MGMEM_INVALID_ARGUMENT;
    const int retained = copy_fd(fd);
    if (retained < 0) return MGMEM_PROVIDER_ERROR;
    *out = register_guard(p->lease, fd, generation);
    if (*out != MGC_SUCCESS) { close_fd(retained); return translate(*out); }
    close_fd(p->ready_fd); p->ready_fd = -1;
    p->completion_fd = retained;
    p->guarded_source_fd = fd;
    p->state = mgmem_gpu_ahb::published;
    return MGMEM_OK;
}

/* Guarded uploader only: it owns the original fd continuously and has already
 * confirmed its irreversible successful sync-file status, retired the GL/EGL
 * objects, and committed the exact Pack cleanup gate. Its original fd and our
 * retained fd refer to the same file because guarded registration duped it. */
extern "C" MGMEM_GUARD_PRIVATE mgmem_status mgmem_gpu_ahb_guard_retire_verified_internal(
    mgmem_gpu_ahb **pending, int32_t source_fd) {
    if (!pending || !*pending || source_fd < 0) return MGMEM_INVALID_ARGUMENT;
    auto *p = *pending;
    if (p->state != mgmem_gpu_ahb::published || p->guarded_source_fd != source_fd || p->completion_fd < 0)
        return MGMEM_INVALID_ARGUMENT;
    return finish(pending);
}
#undef MGMEM_GUARD_PRIVATE

extern "C" mgmem_status mgmem_gpu_ahb_retire(mgmem_gpu_ahb **pending, int32_t timeout_ms) {
    if (!pending || !*pending || timeout_ms < -1) return MGMEM_INVALID_ARGUMENT;
    auto *p = *pending;
    if (p->state == mgmem_gpu_ahb::uncertain) return MGMEM_PROVIDER_ERROR;
    if (p->state != mgmem_gpu_ahb::published) return MGMEM_BUSY;
    const int status = wait_fd(p->completion_fd, timeout_ms);
    if (status == 1 || status == 2) return MGMEM_TIMEOUT;
    if (status != 0) return MGMEM_PROVIDER_ERROR;
    return finish(pending);
}

extern "C" mgmem_status mgmem_gpu_ahb_cancel(mgmem_gpu_ahb **pending, int32_t *out) {
    if (out) *out = MGC_SUCCESS;
    if (!pending || !*pending || !out) return MGMEM_INVALID_ARGUMENT;
    auto *p = *pending;
    if (p->state == mgmem_gpu_ahb::uncertain) return MGMEM_PROVIDER_ERROR;
    if (p->state != mgmem_gpu_ahb::reserved) return MGMEM_BUSY;
    *out = mgc_android_set_acquire_fence(p->lease, p->ready_fd);
    if (*out != MGC_SUCCESS) return translate(*out);
    return finish(pending);
}
