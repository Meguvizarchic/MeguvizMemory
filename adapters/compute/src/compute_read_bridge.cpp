#include <mgmem/compute_read_bridge.h>
#include <meguviz/compute.h>

#include <atomic>
#include <new>

#if defined(MGMEM_COMPUTE_BRIDGE_TESTING)
extern "C" void mgmem_compute_bridge_test_after_retain();
#endif

namespace {
struct Mapping {
    std::atomic<unsigned> references{1};
    std::atomic<bool> transferred{false};
    std::atomic<int32_t> unmap_status{MGC_SUCCESS};
    MgcLease *lease;
    mgmem_compute_unmap_error_v1 on_error;
    void *error_context;
};

void report_origin_error(Mapping *mapping, MgcStatus status) noexcept {
    mapping->unmap_status.store(status, std::memory_order_release);
    mapping->on_error(mapping->error_context, status);
}

mgmem_status retain_mapping(void *context) noexcept {
    auto *mapping = static_cast<Mapping *>(context);
    mapping->references.fetch_add(1, std::memory_order_relaxed);
    mapping->transferred.store(true, std::memory_order_release);
#if defined(MGMEM_COMPUTE_BRIDGE_TESTING)
    mgmem_compute_bridge_test_after_retain();
#endif
    return MGMEM_OK;
}

void release_reference(Mapping *mapping) noexcept {
    if (mapping->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete mapping;
}

MgcStatus finish_mapping(Mapping *mapping) noexcept {
    const MgcStatus status = mgc_lease_unmap(mapping->lease);
    if (status == MGC_SUCCESS) mgc_lease_release(mapping->lease);
    if (status != MGC_SUCCESS) report_origin_error(mapping, status);
    return status;
}

mgmem_status release_callback(void *context) noexcept {
    auto *mapping = static_cast<Mapping *>(context);
    if (finish_mapping(mapping) != MGC_SUCCESS)
        return MGMEM_PROVIDER_ERROR; // retain context along with core's claim
    release_reference(mapping);
    return MGMEM_OK;
}

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

using ImportChecked = mgmem_status (*)(mgmem_domain *, const mgmem_external_buffer_checked_v1 *, mgmem_handle_v1 *);

mgmem_status mgmem_compute_import_read_impl(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_compute_status, ImportChecked import_checked) {
    if (out_compute_status) *out_compute_status = MGC_SUCCESS;
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = {};
    if (!domain || !lease || !external_id || !on_unmap_error || !out_compute_status)
        return MGMEM_INVALID_ARGUMENT;
    if ((mgmem_capabilities_v1() & MGMEM_CAP_CHECKED_EXTERNAL_RELEASE) == 0)
        return MGMEM_UNSUPPORTED;

    MgcLeaseInfo info{};
    info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    MgcStatus status = mgc_lease_info(lease, &info);
    if (status != MGC_SUCCESS) {
        if (out_compute_status) *out_compute_status = status;
        return translate(status);
    }
    if (info.header.struct_size < sizeof(MgcLeaseInfo) ||
        info.header.abi_major != MGC_ABI_MAJOR ||
        info.access != MGC_ACCESS_READ || info.dependency_kind != MGC_DEPENDENCY_READY ||
        info.byte_offset != 0 || !info.capacity_bytes || info.byte_length != info.capacity_bytes)
        return MGMEM_UNSUPPORTED;

    auto *mapping = new (std::nothrow) Mapping{{1}, {false}, {MGC_SUCCESS}, lease, on_unmap_error, error_context};
    if (!mapping) return MGMEM_OUT_OF_MEMORY;
    mgc_lease_retain(lease);
    void *data = nullptr;
    uint64_t bytes = 0;
    status = mgc_lease_map(lease, &data, &bytes);
    if (status != MGC_SUCCESS) {
        mgc_lease_release(lease);
        delete mapping;
        if (out_compute_status) *out_compute_status = status;
        return translate(status);
    }
    if (!data || bytes != info.capacity_bytes) {
        status = finish_mapping(mapping);
        if (status == MGC_SUCCESS) release_reference(mapping);
        if (out_compute_status) *out_compute_status = status;
        return status == MGC_SUCCESS ? MGMEM_PROVIDER_ERROR : MGMEM_COMPUTE_ROLLBACK_QUARANTINED;
    }
    mgmem_external_buffer_checked_v1 external{};
    external.struct_size = sizeof(external);
    external.abi_major = MGMEM_ABI_MAJOR;
    external.abi_minor = MGMEM_ABI_MINOR;
    external.data = data;
    external.capacity_bytes = bytes;
    external.alignment = 1;
    external.external_id = external_id;
    external.access = MGMEM_READ;
    external.context = mapping;
    external.retain = retain_mapping;
    external.release_checked = release_callback;
    external.managing_runtime_origin = mgmem_runtime_origin_v1();
    const auto result = import_checked(domain, &external, out);
    if (!mapping->transferred.load(std::memory_order_acquire)) {
        status = finish_mapping(mapping);
        if (status != MGC_SUCCESS) {
            *out_compute_status = status;
            return MGMEM_COMPUTE_ROLLBACK_QUARANTINED;
        }
    } else {
        status = mapping->unmap_status.load(std::memory_order_acquire);
        if (status != MGC_SUCCESS) *out_compute_status = status;
    }
    release_reference(mapping);
    return result;
}

extern "C" mgmem_status mgmem_compute_import_read_v1(
    mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    mgmem_compute_unmap_error_v1 on_unmap_error, void *error_context,
    mgmem_handle_v1 *out, int32_t *out_compute_status) {
    return mgmem_compute_import_read_impl(domain, lease, external_id, on_unmap_error, error_context,
                                          out, out_compute_status, mgmem_import_external_checked_v1);
}
