#ifndef MGMEM_AHB_SLOT_H
#define MGMEM_AHB_SLOT_H

#include <mgmem/ahb_map.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mgmem_ahb_slot mgmem_ahb_slot;

/* A slot reuses only the bridge's small state object. Every map still reserves
 * the Compute lease, waits its ready fence, locks the AHB and imports it into
 * Memory; every final release still synchronously unlocks and returns the
 * reservation. Slot APIs must be serialized by the caller and a slot must not
 * be used concurrently with close. Callback-driven final release may occur on
 * another thread. A successful close can race the final release callback's
 * return; before destroying the domain, callback context or bridge code, the
 * caller must also join or otherwise await that releasing API call. The domain,
 * callback and context must stay alive until the final release or until any
 * recovery token has been resolved.
 *
 * The slot remains BUSY while an owner, borrow or recovery is outstanding. A
 * checked release whose cleanup is unconfirmed permanently quarantines the
 * slot: close returns BUSY and the caller must retain the slot and its owners. */
mgmem_status mgmem_ahb_slot_create(mgmem_ahb_slot **out);
mgmem_status mgmem_ahb_slot_close(mgmem_ahb_slot **slot);

/* Prefer mgmem_ahb_slot_read/write. Error, timeout, recovery, external ID,
 * access and physical retirement contracts are identical to the existing
 * mgmem_ahb_read/write APIs. write is 0 for READ or 1 for READ/WRITE. */
mgmem_status mgmem_ahb_slot_map_v1(mgmem_ahb_slot *slot, mgmem_domain *domain,
    MgcLease *lease, uint64_t external_id, int32_t timeout_ms, mgmem_ahb_error_fn on_error,
    void *error_context, mgmem_handle_v1 *out, mgmem_ahb_layout *layout,
    mgmem_ahb_recovery **recovery, int32_t *error_stage, int32_t *error_status, uint32_t write);

static inline mgmem_ahb_result mgmem_ahb_slot_read(mgmem_ahb_slot *slot, mgmem_domain *domain,
    MgcLease *lease, uint64_t id, int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *context) {
    mgmem_ahb_result result = {MGMEM_OK, {0, 0, 0}, {0, 0, 0, 0, 0}, 0, MGMEM_AHB_STAGE_NONE, 0};
    result.status = mgmem_ahb_slot_map_v1(slot, domain, lease, id, timeout_ms, on_error, context,
        &result.owner, &result.layout, &result.recovery, &result.error_stage, &result.error_status, 0);
    return result;
}

static inline mgmem_ahb_result mgmem_ahb_slot_write(mgmem_ahb_slot *slot, mgmem_domain *domain,
    MgcLease *lease, uint64_t id, int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *context) {
    mgmem_ahb_result result = {MGMEM_OK, {0, 0, 0}, {0, 0, 0, 0, 0}, 0, MGMEM_AHB_STAGE_NONE, 0};
    result.status = mgmem_ahb_slot_map_v1(slot, domain, lease, id, timeout_ms, on_error, context,
        &result.owner, &result.layout, &result.recovery, &result.error_stage, &result.error_status, 1);
    return result;
}

#ifdef __cplusplus
}
namespace mgmem::ahb {
using slot = ::mgmem_ahb_slot;
[[nodiscard]] inline map_result read(slot *s, mgmem_domain *d, MgcLease *l, uint64_t id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *context) noexcept {
    return mgmem_ahb_slot_read(s, d, l, id, timeout_ms, on_error, context);
}
[[nodiscard]] inline map_result write(slot *s, mgmem_domain *d, MgcLease *l, uint64_t id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *context) noexcept {
    return mgmem_ahb_slot_write(s, d, l, id, timeout_ms, on_error, context);
}
} // namespace mgmem::ahb
#endif
#endif
