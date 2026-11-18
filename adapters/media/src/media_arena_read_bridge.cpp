#include <mgmem/media_read_bridge.h>

using ImportRead = mgmem_status (*)(mgmem_domain *, MgcLease *, uint64_t,
    mgmem_compute_unmap_error_v1, void *, mgmem_handle_v1 *, int32_t *);

mgmem_status mgmem_media_import_read_impl(
    mgmem_domain *, MgmFrame *, uint64_t, mgmem_compute_unmap_error_v1,
    void *, mgmem_handle_v1 *, int32_t *, ImportRead);

extern "C" mgmem_status mgmem_media_import_arena_read_v1(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_media_status) {
    return mgmem_media_import_read_impl(domain, frame, external_id, on_unmap_error,
                                        error_context, out, out_media_status, mgmem_compute_import_arena_read_v1);
}
