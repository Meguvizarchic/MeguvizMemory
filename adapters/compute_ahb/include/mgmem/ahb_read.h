#ifndef MGMEM_AHB_READ_H
#define MGMEM_AHB_READ_H

#include <mgmem/mgmem.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MgcLease MgcLease;
typedef struct mgmem_ahb_recovery mgmem_ahb_recovery;

enum {
    MGMEM_AHB_STAGE_NONE = 0,
    MGMEM_AHB_STAGE_COMPUTE = 1,
    MGMEM_AHB_STAGE_WAIT = 2,
    MGMEM_AHB_STAGE_LOCK = 3,
    MGMEM_AHB_STAGE_UNLOCK = 4,
    MGMEM_AHB_STAGE_RETURN = 5
};

/* Called when a cleanup operation cannot be confirmed. The callback must be
 * thread-safe, must not throw or reenter the Memory domain, and its context
 * must outlive the owner and all its borrows. */
typedef void (*mgmem_ahb_error_fn)(void *context, int32_t stage, int32_t status);

typedef struct mgmem_ahb_layout {
    uint64_t cpu_bytes;       /* accessible row-stride extent, charged by Memory */
    uint64_t compute_bytes;   /* Compute backing capacity, not an additive charge */
    uint64_t row_stride;
    uint32_t width;
    uint32_t height;
} mgmem_ahb_layout;

/* Map a complete, ready Compute AHB READ lease as a CPU READ external backing
 * in a Memory SYSTEM domain. The caller holds lease through return. This call
 * can wait synchronously for the fence returned by the external reservation
 * for timeout_ms (-1 means no deadline). An initial unsignaled Compute lease
 * acquire dependency can instead make reservation return BUSY immediately;
 * wait on the lease or retry separately. The caller supplies a unique
 * external_id for the entire CPU map.
 *
 * The CPU-visible row extent includes per-row padding and may be smaller than
 * Compute's allocation size. Padding content is not initialized by this API.
 * The caller must coordinate physical aliases across Memory runtimes and must
 * not access the AHB through other mappings while this import lives.
 *
 * On success, the owner and all Memory borrows retain the AHB lock, Compute
 * lease and external-access reservation. The final release synchronously
 * unlocks before returning the reservation. A cleanup failure poisons the
 * Memory domain through checked release and retains uncertain resources.
 *
 * On failure before Memory admits the backing, recovery is populated if the
 * external reservation could not be returned, or if a producer fence failed
 * or could not be inspected. The caller must keep it and retry
 * mgmem_ahb_recover() only when cleanup is retryable. A failed/uninspectable
 * fence or an unlock failure is uncertain and remains quarantined. On all
 * other returns recovery is NULL.
 */
mgmem_status mgmem_ahb_read(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context,
    mgmem_handle_v1 *out, mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery,
    int32_t *error_stage, int32_t *error_status);

/* Same ownership, wait, cleanup and recovery contract as mgmem_ahb_read,
 * for a complete Compute WRITE lease and a Memory READ/WRITE backing.
 * The AHB must permit both CPU READ and WRITE: Memory WRITE owners may also
 * issue READ borrows. The caller must finish and release every Memory borrow
 * and owner before publishing or otherwise using the Compute lease. Neither
 * this function nor a failed import initializes the buffer's contents. */
mgmem_status mgmem_ahb_write(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context,
    mgmem_handle_v1 *out, mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery,
    int32_t *error_stage, int32_t *error_status);

/* Retry returning a pre-admission reservation. Success consumes *recovery.
 * The caller serializes access to each recovery handle and keeps the callback
 * and its context alive until recovery succeeds. An unlock or failed fence
 * error is never retried because physical state is unknown. */
mgmem_status mgmem_ahb_recover(mgmem_ahb_recovery **recovery);

#ifdef __cplusplus
}
#endif
#endif
