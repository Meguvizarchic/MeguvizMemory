#ifndef MGMEM_H
#define MGMEM_H

#include <stdint.h>

#if defined(_WIN32) && defined(MGMEM_SHARED)
# if defined(MGMEM_BUILDING)
#  define MGMEM_API __declspec(dllexport)
# else
#  define MGMEM_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define MGMEM_API __attribute__((visibility("default")))
#else
# define MGMEM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MGMEM_ABI_MAJOR 1u
#define MGMEM_ABI_MINOR 1u
#define MGMEM_SYSTEM 0u
#define MGMEM_ARENA 1u
#define MGMEM_OPEN 0u
#define MGMEM_CLOSING 1u
#define MGMEM_CLOSED 2u
#define MGMEM_READ 0u
#define MGMEM_WRITE 1u
#define MGMEM_CAP_SYSTEM 1ull
#define MGMEM_CAP_ARENA 2ull
#define MGMEM_CAP_CUSTOM_PROVIDER 4ull
#define MGMEM_CAP_RW_LEASE 8ull
#define MGMEM_CAP_EXPLICIT_COLLECTION 16ull
#define MGMEM_CAP_EXTERNAL_IMPORT 32ull
#define MGMEM_CAP_SOFT_BUDGET 64ull
/* Optional additive extension; availability is not implied by ABI minor 1. */
#define MGMEM_CAP_CHECKED_EXTERNAL_RELEASE 128ull
/* Optional additive extension; resolve both arena import symbols separately. */
#define MGMEM_CAP_ARENA_EXTERNAL_IMPORT 256ull

typedef uint32_t mgmem_status;
#define MGMEM_OK 0u
#define MGMEM_INVALID_ARGUMENT 1u
#define MGMEM_OVERFLOW 2u
#define MGMEM_OUT_OF_MEMORY 3u
#define MGMEM_OUT_OF_BUDGET 4u
#define MGMEM_STALE_HANDLE 5u
#define MGMEM_BUSY 6u
#define MGMEM_IS_CLOSED 7u
#define MGMEM_UNSUPPORTED 8u
#define MGMEM_PROVIDER_ERROR 9u
#define MGMEM_TIMEOUT 10u

typedef struct mgmem_domain mgmem_domain;
/* IDs never reuse an address or a slot. Handles belong to exactly one domain. */
typedef struct mgmem_handle_v1 {
    uint64_t domain_id;
    uint64_t id;
    uint64_t runtime_origin; /* live-module routing cookie, never an allocation identity */
} mgmem_handle_v1;

/* Callbacks must not throw or reenter the same domain. allocate is failure atomic,
 * returns aligned, uninitialized bytes, and leaves *out NULL on failure.
 * retain/release keep the context alive. All callbacks execute outside core locks
 * and must be thread-safe, including contexts shared between domains.
 * The core copies this table; context remains owned by the provider. */
typedef struct mgmem_provider_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t required_flags;
    void *context;
    mgmem_status (*allocate)(void *context, uint64_t bytes, uint64_t alignment, void **out);
    void (*deallocate)(void *context, void *address, uint64_t bytes, uint64_t alignment);
    void (*retain)(void *context);
    void (*release)(void *context);
} mgmem_provider_v1;

typedef struct mgmem_domain_options_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t required_flags;
    uint32_t mode;
    uint32_t reserved;
    uint64_t budget_bytes;
    uint64_t arena_chunk_bytes;
    const mgmem_provider_v1 *provider; /* NULL selects the System provider. */
} mgmem_domain_options_v1;

typedef struct mgmem_borrow_v1 {
    mgmem_handle_v1 lease;
    void *data;
    uint64_t length;
} mgmem_borrow_v1;

typedef struct mgmem_snapshot_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t state;
    uint64_t domain_id;
    uint64_t generation;
    uint64_t budget_bytes;
    uint64_t backing_bytes; /* unique managed capacity: owned + imported; excludes metadata/RSS */
    uint64_t reserved_bytes; /* pending provider calls, included in the budget check */
    uint64_t peak_backing_bytes; /* committed + pending high water mark */
    uint64_t live_bytes; /* owner-held payload, even when also borrowed */
    uint64_t retiring_bytes; /* released payload still held by a lease */
    uint64_t retained_bytes; /* backing - live - retiring; arena needs reset for reuse */
    uint64_t allocation_count; /* successful logical allocations over domain lifetime */
    uint64_t provider_allocation_count;
    uint64_t cumulative_requested_bytes;
    uint64_t active_allocations;
    uint64_t active_leases;
    uint64_t page_size;
} mgmem_snapshot_v1;

/* ABI 1.1: one complete externally-owned region. The original import functions
 * accept SYSTEM domains only; the optional explicit arena functions below
 * accept ARENA domains only and require MGMEM_CAP_ARENA_EXTERNAL_IMPORT.
 * external_id is a stable nonzero identity assigned by its origin owner. During
 * any import lifetime it must identify exactly this region and callback owner.
 * Memory imports the entire capacity, not a separately charged child view.
 * Use ordinary borrow offsets for views. Duplicate identities or overlapping
 * regions in this loaded runtime return BUSY, including across domains and
 * overlap with Memory-owned backing. Cross-runtime import coordination is not
 * supported: a physical region must use one managing Memory runtime only.
 * managing_runtime_origin must be mgmem_runtime_origin_v1() of that manager;
 * a descriptor routed to another loaded runtime is rejected. This cookie is
 * routing protection, not authentication or physical-page alias detection.
 *
 * retain is failure-atomic: non-OK MUST leave reference count unchanged. A
 * successful retain keeps data/context, size and address stable until release.
 * Both callbacks are thread-safe, no-throw and outside domain/registry locks;
 * they must not reenter the same domain. Memory never calls free/deallocate on
 * data. The origin remains responsible for allocator pairing and any other
 * aliases/access outside Memory leases. READ forbids WRITE acquisition, but is
 * an API contract, not page protection. Caller keeps its own reference until
 * this call returns. Callback contract violations poison the domain, retain
 * uncertain charges/claims, and are never retried or forcibly reclaimed.
 */
typedef struct mgmem_external_buffer_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t required_flags;
    const void *data;
    uint64_t capacity_bytes;
    uint64_t alignment;
    uint64_t external_id;
    uint32_t access;
    uint32_t reserved;
    void *context;
    mgmem_status (*retain)(void *context);
    void (*release)(void *context);
    uint64_t managing_runtime_origin;
} mgmem_external_buffer_v1;

/* Optional extension to external import. The region, identity, runtime and
 * retain rules are identical to mgmem_external_buffer_v1. release_checked is
 * called once outside core locks. Only MGMEM_OK confirms that the retained
 * origin reference was safely released. Any other result or exception means
 * ownership is uncertain: the domain is poisoned, the backing remains charged
 * and its claim is retained, and release is never retried by Memory. The caller
 * must keep callback code loaded and not reenter the same domain. A failed final
 * release consumes the owner/lease handle: retrying it is invalid. The status
 * from release or the last lease release is MGMEM_PROVIDER_ERROR; collect/drain
 * cannot claim normal completion and destroy remains BUSY.
 *
 * This is an optional additive ABI 1.1 extension: first resolve
 * mgmem_import_external_checked_v1 in the chosen loaded runtime, then verify
 * MGMEM_CAP_CHECKED_EXTERNAL_RELEASE via that same runtime's capabilities.
 * Older 1.1 libraries do not necessarily provide this symbol or capability. */
typedef struct mgmem_external_buffer_checked_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t required_flags;
    const void *data;
    uint64_t capacity_bytes;
    uint64_t alignment;
    uint64_t external_id;
    uint32_t access;
    uint32_t reserved;
    void *context;
    mgmem_status (*retain)(void *context);
    mgmem_status (*release_checked)(void *context);
    uint64_t managing_runtime_origin;
} mgmem_external_buffer_checked_v1;

/* Subset of the old snapshot, not additional bytes to add to backing_bytes.
 * Charged imports with a violating callback remain visible even if publication
 * failed; import_count counts successfully published imports only. */
typedef struct mgmem_import_snapshot_v1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t poisoned;
    uint64_t external_backing_bytes;
    uint64_t external_backing_count;
    uint64_t pending_import_bytes;
    uint64_t import_count;
} mgmem_import_snapshot_v1;

MGMEM_API uint32_t mgmem_abi_version_v1(void); /* major << 16 | minor */
/* Deliberately excludes JNI, special placement, page hints and hardware claims. */
MGMEM_API uint64_t mgmem_capabilities_v1(void);
MGMEM_API uint64_t mgmem_runtime_origin_v1(void);
MGMEM_API mgmem_status mgmem_checked_mul_v1(uint64_t a, uint64_t b, uint64_t *out);
MGMEM_API mgmem_status mgmem_checked_round_up_v1(uint64_t bytes, uint64_t alignment, uint64_t *out);
MGMEM_API mgmem_status mgmem_domain_create_v1(const mgmem_domain_options_v1 *options, mgmem_domain **out);
MGMEM_API mgmem_status mgmem_allocate_v1(mgmem_domain *domain, uint64_t bytes, uint64_t alignment, mgmem_handle_v1 *out);
MGMEM_API mgmem_status mgmem_import_external_v1(mgmem_domain *domain,
    const mgmem_external_buffer_v1 *buffer, mgmem_handle_v1 *out);
MGMEM_API mgmem_status mgmem_import_external_checked_v1(mgmem_domain *domain,
    const mgmem_external_buffer_checked_v1 *buffer, mgmem_handle_v1 *out);
/* Optional arena imports. Imported capacity is charged once to the same budget,
 * never used as arena allocation storage, and returned to its origin immediately
 * after the final owner/lease. Existing import functions retain their SYSTEM-only
 * contract. Availability is not implied by ABI minor 1: resolve the desired
 * symbol and check MGMEM_CAP_ARENA_EXTERNAL_IMPORT in that same runtime; the
 * checked form additionally requires MGMEM_CAP_CHECKED_EXTERNAL_RELEASE. */
MGMEM_API mgmem_status mgmem_import_external_arena_v1(mgmem_domain *domain,
    const mgmem_external_buffer_v1 *buffer, mgmem_handle_v1 *out);
MGMEM_API mgmem_status mgmem_import_external_arena_checked_v1(mgmem_domain *domain,
    const mgmem_external_buffer_checked_v1 *buffer, mgmem_handle_v1 *out);
/* A soft admission limit, including zero. Existing owners/leases and operations
 * with an existing reservation remain valid; no forced reclamation or implicit
 * close. New admission must fit the new limit. Snapshot may temporarily show
 * backing_bytes + reserved_bytes > budget_bytes after lowering the limit. */
MGMEM_API mgmem_status mgmem_domain_set_budget_v1(mgmem_domain *domain, uint64_t bytes);
MGMEM_API mgmem_status mgmem_domain_import_snapshot_v1(mgmem_domain *domain,
    mgmem_import_snapshot_v1 *out, uint32_t out_size);
/* Release consumes the allocation owner, but outstanding leases keep storage alive. */
MGMEM_API mgmem_status mgmem_release_v1(mgmem_domain *domain, mgmem_handle_v1 allocation);
/* Readers may coexist. A writer excludes all other leases of that allocation.
 * Acquire BEFORE enqueueing asynchronous work. Pointers must not outlive leases.
 * Results can escape by holding their lease, or by copying into a separate owner. */
MGMEM_API mgmem_status mgmem_borrow_acquire_v1(mgmem_domain *domain, mgmem_handle_v1 allocation,
    uint64_t offset, uint64_t length, uint32_t access, mgmem_borrow_v1 *out);
MGMEM_API mgmem_status mgmem_borrow_release_v1(mgmem_domain *domain, mgmem_handle_v1 lease);
/* reset: arena only, no live allocation/lease/pending operation; rewind retained blocks.
 * trim: no live allocation/lease/pending operation; return all backing to origin.
 * begin_close: nonblocking; existing leases/releases remain valid.
 * collect: synchronous final cleanup, CLOSING and quiet only; run on a cleanup worker.
 * drain: only waits for CLOSED, never runs callbacks; timeout 0 polls. A worker must
 * call collect after all allocations/leases finish. Never wait on your own lease.
 * destroy: requires CLOSED; caller must serialize it against every other API call. */
MGMEM_API mgmem_status mgmem_arena_reset_v1(mgmem_domain *domain);
MGMEM_API mgmem_status mgmem_domain_trim_v1(mgmem_domain *domain);
MGMEM_API mgmem_status mgmem_domain_begin_close_v1(mgmem_domain *domain);
MGMEM_API mgmem_status mgmem_domain_collect_v1(mgmem_domain *domain);
MGMEM_API mgmem_status mgmem_domain_drain_v1(mgmem_domain *domain, uint64_t timeout_ms);
MGMEM_API mgmem_status mgmem_domain_destroy_v1(mgmem_domain *domain);
MGMEM_API mgmem_status mgmem_domain_snapshot_v1(mgmem_domain *domain, mgmem_snapshot_v1 *out, uint32_t out_size);

#ifdef __cplusplus
}
#endif
#endif
