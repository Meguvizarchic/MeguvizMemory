#ifndef MGMEM_AHB_MAP_H
#define MGMEM_AHB_MAP_H

#include <mgmem/ahb_read.h>

/* The owner and any recovery token belong to the caller. On failure the
 * recovery token may be non-null and must be preserved; do not copy it into
 * independent recovery owners. error_stage and error_status carry the same
 * synchronous failure information as mgmem_ahb_read. */
typedef struct mgmem_ahb_result {
    mgmem_status status;
    mgmem_handle_v1 owner;
    mgmem_ahb_layout layout;
    mgmem_ahb_recovery *recovery;
    int32_t error_stage;
    int32_t error_status;
} mgmem_ahb_result;

static inline mgmem_ahb_result mgmem_ahb_map(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context) {
    mgmem_ahb_result result = {MGMEM_OK, {0, 0, 0}, {0, 0, 0, 0, 0}, 0, MGMEM_AHB_STAGE_NONE, 0};
    result.status = mgmem_ahb_read(domain, lease, external_id, timeout_ms, on_error, error_context,
        &result.owner, &result.layout, &result.recovery, &result.error_stage, &result.error_status);
    return result;
}

static inline mgmem_ahb_result mgmem_ahb_map_write(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context) {
    mgmem_ahb_result result = {MGMEM_OK, {0, 0, 0}, {0, 0, 0, 0, 0}, 0, MGMEM_AHB_STAGE_NONE, 0};
    result.status = mgmem_ahb_write(domain, lease, external_id, timeout_ms, on_error, error_context,
        &result.owner, &result.layout, &result.recovery, &result.error_stage, &result.error_status);
    return result;
}

#ifdef __cplusplus
namespace mgmem::ahb {
using map_result = ::mgmem_ahb_result;

[[nodiscard]] inline map_result read(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context) noexcept {
    return mgmem_ahb_map(domain, lease, external_id, timeout_ms, on_error, error_context);
}
[[nodiscard]] inline map_result write(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context) noexcept {
    return mgmem_ahb_map_write(domain, lease, external_id, timeout_ms, on_error, error_context);
}
} // namespace mgmem::ahb
#endif

#endif
