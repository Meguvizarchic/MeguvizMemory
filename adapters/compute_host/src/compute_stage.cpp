#include <mgmem/compute_stage.h>
#include <atomic>
#include <new>

struct mgmem_stage {
    std::atomic<bool> busy{false};
    MgcLease *host = nullptr;
    uint64_t bytes = 0;
    bool upload = false;
    bool mapped = false;
    MgcCompletionFn completion = nullptr;
    void *context = nullptr;
    MgcContextFn retain_context = nullptr;
    MgcContextFn release_context = nullptr;
};

namespace {
mgmem_status translate(MgcStatus status) noexcept {
    switch (status) {
    case MGC_ACCEPTED: case MGC_SUCCESS: return MGMEM_OK;
    case MGC_INVALID_ARGUMENT: return MGMEM_INVALID_ARGUMENT;
    case MGC_BUSY: case MGC_BACKPRESSURE: return MGMEM_BUSY;
    case MGC_OUT_OF_MEMORY: return MGMEM_OUT_OF_MEMORY;
    case MGC_TIMEOUT: return MGMEM_TIMEOUT;
    case MGC_UNSUPPORTED_ABI: case MGC_UNSUPPORTED_CAPABILITY: return MGMEM_UNSUPPORTED;
    default: return MGMEM_PROVIDER_ERROR;
    }
}
void retain_context(void *opaque) {
    auto *stage = static_cast<mgmem_stage *>(opaque);
    if (stage->retain_context) stage->retain_context(stage->context);
}
void complete(void *opaque, const MgcCompletion *completion) {
    auto *stage = static_cast<mgmem_stage *>(opaque);
    if (stage->completion) stage->completion(stage->context, completion);
}
void release_context(void *opaque) {
    auto *stage = static_cast<mgmem_stage *>(opaque);
    if (stage->release_context) stage->release_context(stage->context);
    stage->busy.store(false, std::memory_order_release);
}
}

extern "C" mgmem_stage_result mgmem_stage_prepare_v1(const mgmem_transfer *args, uint32_t upload) {
    mgmem_stage_result result{MGMEM_INVALID_ARGUMENT, MGC_SUCCESS, nullptr};
    if (!args || !args->memory || !args->compute || !args->bytes || !args->on_error || upload > 1)
        return result;
    auto *stage = new (std::nothrow) mgmem_stage;
    if (!stage) { result.status = MGMEM_OUT_OF_MEMORY; return result; }
    MgcResourceDesc desc{};
    desc.header = MGC_HEADER_INIT(MgcResourceDesc);
    desc.capacity_bytes = args->bytes;
    desc.format = MGC_FORMAT_BYTES;
    const uint32_t access = upload ? MGC_ACCESS_READ : MGC_ACCESS_WRITE;
    MgcResource *resource = nullptr;
    result.status = mgmem_compute_create_host_resource_v1(args->memory, args->allocation, args->offset,
        args->compute, &desc, access, args->on_error, args->error_context, &resource, &result.origin_status);
    if (result.status != MGMEM_OK) { delete stage; return result; }
    MgcLeaseDesc lease{};
    lease.header = MGC_HEADER_INIT(MgcLeaseDesc);
    lease.content_id = 1;
    lease.byte_length = args->bytes;
    lease.access = access;
    result.origin_status = mgc_lease_create(resource, &lease, &stage->host);
    mgc_resource_release(resource);
    if (result.origin_status != MGC_SUCCESS) {
        result.status = translate(result.origin_status);
        delete stage;
        return result;
    }
    stage->bytes = args->bytes;
    stage->upload = upload != 0;
    result.stage = stage;
    return result;
}

extern "C" mgmem_transfer_result mgmem_stage_send(mgmem_stage *stage, const mgmem_transfer *args) {
    mgmem_transfer_result result{MGMEM_INVALID_ARGUMENT, MGC_SUCCESS, nullptr};
    if (!stage || !args || !args->session || !args->device ||
        bool(args->retain_context) != bool(args->release_context)) return result;
    if (stage->mapped || stage->busy.exchange(true, std::memory_order_acq_rel)) {
        result.status = MGMEM_BUSY;
        return result;
    }
    stage->completion = args->completion;
    stage->context = args->context;
    stage->retain_context = args->retain_context;
    stage->release_context = args->release_context;
    MgcSubmitDesc submit{};
    submit.header = MGC_HEADER_INIT(MgcSubmitDesc);
    submit.request_id = args->request_id;
    submit.operation = MGC_OPERATION_COPY;
    submit.input = stage->upload ? stage->host : args->device;
    submit.output = stage->upload ? args->device : stage->host;
    submit.byte_count = stage->bytes;
    submit.completion = complete;
    submit.context = stage;
    submit.retain_context = retain_context;
    submit.release_context = release_context;
    mgc_lease_retain(stage->host);
    mgc_lease_retain(args->device);
    result.origin_status = mgc_submit(args->session, &submit, &result.ticket);
    if (result.origin_status != MGC_ACCEPTED) {
        mgc_lease_release(args->device);
        mgc_lease_release(stage->host);
        stage->busy.store(false, std::memory_order_release);
    }
    result.status = result.origin_status == MGC_SUCCESS ? MGMEM_PROVIDER_ERROR : translate(result.origin_status);
    return result;
}

extern "C" MgcStatus mgmem_stage_map(mgmem_stage *stage, void **data, uint64_t *bytes) {
    if (!stage || !data || !bytes) return MGC_INVALID_ARGUMENT;
    *data = nullptr; *bytes = 0;
    if (stage->upload) return MGC_UNSUPPORTED_CAPABILITY;
    if (stage->mapped || stage->busy.load(std::memory_order_acquire)) return MGC_BUSY;
    const auto status = mgc_lease_map(stage->host, data, bytes);
    if (status == MGC_SUCCESS) stage->mapped = true;
    return status;
}
extern "C" MgcStatus mgmem_stage_unmap(mgmem_stage *stage) {
    if (!stage || !stage->mapped) return MGC_INVALID_ARGUMENT;
    const auto status = mgc_lease_unmap(stage->host);
    if (status == MGC_SUCCESS) stage->mapped = false;
    return status;
}
extern "C" mgmem_status mgmem_stage_close(mgmem_stage **input) {
    if (!input || !*input) return MGMEM_INVALID_ARGUMENT;
    auto *stage = *input;
    if (stage->mapped || stage->busy.load(std::memory_order_acquire)) return MGMEM_BUSY;
    mgc_lease_release(stage->host);
    delete stage;
    *input = nullptr;
    return MGMEM_OK;
}
