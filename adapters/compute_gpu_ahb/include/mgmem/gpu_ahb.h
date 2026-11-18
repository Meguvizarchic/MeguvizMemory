#ifndef MGMEM_GPU_AHB_H
#define MGMEM_GPU_AHB_H

#include <mgmem/mgmem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MgcLease MgcLease;
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct mgmem_gpu_ahb mgmem_gpu_ahb;
typedef void (*mgmem_gpu_ahb_error_fn)(void *context, mgmem_status status);

typedef struct mgmem_gpu_ahb_view {
    const void *data;
    uint64_t length;
    AHardwareBuffer *buffer;
    uint64_t row_stride;
    uint32_t width, height;
    int32_t ready_fd; /* borrowed until successful publish/cancel; duplicate before consuming import */
} mgmem_gpu_ahb_view;

typedef struct mgmem_gpu_ahb_result {
    mgmem_status status;
    int32_t compute_status;
    mgmem_gpu_ahb *pending;
    mgmem_gpu_ahb_view view;
} mgmem_gpu_ahb_result;

/* The caller keeps domain, callback and callback context alive through retire.
 * The callback is thread-safe, must not throw, and must not reenter domain.
 * The caller holds the original lease and allocation throughout this call.
 * Success reserves a complete Compute WRITE AHB and pins a Memory READ borrow.
 * A hard reservation failure may have issued an unretired ownership transition.
 * Such a failure returns a quarantined pending token, which cannot be retired
 * or canceled by this API; preserve it and the affected domain/backing. The producer is
 * responsible for importing/waiting ready_fd and for GPU synchronization and
 * FOREIGN/GENERAL ownership transitions. This API does not upload pixels. */
mgmem_gpu_ahb_result mgmem_gpu_ahb_begin(mgmem_domain *domain,
    mgmem_handle_v1 allocation, uint64_t offset, uint64_t length,
    MgcLease *write_lease, mgmem_gpu_ahb_error_fn on_error, void *context);

/* After submitting producer GPU work, pass its borrowed sync-file fence. The
 * producer MUST depend on ready_fd and return the image to FOREIGN/GENERAL.
 * A -1 fence asserts that all producer work has physically completed. This
 * low-level entry may publish before that fence signals: callers must ensure
 * downstream Compute consumers can safely handle producer errors, or first
 * verify successful fence completion before publishing. Failure keeps the
 * reservation and borrow; retry with the original valid fence. On
 * success the caller may close its fence; this token retains its own copy.
 * The caller must keep the descriptor valid and prevent its concurrent close
 * for the entire publish call.
 * Never call cancel after issuing any GPU work. Serialize token operations. */
mgmem_status mgmem_gpu_ahb_publish(mgmem_gpu_ahb *pending, int32_t producer_fd,
    int32_t *compute_status);

/* Poll/wait and verify successful sync-file status before releasing the Memory
 * borrow. Timeout, a failed fence, or unavailable/unconfirmed status leaves
 * *pending intact; success consumes it. A failed final Memory release
 * quarantines the token and calls on_error. */
mgmem_status mgmem_gpu_ahb_retire(mgmem_gpu_ahb **pending, int32_t timeout_ms);

/* Only before ANY GPU work is issued: return the original ready dependency to
 * Compute and release Memory. Failure keeps *pending for retry. */
mgmem_status mgmem_gpu_ahb_cancel(mgmem_gpu_ahb **pending, int32_t *compute_status);

#ifdef __cplusplus
}
namespace mgmem::gpu_ahb {
using result = ::mgmem_gpu_ahb_result;
[[nodiscard]] inline result begin(mgmem_domain *domain, mgmem_handle_v1 allocation,
    uint64_t offset, uint64_t length, MgcLease *lease,
    mgmem_gpu_ahb_error_fn on_error, void *context) noexcept {
    return mgmem_gpu_ahb_begin(domain, allocation, offset, length, lease, on_error, context);
}
inline mgmem_status publish(mgmem_gpu_ahb *pending, int32_t fd, int32_t *status) noexcept {
    return mgmem_gpu_ahb_publish(pending, fd, status);
}
inline mgmem_status retire(mgmem_gpu_ahb **pending, int32_t timeout_ms) noexcept {
    return mgmem_gpu_ahb_retire(pending, timeout_ms);
}
inline mgmem_status cancel(mgmem_gpu_ahb **pending, int32_t *status) noexcept {
    return mgmem_gpu_ahb_cancel(pending, status);
}
}
#endif
#endif
