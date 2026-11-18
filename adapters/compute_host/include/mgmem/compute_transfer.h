#ifndef MGMEM_COMPUTE_TRANSFER_H
#define MGMEM_COMPUTE_TRANSFER_H

#include <mgmem/compute_host_bridge.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Memory domain and error callback context must remain alive until the
 * accepted submission releases its host lease, after completion returns.
 * The Compute lease remains caller-owned on every path. An accepted ticket is
 * caller-owned and must be released. A successful submission returns
 * MGMEM_OK and MGC_ACCEPTED; check completion for execution failure.
 * Completion may run before the call returns. Keep its context alive and do
 * not access or publish the submitted lease until execution has retired.
 * Ticket completion alone does not mean its callback has returned or the
 * Memory borrow has been released; drain the session for that boundary.
 * CPU-reference Compute is useful for testing the same transfer contract;
 * GPU buffers require a Vulkan domain. AHB is not supported by Compute COPY.
 * COPY on Vulkan requires 4-byte size and offsets and may require stricter GPU
 * offset alignment. It stages/copies data and is not a zero-copy GPU mapping. */
typedef struct mgmem_transfer {
    mgmem_domain *memory;
    mgmem_handle_v1 allocation;
    uint64_t offset;
    uint64_t bytes;
    MgcDomain *compute;
    MgcSession *session;
    MgcLease *device;
    uint64_t request_id;
    MgcCompletionFn completion;
    void *context;
    MgcContextFn retain_context;
    MgcContextFn release_context;
    mgmem_compute_host_error_fn on_error;
    void *error_context;
} mgmem_transfer;

typedef struct mgmem_transfer_result {
    mgmem_status status;
    int32_t origin_status;
    MgcTicket *ticket;
} mgmem_transfer_result;

/* Internal common entry; prefer the short direction-specific wrappers. */
mgmem_transfer_result mgmem_compute_transfer_v1(const mgmem_transfer *transfer, uint32_t upload);

static inline mgmem_transfer_result mgmem_upload(const mgmem_transfer *transfer) {
    return mgmem_compute_transfer_v1(transfer, 1);
}
static inline mgmem_transfer_result mgmem_download(const mgmem_transfer *transfer) {
    return mgmem_compute_transfer_v1(transfer, 0);
}

#ifdef __cplusplus
}
namespace mgmem::compute {
using transfer = ::mgmem_transfer;
using transfer_result = ::mgmem_transfer_result;
[[nodiscard]] inline transfer_result upload(const transfer &value) noexcept { return mgmem_upload(&value); }
[[nodiscard]] inline transfer_result download(const transfer &value) noexcept { return mgmem_download(&value); }
}
#endif
#endif
