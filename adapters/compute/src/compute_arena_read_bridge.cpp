#include <mgmem/compute_read_bridge.h>

using ImportChecked = mgmem_status (*)(mgmem_domain *, const mgmem_external_buffer_checked_v1 *, mgmem_handle_v1 *);

mgmem_status mgmem_compute_import_read_impl(
    mgmem_domain *, MgcLease *, uint64_t, mgmem_compute_unmap_error_v1, void *,
    mgmem_handle_v1 *, int32_t *, ImportChecked);

extern "C" mgmem_status mgmem_compute_import_arena_read_v1(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_compute_status) {
    if (out_compute_status) *out_compute_status = 0;
    if (out) *out = {};
    if (!out || !domain || !lease || !external_id || !on_unmap_error || !out_compute_status)
        return MGMEM_INVALID_ARGUMENT;
    const uint64_t required = MGMEM_CAP_CHECKED_EXTERNAL_RELEASE | MGMEM_CAP_ARENA_EXTERNAL_IMPORT;
    if ((mgmem_capabilities_v1() & required) != required) return MGMEM_UNSUPPORTED;
    return mgmem_compute_import_read_impl(domain, lease, external_id, on_unmap_error, error_context,
                                          out, out_compute_status, mgmem_import_external_arena_checked_v1);
}
