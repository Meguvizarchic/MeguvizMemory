#include <mgmem/compute_host.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

static int resource_marker;
static int context_marker;

static void on_error(void *context, mgmem_status status) {
    (void)context;
    (void)status;
}

mgmem_status mgmem_compute_create_host_resource_v1(
    mgmem_domain *domain, mgmem_handle_v1 allocation, uint64_t offset,
    MgcDomain *compute, const MgcResourceDesc *desc, uint32_t access,
    mgmem_compute_host_error_fn callback, void *context,
    MgcResource **out, int32_t *out_status) {
    (void)domain;
    (void)compute;
    assert(offset == 7 && desc && access == MGC_ACCESS_READ);
    assert(callback == on_error && context == &context_marker);
    assert(out && out_status);
    *out = 0;
    *out_status = 0;
    if (!allocation.id) {
        *out_status = -42;
        return MGMEM_PROVIDER_ERROR;
    }
    *out = (MgcResource *)&resource_marker;
    return MGMEM_OK;
}

static void expect(mgmem_host_result result, int success) {
    assert(result.status == (success ? MGMEM_OK : MGMEM_PROVIDER_ERROR));
    assert(result.origin_status == (success ? 0 : -42));
    assert(result.resource == (success ? (MgcResource *)&resource_marker : 0));
}

int main(void) {
    mgmem_handle_v1 allocation = {3, 4, 5};
    MgcResourceDesc desc = {0};
    expect(mgmem_compute_host(0, allocation, 7, 0, &desc, MGC_ACCESS_READ, on_error, &context_marker), 1);
    allocation.id = 0;
    expect(mgmem_compute_host(0, allocation, 7, 0, &desc, MGC_ACCESS_READ, on_error, &context_marker), 0);
#ifdef __cplusplus
    expect(mgmem::compute::host(nullptr, allocation, 7, nullptr, &desc, MGC_ACCESS_READ, on_error, &context_marker), 0);
    allocation.id = 4;
    expect(mgmem::compute::host(nullptr, allocation, 7, nullptr, &desc, MGC_ACCESS_READ, on_error, &context_marker), 1);
#endif
    return 0;
}
