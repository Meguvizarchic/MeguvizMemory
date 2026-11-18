#ifndef MGMEM_AHB_UPLOAD_GUARDED_H
#define MGMEM_AHB_UPLOAD_GUARDED_H

#include <mgmem/ahb_upload.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MgcDomain MgcDomain;

/* Optional target MeguvizMemoryAhbGlesUpload::guarded; requires the newer
 * Compute guarded-producer SDK. A success registers the producer fence with
 * Compute while keeping its cleanup gate closed. Callers may then publish a
 * READ lease and enqueue downstream work, but must still call upload_wait on
 * the original GL thread to verify the producer, clean GL/EGL, and open the
 * exact generation's gate. A failed or unconfirmed cleanup never opens it.
 *
 * The domain and lease must belong to the same loaded Compute runtime. This
 * function checks the domain id/generation and capability before registration.
 * Unsupported capability or mismatched domain leaves the original safe path
 * intact. The caller serializes all token operations. */
mgmem_status mgmem_ahb_queue(mgmem_ahb_upload *pending, MgcDomain *domain, int32_t *compute_status);

#ifdef __cplusplus
}
namespace mgmem::ahb {
[[nodiscard]] inline mgmem_status queue(mgmem_ahb_upload *pending, MgcDomain *domain,
                                        int32_t *compute_status) noexcept {
    return mgmem_ahb_queue(pending, domain, compute_status);
}
}
#endif
#endif
