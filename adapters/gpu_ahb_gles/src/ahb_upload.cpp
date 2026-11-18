#include <mgmem/ahb_upload.h>
#include "upload_internal.hpp"
#include <climits>
#include <cstddef>
#include <limits>
#include <new>

namespace {
void destroy(mgmem_ahb_upload *p) noexcept {
    if (p->producer_fd >= 0) mgmem_upload_platform_close_fd(p->producer_fd);
    mgmem_upload_platform_discard(p->platform);
    delete p;
}
bool valid(const mgmem_ahb_upload_desc &d) noexcept {
    if (!d.memory || !d.write_lease || !d.on_error || !d.width || !d.height ||
        d.ready_timeout_ms < -1 || d.width > INT_MAX || d.height > INT_MAX ||
        d.source_stride % 4 || d.source_stride / 4 > INT_MAX) return false;
    const uint64_t row = uint64_t(d.width) * 4;
    if (d.source_stride < row ||
        uint64_t(d.height - 1) > (std::numeric_limits<uint64_t>::max() - row) / d.source_stride)
        return false;
    const uint64_t needed = uint64_t(d.height - 1) * d.source_stride + row;
    return needed <= d.length && needed <= std::numeric_limits<std::size_t>::max();
}
mgmem_status cancel_before_submit(mgmem_ahb_upload *p, int32_t *compute) noexcept {
    const auto status = mgmem_gpu_ahb_cancel(&p->reservation, compute);
    if (status != MGMEM_OK) {
        /* Compute accepted the return but final Memory release was not
         * confirmed. The underlying reservation is now quarantined. */
        if (status == MGMEM_PROVIDER_ERROR && *compute == 0) p->phase = MGMEM_UPLOAD_QUARANTINED;
        return status;
    }
    destroy(p);
    return MGMEM_OK;
}
/* A reported failure invalidates Pack's content but is not a physical
 * retirement proof. Local owners remain quarantined after notification. */
mgmem_status notify_guard_failure(mgmem_ahb_upload *p, int32_t *compute) noexcept {
    const auto status = p->guard_finish(p->write_lease, p->guard_generation, true, compute);
    p->phase = (status == MGMEM_BUSY || status == MGMEM_TIMEOUT)
        ? MGMEM_UPLOAD_FAIL_NOTIFY : MGMEM_UPLOAD_QUARANTINED;
    return status;
}
}

extern "C" mgmem_ahb_upload_result mgmem_ahb_upload_submit(const mgmem_ahb_upload_desc *d) {
    mgmem_ahb_upload_result result{};
    result.status = MGMEM_INVALID_ARGUMENT;
    if (!d || !valid(*d)) return result;
    auto *p = new (std::nothrow) mgmem_ahb_upload{};
    if (!p) { result.status = MGMEM_OUT_OF_MEMORY; return result; }
    p->write_lease = d->write_lease;
    result.status = mgmem_upload_platform_open(&p->platform, &result.platform_status);
    if (result.status != MGMEM_OK) { destroy(p); return result; }
    auto r = mgmem_gpu_ahb_begin(d->memory, d->allocation, d->offset, d->length,
                                  d->write_lease, d->on_error, d->error_context);
    result.status = r.status; result.compute_status = r.compute_status;
    p->reservation = r.pending;
    if (r.status != MGMEM_OK) {
        if (p->reservation) { p->phase = MGMEM_UPLOAD_QUARANTINED; result.pending = p; result.phase = p->phase; }
        else destroy(p);
        return result;
    }
    p->phase = MGMEM_UPLOAD_CANCEL;
    const int ready = mgmem_upload_platform_wait(r.view.ready_fd, d->ready_timeout_ms);
    if (ready < 0 || ready == 2 || r.view.ready_fd < -1) {
        /* A terminal error or unverifiable input fence cannot be returned to a
         * Compute path that might interpret a signal as successful content. */
        result.status = MGMEM_PROVIDER_ERROR;
        result.platform_status = ready == 2 ? 2 : -1;
        p->phase = MGMEM_UPLOAD_QUARANTINED;
        result.pending = p; result.phase = p->phase;
        return result;
    }
    if (ready || !r.view.data || r.view.length < d->length ||
        r.view.width != d->width || r.view.height != d->height) {
        result.status = ready == 1 ? MGMEM_TIMEOUT : MGMEM_UNSUPPORTED;
        const auto cancelled = cancel_before_submit(p, &result.compute_status);
        if (cancelled != MGMEM_OK) { result.pending = p; result.phase = p->phase; }
        return result;
    }
    /* From here even target binding can change the AHB. Never cancel it on
     * an unconfirmed GL/EGL error; preserve every owner and target object. */
    p->phase = MGMEM_UPLOAD_QUARANTINED;
    result.status = mgmem_upload_platform_write(p->platform, r.view, d->source_stride, &result.platform_status);
    if (result.status != MGMEM_OK) { result.pending = p; result.phase = p->phase; return result; }
    result.status = mgmem_upload_platform_fence(p->platform, &p->producer_fd, &result.platform_status);
    if (result.status != MGMEM_OK || p->producer_fd < 0) {
        if (result.status == MGMEM_OK) result.status = MGMEM_PROVIDER_ERROR;
        result.pending = p; result.phase = p->phase; return result;
    }
    /* Do not hand Compute an unverified sync-file: a signaled fence may carry
     * an error, and the existing Compute path does not expose that status. */
    p->phase = MGMEM_UPLOAD_PRODUCING;
    result.status = MGMEM_OK;
    result.pending = p; result.phase = p->phase;
    return result;
}

extern "C" mgmem_status mgmem_ahb_upload_retry(mgmem_ahb_upload *p, int32_t *compute) {
    if (!p || !compute) return MGMEM_INVALID_ARGUMENT;
    if (p->phase == MGMEM_UPLOAD_FAIL_NOTIFY && p->guarded_published)
        return notify_guard_failure(p, compute);
    if (p->phase != MGMEM_UPLOAD_PUBLISH || !p->producer_verified || !p->platform_cleaned)
        return MGMEM_INVALID_ARGUMENT;
    /* A successful sync-file status proves physical producer retirement. */
    const auto status = mgmem_gpu_ahb_publish(p->reservation, -1, compute);
    if (status == MGMEM_OK) p->phase = MGMEM_UPLOAD_RETIRE;
    return status;
}

extern "C" mgmem_status mgmem_ahb_upload_wait(mgmem_ahb_upload **input, int32_t timeout_ms) {
    if (!input || !*input || timeout_ms < -1) return MGMEM_INVALID_ARGUMENT;
    auto *p = *input;
    if (p->phase == MGMEM_UPLOAD_QUARANTINED) return MGMEM_PROVIDER_ERROR;
    if (p->phase != MGMEM_UPLOAD_PRODUCING && p->phase != MGMEM_UPLOAD_PUBLISH &&
        p->phase != MGMEM_UPLOAD_RETIRE) return MGMEM_BUSY;
    if (p->phase == MGMEM_UPLOAD_PRODUCING) {
        if (!p->producer_verified) {
            const int waited = p->guarded_published && timeout_ms == 0
                ? mgmem_upload_platform_probe(p->producer_fd)
                : mgmem_upload_platform_wait(p->producer_fd, timeout_ms);
            if (waited == 1 || waited == 2) return MGMEM_TIMEOUT;
            if (waited != 0) {
                /* Wait's generic error also includes unverifiable status.
                 * Probe can distinguish a confirmed negative producer status. */
                const int failure = p->guarded_published && timeout_ms != 0
                    ? mgmem_upload_platform_probe(p->producer_fd) : waited;
                if (p->guarded_published && failure == -2) {
                    int32_t compute = 0;
                    (void)notify_guard_failure(p, &compute);
                } else p->phase = MGMEM_UPLOAD_QUARANTINED;
                return MGMEM_PROVIDER_ERROR;
            }
            p->producer_verified = true;
        }
        if (!p->platform_cleaned) {
            int32_t detail = 0;
            const auto status = mgmem_upload_platform_cleanup(p->platform, &detail);
            if (status != MGMEM_OK) {
                if (status != MGMEM_BUSY) {
                    if (p->guarded_published) {
                        int32_t compute = 0;
                        (void)notify_guard_failure(p, &compute);
                    } else p->phase = MGMEM_UPLOAD_QUARANTINED;
                }
                return status;
            }
            p->platform_cleaned = true;
        }
        if (p->guarded_published) {
            int32_t compute = 0;
            const auto status = p->guard_finish(p->write_lease, p->guard_generation, false, &compute);
            if (status != MGMEM_OK) {
                if (status != MGMEM_BUSY && status != MGMEM_TIMEOUT)
                    p->phase = MGMEM_UPLOAD_QUARANTINED;
                return status;
            }
            p->phase = MGMEM_UPLOAD_RETIRE;
        } else {
            p->phase = MGMEM_UPLOAD_PUBLISH;
        }
    }
    if (p->phase == MGMEM_UPLOAD_PUBLISH) {
        int32_t compute = 0;
        const auto status = mgmem_gpu_ahb_publish(p->reservation, -1, &compute);
        if (status != MGMEM_OK) return status;
        p->phase = MGMEM_UPLOAD_RETIRE;
    }
    const auto status = p->guarded_published
        ? p->guard_retire(&p->reservation, p->producer_fd)
        : mgmem_gpu_ahb_retire(&p->reservation, 0);
    if (status != MGMEM_OK) {
        if (status != MGMEM_TIMEOUT && status != MGMEM_BUSY) p->phase = MGMEM_UPLOAD_QUARANTINED;
        return status;
    }
    destroy(p); *input = nullptr;
    return MGMEM_OK;
}

extern "C" mgmem_status mgmem_ahb_upload_cancel(mgmem_ahb_upload **input, int32_t *compute) {
    if (!input || !*input || !compute) return MGMEM_INVALID_ARGUMENT;
    auto *p = *input;
    if (p->phase != MGMEM_UPLOAD_CANCEL) return MGMEM_BUSY;
    const auto status = cancel_before_submit(p, compute);
    if (status == MGMEM_OK) *input = nullptr;
    return status;
}

extern "C" uint32_t mgmem_ahb_upload_phase(const mgmem_ahb_upload *p) {
    return p ? p->phase : MGMEM_UPLOAD_NONE;
}
