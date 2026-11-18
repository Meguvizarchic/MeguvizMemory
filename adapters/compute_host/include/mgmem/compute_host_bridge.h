#ifndef MGMEM_COMPUTE_HOST_BRIDGE_H
#define MGMEM_COMPUTE_HOST_BRIDGE_H

#include <mgmem/mgmem.h>
#include <meguviz/compute.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called if Memory rejects the final borrow release. The callback must be
 * thread-safe, non-throwing, must not reenter the same domain, and its context
 * must remain alive until the Compute resource and its leases are retired. */
typedef void (*mgmem_compute_host_error_fn)(void *context, mgmem_status status);

/* Create a Compute host resource over an existing Memory allocation. A READ or
 * WRITE borrow is held until the last Compute resource/lease/task is retired.
 * desc must describe host storage with capacity_bytes equal to the requested
 * Memory range length. The caller retains domain until final resource release.
 * No pointer may be used outside the lease contract, or by another runtime.
 * On a failed final release the bridge retains its context and reports through
 * on_error. No retry or claim of successful cleanup is made. */
mgmem_status mgmem_compute_create_host_resource_v1(
    mgmem_domain *domain, mgmem_handle_v1 allocation, uint64_t offset,
    MgcDomain *compute, const MgcResourceDesc *desc, uint32_t access,
    mgmem_compute_host_error_fn on_error, void *error_context,
    MgcResource **out, int32_t *out_compute_status);

#ifdef __cplusplus
}
#endif
#endif
