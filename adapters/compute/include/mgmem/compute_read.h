#ifndef MGMEM_COMPUTE_READ_H
#define MGMEM_COMPUTE_READ_H

#include <mgmem/compute_read_bridge.h>

/* The owner is an ordinary Memory handle. Its lifetime and the error callback
 * contract are identical to the corresponding bridge function. A failed
 * import always returns a zero owner. origin_status records the synchronous
 * Compute error and does not replace later unmap error notifications. */
typedef struct mgmem_read_result {
    mgmem_status status;
    mgmem_handle_v1 owner;
    int32_t origin_status;
} mgmem_read_result;

/* Import a complete, ready Compute READ lease into a SYSTEM domain. */
static inline mgmem_read_result mgmem_compute_read(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_error, void *error_context) {
    mgmem_read_result result = {MGMEM_OK, {0, 0, 0}, 0};
    result.status = mgmem_compute_import_read_v1(domain, lease, external_id, on_error,
                                                 error_context, &result.owner, &result.origin_status);
    return result;
}

/* Link the optional arena_read_bridge target when this entry point is used. */
static inline mgmem_read_result mgmem_compute_read_arena(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_error, void *error_context) {
    mgmem_read_result result = {MGMEM_OK, {0, 0, 0}, 0};
    result.status = mgmem_compute_import_arena_read_v1(domain, lease, external_id, on_error,
                                                       error_context, &result.owner, &result.origin_status);
    return result;
}

#ifdef __cplusplus
namespace mgmem::compute {
using read_result = ::mgmem_read_result;

[[nodiscard]] inline read_result read(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
                                      mgmem_compute_unmap_error_v1 on_error, void *error_context) noexcept {
    return mgmem_compute_read(domain, lease, external_id, on_error, error_context);
}

[[nodiscard]] inline read_result read_arena(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
                                            mgmem_compute_unmap_error_v1 on_error, void *error_context) noexcept {
    return mgmem_compute_read_arena(domain, lease, external_id, on_error, error_context);
}
} // namespace mgmem::compute
#endif

#endif
