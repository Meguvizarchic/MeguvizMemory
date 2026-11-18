#ifndef MGMEM_COMPUTE_HOST_H
#define MGMEM_COMPUTE_HOST_H

#include <mgmem/compute_host_bridge.h>

/* On success resource owns a Compute reference and must be released with
 * mgc_resource_release. Pass MGC_ACCESS_READ or MGC_ACCESS_WRITE as access.
 * On failure resource is null. origin_status is the
 * synchronous Compute status. The error callback remains responsible for
 * failures that occur when the final Memory borrow is returned. */
typedef struct mgmem_host_result {
    mgmem_status status;
    MgcResource *resource;
    int32_t origin_status;
} mgmem_host_result;

static inline mgmem_host_result mgmem_compute_host(
    mgmem_domain *domain, mgmem_handle_v1 allocation, uint64_t offset,
    MgcDomain *compute, const MgcResourceDesc *desc, uint32_t access,
    mgmem_compute_host_error_fn on_error, void *error_context) {
    mgmem_host_result result = {MGMEM_OK, 0, 0};
    result.status = mgmem_compute_create_host_resource_v1(domain, allocation, offset, compute,
        desc, access, on_error, error_context, &result.resource, &result.origin_status);
    return result;
}

#ifdef __cplusplus
namespace mgmem::compute {
using host_result = ::mgmem_host_result;

[[nodiscard]] inline host_result host(
    mgmem_domain *domain, mgmem_handle_v1 allocation, uint64_t offset,
    MgcDomain *compute_domain, const MgcResourceDesc *desc, uint32_t access,
    mgmem_compute_host_error_fn on_error, void *error_context) noexcept {
    return mgmem_compute_host(domain, allocation, offset, compute_domain, desc, access, on_error, error_context);
}
} // namespace mgmem::compute
#endif

#endif
