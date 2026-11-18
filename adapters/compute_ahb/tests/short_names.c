#include <mgmem/ahb_map.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static int marker;

static void on_error(void *context, int32_t stage, int32_t status) {
    (void)context;
    (void)stage;
    (void)status;
}

mgmem_status mgmem_ahb_read(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn callback, void *context, mgmem_handle_v1 *owner,
    mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery, int32_t *stage, int32_t *status) {
    (void)domain;
    (void)lease;
    assert(timeout_ms == 12 && callback == on_error && context == &marker);
    assert(owner && layout && recovery && stage && status);
    if (!external_id) {
        *recovery = (mgmem_ahb_recovery *)&marker;
        *stage = MGMEM_AHB_STAGE_RETURN;
        *status = -42;
        return MGMEM_PROVIDER_ERROR;
    }
    owner->domain_id = 3;
    owner->id = 4;
    owner->runtime_origin = 5;
    layout->cpu_bytes = 64;
    layout->compute_bytes = 128;
    layout->row_stride = 32;
    layout->width = 4;
    layout->height = 2;
    return MGMEM_OK;
}

mgmem_status mgmem_ahb_write(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn callback, void *context, mgmem_handle_v1 *owner,
    mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery, int32_t *stage, int32_t *status) {
    return mgmem_ahb_read(domain, lease, external_id, timeout_ms, callback, context,
        owner, layout, recovery, stage, status);
}

static void verify(mgmem_ahb_result result, int ok) {
    assert(result.status == (ok ? MGMEM_OK : MGMEM_PROVIDER_ERROR));
    assert(result.owner.id == (ok ? 4u : 0u));
    assert(result.layout.cpu_bytes == (ok ? 64u : 0u));
    assert(result.layout.compute_bytes == (ok ? 128u : 0u));
    assert(result.layout.row_stride == (ok ? 32u : 0u));
    assert(result.layout.width == (ok ? 4u : 0u));
    assert(result.layout.height == (ok ? 2u : 0u));
    assert(result.recovery == (ok ? 0 : (mgmem_ahb_recovery *)&marker));
    assert(result.error_stage == (ok ? MGMEM_AHB_STAGE_NONE : MGMEM_AHB_STAGE_RETURN));
    assert(result.error_status == (ok ? 0 : -42));
}

int main(void) {
    verify(mgmem_ahb_map(0, 0, 1, 12, on_error, &marker), 1);
    verify(mgmem_ahb_map(0, 0, 0, 12, on_error, &marker), 0);
    verify(mgmem_ahb_map_write(0, 0, 1, 12, on_error, &marker), 1);
    verify(mgmem_ahb_map_write(0, 0, 0, 12, on_error, &marker), 0);
#ifdef __cplusplus
    verify(mgmem::ahb::read(nullptr, nullptr, 1, 12, on_error, &marker), 1);
    verify(mgmem::ahb::read(nullptr, nullptr, 0, 12, on_error, &marker), 0);
    verify(mgmem::ahb::write(nullptr, nullptr, 1, 12, on_error, &marker), 1);
    verify(mgmem::ahb::write(nullptr, nullptr, 0, 12, on_error, &marker), 0);
#endif
    return 0;
}
