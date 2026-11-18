#include <mgmem/media_read_bridge.h>
#include <meguviz/media.h>

using ImportRead = mgmem_status (*)(mgmem_domain *, MgcLease *, uint64_t,
    mgmem_compute_unmap_error_v1, void *, mgmem_handle_v1 *, int32_t *);

mgmem_status mgmem_media_import_read_impl(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_media_status, ImportRead import_read) {
    if (out_media_status) *out_media_status = MGC_SUCCESS;
    if (out) *out = {};
    if (!domain || !frame || !external_id || !on_unmap_error || !out || !out_media_status)
        return MGMEM_INVALID_ARGUMENT;

    mgm_frame_retain(frame);
    MgmFrameDesc desc{};
    desc.header = MGC_HEADER_INIT(MgmFrameDesc);
    const MgcStatus status = mgm_frame_describe(frame, &desc);
    if (status != MGC_SUCCESS) {
        *out_media_status = status;
        mgm_frame_release(frame);
        return MGMEM_PROVIDER_ERROR;
    }
    const auto result = import_read(domain, desc.content, external_id, on_unmap_error,
                                    error_context, out, out_media_status);
    mgm_frame_release(frame);
    return result;
}

extern "C" mgmem_status mgmem_media_import_read_v1(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_media_status) {
    return mgmem_media_import_read_impl(domain, frame, external_id, on_unmap_error,
                                        error_context, out, out_media_status, mgmem_compute_import_read_v1);
}
