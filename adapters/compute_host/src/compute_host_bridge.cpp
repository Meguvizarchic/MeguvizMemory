#include <mgmem/compute_host_bridge.h>

#include <atomic>
#include <new>

namespace {
struct Context {
    std::atomic<unsigned> references{1};
    mgmem_domain *domain;
    mgmem_borrow_v1 view{};
    mgmem_compute_host_error_fn on_error;
    void *error_context;
};

void retain(void *opaque) noexcept {
    static_cast<Context *>(opaque)->references.fetch_add(1, std::memory_order_relaxed);
}

mgmem_status release_context(void *opaque) noexcept {
    auto *context = static_cast<Context *>(opaque);
    if (context->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return MGMEM_OK;
    const auto status = mgmem_borrow_release_v1(context->domain, context->view.lease);
    if (status != MGMEM_OK) {
        context->on_error(context->error_context, status);
        return status;
    }
    delete context;
    return MGMEM_OK;
}

void release(void *opaque) noexcept { (void)release_context(opaque); }

mgmem_status translate(MgcStatus status) noexcept {
    switch (status) {
    case MGC_SUCCESS: return MGMEM_OK;
    case MGC_INVALID_ARGUMENT: return MGMEM_INVALID_ARGUMENT;
    case MGC_BUSY: case MGC_BACKPRESSURE: return MGMEM_BUSY;
    case MGC_OUT_OF_MEMORY: return MGMEM_OUT_OF_MEMORY;
    case MGC_TIMEOUT: return MGMEM_TIMEOUT;
    case MGC_UNSUPPORTED_ABI: case MGC_UNSUPPORTED_CAPABILITY: return MGMEM_UNSUPPORTED;
    default: return MGMEM_PROVIDER_ERROR;
    }
}
}

extern "C" mgmem_status mgmem_compute_create_host_resource_v1(
    mgmem_domain *domain, mgmem_handle_v1 allocation, uint64_t offset,
    MgcDomain *compute, const MgcResourceDesc *desc, uint32_t access,
    mgmem_compute_host_error_fn on_error, void *error_context,
    MgcResource **out, int32_t *out_compute_status) {
    if (out_compute_status) *out_compute_status = MGC_SUCCESS;
    if (out) *out = nullptr;
    if (!domain || !compute || !desc || !on_error || !out || !out_compute_status ||
        (access != MGC_ACCESS_READ && access != MGC_ACCESS_WRITE) ||
        desc->header.struct_size < sizeof(MgcResourceDesc) || !desc->capacity_bytes || desc->usage != 0)
        return MGMEM_INVALID_ARGUMENT;
    auto *context = new (std::nothrow) Context{{1}, domain, {}, on_error, error_context};
    if (!context) return MGMEM_OUT_OF_MEMORY;
    auto status = mgmem_borrow_acquire_v1(domain, allocation, offset, desc->capacity_bytes,
        access == MGC_ACCESS_READ ? MGMEM_READ : MGMEM_WRITE, &context->view);
    if (status != MGMEM_OK) {
        delete context;
        return status;
    }
    MgcHostMemoryDesc backing{};
    backing.header = MGC_HEADER_INIT(MgcHostMemoryDesc);
    backing.data = context->view.data;
    backing.capacity_bytes = context->view.length;
    backing.access = access;
    backing.context = context;
    backing.retain = retain;
    backing.release = release;
    const auto result = mgc_resource_import_host_v1(compute, desc, &backing, out);
    *out_compute_status = result;
    status = release_context(context);
    if (status != MGMEM_OK) return status;
    return translate(result);
}
