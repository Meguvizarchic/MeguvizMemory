#ifndef MGMEM_AHB_UPLOAD_PLATFORM_HPP
#define MGMEM_AHB_UPLOAD_PLATFORM_HPP
#include <mgmem/gpu_ahb.h>
#include <cstdint>

struct mgmem_upload_platform;
/* open does not create GPU resources or touch the target. upload may touch the
 * target even when it fails; callers must quarantine every such failure.
 * cleanup is called only after producer fence signal and can be retried only
 * when it returns MGMEM_BUSY; any other failure is uncertain. */
mgmem_status mgmem_upload_platform_open(mgmem_upload_platform **out, int32_t *detail) noexcept;
mgmem_status mgmem_upload_platform_write(mgmem_upload_platform *p, const mgmem_gpu_ahb_view &view,
    uint64_t source_stride, int32_t *detail) noexcept;
mgmem_status mgmem_upload_platform_fence(mgmem_upload_platform *p, int *fd, int32_t *detail) noexcept;
mgmem_status mgmem_upload_platform_cleanup(mgmem_upload_platform *p, int32_t *detail) noexcept;
void mgmem_upload_platform_discard(mgmem_upload_platform *p) noexcept;
/* FD remains owned by the caller. 0 verified success, 1 poll timeout,
 * 2 status query interrupted through deadline, <0 error/unknown. */
int mgmem_upload_platform_wait(int fd, int timeout_ms) noexcept;
/* Non-blocking status inspection for an owned, stable sync-file fd. Returns
 * 0 only for confirmed success, 1 for active, 2 for an interrupted query,
 * -2 for a confirmed producer failure, and -1 for invalid/unverifiable status. */
int mgmem_upload_platform_probe(int fd) noexcept;
void mgmem_upload_platform_close_fd(int fd) noexcept;
#endif
