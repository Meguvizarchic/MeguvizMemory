#ifndef MGMEM_MEDIA_READ_H
#define MGMEM_MEDIA_READ_H

#include <mgmem/compute_read.h>
#include <mgmem/media_read_bridge.h>

/* The result, frame lifetime, identity and callback contracts are the same as
 * mgmem_media_import_read_v1. The returned owner belongs to the caller. */
static inline mgmem_read_result mgmem_media_read(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_error, void *error_context) {
    mgmem_read_result result = {MGMEM_OK, {0, 0, 0}, 0};
    result.status = mgmem_media_import_read_v1(domain, frame, external_id, on_error,
                                               error_context, &result.owner, &result.origin_status);
    return result;
}

/* Link the optional arena_read_bridge target when this entry point is used. */
static inline mgmem_read_result mgmem_media_read_arena(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_error, void *error_context) {
    mgmem_read_result result = {MGMEM_OK, {0, 0, 0}, 0};
    result.status = mgmem_media_import_arena_read_v1(domain, frame, external_id, on_error,
                                                     error_context, &result.owner, &result.origin_status);
    return result;
}

#ifdef __cplusplus
namespace mgmem::media {
using read_result = ::mgmem_read_result;

[[nodiscard]] inline read_result read(mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
                                      mgmem_compute_unmap_error_v1 on_error, void *error_context) noexcept {
    return mgmem_media_read(domain, frame, external_id, on_error, error_context);
}

[[nodiscard]] inline read_result read_arena(mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
                                            mgmem_compute_unmap_error_v1 on_error, void *error_context) noexcept {
    return mgmem_media_read_arena(domain, frame, external_id, on_error, error_context);
}
} // namespace mgmem::media
#endif

#endif
