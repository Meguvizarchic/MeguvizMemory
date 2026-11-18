#ifndef MGMEM_AHB_UPLOAD_H
#define MGMEM_AHB_UPLOAD_H

#include <mgmem/gpu_ahb.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mgmem_ahb_upload mgmem_ahb_upload;
enum {
    MGMEM_UPLOAD_NONE = 0,
    MGMEM_UPLOAD_CANCEL = 1,
    MGMEM_UPLOAD_PUBLISH = 2,
    MGMEM_UPLOAD_RETIRE = 3,
    MGMEM_UPLOAD_QUARANTINED = 4,
    MGMEM_UPLOAD_PRODUCING = 5,
    /* Guarded queue only: a confirmed failure has not yet been delivered to
     * Compute because its guard is busy; retry reports notification status. */
    MGMEM_UPLOAD_FAIL_NOTIFY = 6
};

/* Fixed v1 ABI layout: do not extend this structure; future descriptors use a
 * new type and entry point. RGBA8, full-frame rows in GL texture coordinate
 * order. source_stride is the Memory source row pitch, independent of the
 * destination AHB stride. */
typedef struct mgmem_ahb_upload_desc {
    mgmem_domain *memory;
    mgmem_handle_v1 allocation;
    uint64_t offset, length, source_stride;
    uint32_t width, height;
    MgcLease *write_lease;
    int32_t ready_timeout_ms;
    mgmem_gpu_ahb_error_fn on_error;
    void *error_context;
} mgmem_ahb_upload_desc;

typedef struct mgmem_ahb_upload_result {
    mgmem_status status;
    int32_t compute_status;
    int32_t platform_status;
    uint32_t phase;
    mgmem_ahb_upload *pending;
} mgmem_ahb_upload_result;

/* Requires a current GLES3 EGL context with the advertised image and native
 * fence extensions. Waits synchronously for ready_timeout_ms before touching
 * the destination; then submits a full-frame GPU upload. wait confirms the
 * producer sync-file succeeded, cleans up GL/EGL objects on the owning thread,
 * and only then publishes the completed write to Compute. This can
 * prevent downstream work from being enqueued until the producer completes.
 * The caller owns every non-null pending result, including
 * failures. The same EGL context must remain current on the original thread
 * during submit and retirement. No AHB CPU mapping is performed.
 *
 * CANCEL permits retrying cancellation of work never submitted; PUBLISH holds
 * a verified producer fence and permits retry; FAIL_NOTIFY retries delivery
 * of a guarded failure; PRODUCING and RETIRE permit
 * wait; a
 * QUARANTINED token and its source/domain/context must remain alive. The
 * caller must serialize all operations on the same pending token and retain
 * the Memory domain, error callback/context and EGL context until retirement. */
mgmem_ahb_upload_result mgmem_ahb_upload_submit(const mgmem_ahb_upload_desc *desc);
mgmem_status mgmem_ahb_upload_retry(mgmem_ahb_upload *pending, int32_t *compute_status);
mgmem_status mgmem_ahb_upload_wait(mgmem_ahb_upload **pending, int32_t timeout_ms);
mgmem_status mgmem_ahb_upload_cancel(mgmem_ahb_upload **pending, int32_t *compute_status);
/* Serialise this query with all other token operations. */
uint32_t mgmem_ahb_upload_phase(const mgmem_ahb_upload *pending);

#ifdef __cplusplus
}
namespace mgmem::ahb {
using upload_desc = ::mgmem_ahb_upload_desc;
using upload_result = ::mgmem_ahb_upload_result;
[[nodiscard]] inline upload_result upload(const upload_desc &desc) noexcept {
    return mgmem_ahb_upload_submit(&desc);
}
inline mgmem_status retry(mgmem_ahb_upload *value, int32_t *status) noexcept {
    return mgmem_ahb_upload_retry(value, status);
}
inline mgmem_status wait(mgmem_ahb_upload **value, int32_t timeout_ms) noexcept {
    return mgmem_ahb_upload_wait(value, timeout_ms);
}
inline mgmem_status cancel(mgmem_ahb_upload **value, int32_t *status) noexcept {
    return mgmem_ahb_upload_cancel(value, status);
}
inline uint32_t phase(const mgmem_ahb_upload *value) noexcept {
    return mgmem_ahb_upload_phase(value);
}
}
#endif
#endif
