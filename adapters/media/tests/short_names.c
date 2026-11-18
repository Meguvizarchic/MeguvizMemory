#include <mgmem/media_read.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>

static unsigned called;
static void *expected_context;

static void on_error(void *context, int32_t status) {
    (void)context;
    (void)status;
}

static mgmem_status finish(unsigned which, uint64_t id, mgmem_compute_unmap_error_v1 callback,
                           void *context, mgmem_handle_v1 *owner, int32_t *status) {
    called = which;
    assert(callback == on_error && context == expected_context);
    assert(owner && status);
    owner->domain_id = 0;
    owner->id = 0;
    owner->runtime_origin = 0;
    *status = 0;
    if (id == 0) {
        *status = -42;
        return MGMEM_PROVIDER_ERROR;
    }
    owner->domain_id = 3;
    owner->id = 4;
    owner->runtime_origin = 5;
    return MGMEM_OK;
}

mgmem_status mgmem_compute_import_read_v1(
    mgmem_domain *domain, MgcLease *lease, uint64_t id, mgmem_compute_unmap_error_v1 callback,
    void *context, mgmem_handle_v1 *owner, int32_t *status) {
    (void)domain;
    (void)lease;
    return finish(1, id, callback, context, owner, status);
}

mgmem_status mgmem_media_import_read_v1(
    mgmem_domain *domain, MgmFrame *frame, uint64_t id, mgmem_compute_unmap_error_v1 callback,
    void *context, mgmem_handle_v1 *owner, int32_t *status) {
    (void)domain;
    (void)frame;
    return finish(2, id, callback, context, owner, status);
}

#if defined(MGMEM_TEST_SHORT_ARENA)
mgmem_status mgmem_compute_import_arena_read_v1(
    mgmem_domain *domain, MgcLease *lease, uint64_t id, mgmem_compute_unmap_error_v1 callback,
    void *context, mgmem_handle_v1 *owner, int32_t *status) {
    (void)domain;
    (void)lease;
    return finish(3, id, callback, context, owner, status);
}

mgmem_status mgmem_media_import_arena_read_v1(
    mgmem_domain *domain, MgmFrame *frame, uint64_t id, mgmem_compute_unmap_error_v1 callback,
    void *context, mgmem_handle_v1 *owner, int32_t *status) {
    (void)domain;
    (void)frame;
    return finish(4, id, callback, context, owner, status);
}
#endif

static void expect(mgmem_read_result result, unsigned which, int success) {
    assert(called == which);
    assert(result.status == (success ? MGMEM_OK : MGMEM_PROVIDER_ERROR));
    assert(result.origin_status == (success ? 0 : -42));
    assert(result.owner.domain_id == (success ? 3u : 0u));
    assert(result.owner.id == (success ? 4u : 0u));
    assert(result.owner.runtime_origin == (success ? 5u : 0u));
}

int main(void) {
    int context = 0;
    expected_context = &context;
    expect(mgmem_compute_read(0, 0, 123, on_error, &context), 1, 1);
    expect(mgmem_compute_read(0, 0, 0, on_error, &context), 1, 0);
    expect(mgmem_media_read(0, 0, 123, on_error, &context), 2, 1);
    expect(mgmem_media_read(0, 0, 0, on_error, &context), 2, 0);
#if defined(MGMEM_TEST_SHORT_ARENA)
    expect(mgmem_compute_read_arena(0, 0, 123, on_error, &context), 3, 1);
    expect(mgmem_compute_read_arena(0, 0, 0, on_error, &context), 3, 0);
    expect(mgmem_media_read_arena(0, 0, 123, on_error, &context), 4, 1);
    expect(mgmem_media_read_arena(0, 0, 0, on_error, &context), 4, 0);
#endif
#ifdef __cplusplus
    expect(mgmem::compute::read(nullptr, nullptr, 123, on_error, &context), 1, 1);
    expect(mgmem::media::read(nullptr, nullptr, 0, on_error, &context), 2, 0);
#if defined(MGMEM_TEST_SHORT_ARENA)
    expect(mgmem::compute::read_arena(nullptr, nullptr, 0, on_error, &context), 3, 0);
    expect(mgmem::media::read_arena(nullptr, nullptr, 123, on_error, &context), 4, 1);
#endif
#endif
    return 0;
}
