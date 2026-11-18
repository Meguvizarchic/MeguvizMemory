#ifndef MGMEM_COMPUTE_READ_BRIDGE_H
#define MGMEM_COMPUTE_READ_BRIDGE_H

#include <mgmem/mgmem.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional adapter: clients must link the same Compute runtime that owns this
 * lease and the Memory runtime that owns domain. This header does not add a
 * Compute dependency to the Memory core. */
typedef struct MgcLease MgcLease;

/* Synchronous admission failed and the subsequent foreign unmap failed too.
 * The adapter has retained an uncertain foreign mapping, but the Memory core
 * may never have accepted it and thus has no claim over the region. */
#define MGMEM_COMPUTE_ROLLBACK_QUARANTINED ((mgmem_status)0x80000001u)

/* Called on the thread releasing the final Memory lease if Compute reports an
 * unmap failure. It must be thread-safe, must not throw or reenter the Memory
 * domain, and its context must remain alive until the imported owner and all
 * leases have ended. The bridge independently retains the origin lease before
 * mapping. On failure it preserves the pin if its ownership is known. Foreign
 * C ABI functions and callbacks must not throw; violating this precondition is
 * not a recoverable or supported execution path. */
typedef void (*mgmem_compute_unmap_error_v1)(void *context, int32_t status);

/* Import one entire, ready, host-backed Compute READ lease into a SYSTEM domain.
 * external_id must uniquely and stably identify the complete origin backing in
 * this Memory runtime, including across all other external import adapters. It
 * is NOT a hash of the Compute identity tuple. The caller must coordinate IDs
 * across all adapters and Memory runtimes: the same physical backing must have
 * exactly one managing Memory runtime, and aliased virtual mappings cannot be
 * detected. The caller retains its lease through this call and must not unmap
 * the adapter's mapping, mutate through aliases, or unload either runtime or
 * callback code while the imported owner/Memory leases exist.
 *
 * On success out contains one ordinary Memory allocation owner; Memory retains
 * the Compute map through the last lease, including across begin_close. On
 * failure out is zero. Required out_compute_status is set to the last
 * synchronous Compute error (0 if the failure originated in Memory).
 * on_unmap_error is also required. A later unmap failure
 * is reported through on_unmap_error; checked release poisons
 * the domain and preserves its charge and registry claim. If unmap instead
 * fails while rolling back an import rejected before Memory retained it, this
 * returns MGMEM_COMPUTE_ROLLBACK_QUARANTINED; no Memory claim exists to retain.
 * The caller must stop using that backing and coordinate all subsequent
 * imports. Compute normally permits this failure
 * only after a contract violation such as an external unmap.
 * A successful foreign unmap can recycle the storage before the callback
 * returns while its Memory claim remains live. A concurrent provider reusing
 * that address may conservatively be rejected or isolated as an overlap;
 * checked release has no two-phase handoff with the foreign allocator.
 */
mgmem_status mgmem_compute_import_read_v1(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_compute_status);

/* Arena-domain variant of the same full, ready CPU READ import contract.
 * Link the separate arena_read_bridge target and a Memory runtime exporting
 * mgmem_import_external_arena_checked_v1 with both advertised capabilities.
 * Imported backing is charged to the arena domain but does not supply arena
 * storage. The SYSTEM-only function above retains its original behavior. */
mgmem_status mgmem_compute_import_arena_read_v1(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_compute_status);

#ifdef __cplusplus
}
#endif
#endif
