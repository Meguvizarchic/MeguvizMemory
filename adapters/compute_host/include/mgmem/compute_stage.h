#ifndef MGMEM_COMPUTE_STAGE_H
#define MGMEM_COMPUTE_STAGE_H

#include <mgmem/compute_transfer.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mgmem_stage mgmem_stage;
typedef struct mgmem_stage_result {
    mgmem_status status;
    int32_t origin_status;
    mgmem_stage *stage;
} mgmem_stage_result;

/* Preparation pins one Memory borrow and Compute host lease until stage close
 * and all accepted submissions release their extra references. Upload pins a
 * READ borrow: the Memory content must remain unchanged throughout. Download
 * pins a WRITE borrow: inspect/update it through mgmem_stage_map/unmap only.
 * Only memory, allocation, offset, bytes, compute, on_error and error_context
 * are used from transfer at preparation time. The domain and error callback
 * context must outlive both the stage and all its accepted submissions. */
mgmem_stage_result mgmem_stage_prepare_v1(const mgmem_transfer *transfer, uint32_t upload);
static inline mgmem_stage_result mgmem_prepare_upload(const mgmem_transfer *t) {
    return mgmem_stage_prepare_v1(t, 1);
}
static inline mgmem_stage_result mgmem_prepare_download(const mgmem_transfer *t) {
    return mgmem_stage_prepare_v1(t, 0);
}

/* Send uses session, device, request_id, completion, context, retain_context
 * and release_context from t. All other t fields are ignored. The caller owns
 * its device lease and the returned ticket, as for mgmem_upload/download.
 * At most one submission is accepted at a time; BUSY lasts through completion
 * and release_context callbacks, including cancellation. Callers serialize all
 * stage API calls (including close) against one another and keep the stage alive
 * until send returns. Callback context retain/release are paired, thread-safe,
 * non-throwing and may run before send returns. */
mgmem_transfer_result mgmem_stage_send(mgmem_stage *stage, const mgmem_transfer *t);

/* Download stages can map their pinned Memory bytes when idle. Map/unmap are
 * serialized with other stage calls. Every successful map must be unmapped
 * before send or close. An upload stage rejects map and remains immutable. */
MgcStatus mgmem_stage_map(mgmem_stage *stage, void **data, uint64_t *bytes);
MgcStatus mgmem_stage_unmap(mgmem_stage *stage);

/* Returns BUSY while mapped or until the active completion and context-release
 * callbacks return. Success consumes *stage. A successful close does not prove
 * Compute has dropped its last submission lease: drain the session before
 * destroying the Memory domain or its error callback context. */
mgmem_status mgmem_stage_close(mgmem_stage **stage);

#ifdef __cplusplus
}
namespace mgmem::compute {
using stage = ::mgmem_stage;
using stage_result = ::mgmem_stage_result;
[[nodiscard]] inline stage_result prepare_upload(const transfer &t) noexcept { return mgmem_prepare_upload(&t); }
[[nodiscard]] inline stage_result prepare_download(const transfer &t) noexcept { return mgmem_prepare_download(&t); }
[[nodiscard]] inline transfer_result send(stage *s, const transfer &t) noexcept { return mgmem_stage_send(s, &t); }
}
#endif
#endif
