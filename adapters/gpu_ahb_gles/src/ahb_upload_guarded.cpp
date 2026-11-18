#include <mgmem/ahb_upload_guarded.h>
#include <meguviz/compute_android.h>
#include "upload_internal.hpp"

/* These additive Pack APIs are referenced only by the optional guarded
 * target. Existing uploader binaries never acquire these new dependencies. */
extern "C" MgcStatus mgc_android_set_guarded_producer_fence(MgcLease *, int32_t, uint64_t *);
extern "C" MgcStatus mgc_android_finish_guarded_producer(MgcLease *, uint64_t, MgcStatus);
extern "C" mgmem_status mgmem_gpu_ahb_guard_register_internal(mgmem_gpu_ahb *, int32_t,
    int32_t (*)(MgcLease *, int32_t, uint64_t *), uint64_t *, int32_t *);
extern "C" mgmem_status mgmem_gpu_ahb_guard_retire_verified_internal(mgmem_gpu_ahb **, int32_t);

namespace {
constexpr uint32_t guarded_producer_capability = 19;
#if defined(MGMEM_GUARDED_REQUIRE_SDK_ENUMS)
static_assert(MGC_CAP_GUARDED_PRODUCER_FENCE == guarded_producer_capability,
              "Compute guarded capability ABI differs");
static_assert(MGC_PRODUCER_FAILED == -15, "Compute producer failure ABI differs");
#endif
mgmem_status translate(MgcStatus value) noexcept {
    switch (value) {
    case MGC_SUCCESS: return MGMEM_OK;
    case MGC_INVALID_ARGUMENT: case MGC_INCOMPATIBLE_DOMAIN: return MGMEM_INVALID_ARGUMENT;
    case MGC_BUSY: case MGC_BACKPRESSURE: return MGMEM_BUSY;
    case MGC_TIMEOUT: return MGMEM_TIMEOUT;
    case MGC_OUT_OF_MEMORY: return MGMEM_OUT_OF_MEMORY;
    case MGC_UNSUPPORTED_ABI: case MGC_UNSUPPORTED_CAPABILITY: return MGMEM_UNSUPPORTED;
    default: return MGMEM_PROVIDER_ERROR;
    }
}
mgmem_status finish_guard(MgcLease *lease, uint64_t generation, bool failed, int32_t *out) noexcept {
    /* -15 is the additive MGC_PRODUCER_FAILED from guarded Compute. */
    *out = mgc_android_finish_guarded_producer(lease, generation, failed ? -15 : MGC_SUCCESS);
    return translate(*out);
}
}

extern "C" mgmem_status mgmem_ahb_queue(mgmem_ahb_upload *p, MgcDomain *domain, int32_t *out) {
    if (out) *out = MGC_SUCCESS;
    if (!p || !domain || !out) return MGMEM_INVALID_ARGUMENT;
    if (p->phase != MGMEM_UPLOAD_PRODUCING || !p->reservation || !p->write_lease ||
        p->producer_fd < 0 || p->guarded_published) return MGMEM_BUSY;

    MgcLeaseInfo info{};
    info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    *out = mgc_lease_info(p->write_lease, &info);
    if (*out != MGC_SUCCESS) return translate(*out);
    if (info.header.struct_size < sizeof(info) || info.header.abi_major != MGC_ABI_MAJOR ||
        info.access != MGC_ACCESS_WRITE) return MGMEM_UNSUPPORTED;
    uint64_t id = 0, generation = 0;
    *out = mgc_domain_identity(domain, &id, &generation);
    if (*out != MGC_SUCCESS) return translate(*out);
    if (!id || id != info.domain_id || generation != info.domain_generation) {
        *out = MGC_INCOMPATIBLE_DOMAIN;
        return MGMEM_INVALID_ARGUMENT;
    }
    MgcCapability capability{};
    capability.header = MGC_HEADER_INIT(MgcCapability);
    *out = mgc_domain_capability(domain, guarded_producer_capability, &capability);
    if (*out == MGC_INVALID_ARGUMENT || *out == MGC_UNSUPPORTED_CAPABILITY || *out == MGC_UNSUPPORTED_ABI)
        return MGMEM_UNSUPPORTED;
    if (*out != MGC_SUCCESS) return translate(*out);
    if (capability.header.struct_size < sizeof(capability) || capability.header.abi_major != MGC_ABI_MAJOR ||
        capability.feature != guarded_producer_capability || !capability.advertised || !capability.enabled) {
        *out = MGC_UNSUPPORTED_CAPABILITY;
        return MGMEM_UNSUPPORTED;
    }
    if (capability.probe_result != MGC_SUCCESS) {
        *out = capability.probe_result;
        return translate(*out);
    }

    uint64_t guard = 0;
    const auto status = mgmem_gpu_ahb_guard_register_internal(p->reservation, p->producer_fd,
        mgc_android_set_guarded_producer_fence, &guard, out);
    if (status != MGMEM_OK) return status;
    p->guard_finish = finish_guard;
    p->guard_retire = mgmem_gpu_ahb_guard_retire_verified_internal;
    p->guard_generation = guard;
    p->guarded_published = true;
    if (!guard) {
        /* Pack accepted a guard but returned no usable identity. It must not
         * be guessed or released: retain the entire uncertain reservation. */
        p->phase = MGMEM_UPLOAD_QUARANTINED;
        return MGMEM_PROVIDER_ERROR;
    }
    return MGMEM_OK;
}
