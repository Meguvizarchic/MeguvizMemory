#include <mgmem/compute_transfer.h>

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
}

extern "C" mgmem_transfer_result mgmem_compute_transfer_v1(const mgmem_transfer *args, uint32_t upload) {
    mgmem_transfer_result result{MGMEM_INVALID_ARGUMENT, MGC_SUCCESS, nullptr};
    if (!args || !args->memory || !args->compute || !args->session || !args->device ||
        !args->bytes || !args->on_error || upload > 1 ||
        bool(args->retain_context) != bool(args->release_context)) return result;

    MgcResourceDesc resource_desc{};
    resource_desc.header = MGC_HEADER_INIT(MgcResourceDesc);
    resource_desc.capacity_bytes = args->bytes;
    resource_desc.format = MGC_FORMAT_BYTES;
    MgcResource *resource = nullptr;
    result.status = mgmem_compute_create_host_resource_v1(args->memory, args->allocation, args->offset,
        args->compute, &resource_desc, upload ? MGC_ACCESS_READ : MGC_ACCESS_WRITE,
        args->on_error, args->error_context, &resource, &result.origin_status);
    if (result.status != MGMEM_OK) return result;

    MgcLeaseDesc lease_desc{};
    lease_desc.header = MGC_HEADER_INIT(MgcLeaseDesc);
    lease_desc.content_id = 1;
    lease_desc.byte_length = args->bytes;
    lease_desc.access = upload ? MGC_ACCESS_READ : MGC_ACCESS_WRITE;
    MgcLease *host = nullptr;
    result.origin_status = mgc_lease_create(resource, &lease_desc, &host);
    mgc_resource_release(resource);
    if (result.origin_status != MGC_SUCCESS) {
        result.status = translate(result.origin_status);
        return result;
    }

    MgcSubmitDesc submit{};
    submit.header = MGC_HEADER_INIT(MgcSubmitDesc);
    submit.request_id = args->request_id;
    submit.operation = MGC_OPERATION_COPY;
    submit.input = upload ? host : args->device;
    submit.output = upload ? args->device : host;
    submit.byte_count = args->bytes;
    submit.completion = args->completion;
    submit.context = args->context;
    submit.retain_context = args->retain_context;
    submit.release_context = args->release_context;
    mgc_lease_retain(args->device);
    result.origin_status = mgc_submit(args->session, &submit, &result.ticket);
    if (result.origin_status != MGC_ACCEPTED) {
        mgc_lease_release(args->device);
        mgc_lease_release(host);
    }
    result.status = result.origin_status == MGC_SUCCESS ? MGMEM_PROVIDER_ERROR : translate(result.origin_status);
    return result;
}
