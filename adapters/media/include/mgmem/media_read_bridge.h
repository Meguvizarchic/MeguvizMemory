#ifndef MGMEM_MEDIA_READ_BRIDGE_H
#define MGMEM_MEDIA_READ_BRIDGE_H

#include <mgmem/compute_read_bridge.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MgmFrame MgmFrame;

/* The caller owns a live frame reference through the call. The adapter keeps
 * the frame alive while reading its borrowed content lease. On success the
 * existing Compute bridge owns the mapping and its own lease reference; the
 * frame may then be released independently. The complete READ lease and all
 * identity, alias, callback, error and runtime contracts of the Compute bridge
 * apply unchanged. The raw pointer never escapes the Memory lease. */
mgmem_status mgmem_media_import_read_v1(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_media_status);

/* Arena variant lives in a separate archive and requires the Arena external
 * import capability and matching Compute arena bridge. */
mgmem_status mgmem_media_import_arena_read_v1(
    mgmem_domain *domain, MgmFrame *frame, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_media_status);

#ifdef __cplusplus
}
#endif
#endif
