#include <mgmem/media_read.h>

#include <stdint.h>

static void on_error(void *context, int32_t status) {
    (void)context;
    (void)status;
}

int main(void) {
    const mgmem_read_result first = mgmem_compute_read(0, 0, 1, on_error, 0);
    const mgmem_read_result second = mgmem_media_read(0, 0, 2, on_error, 0);
    if (first.status != MGMEM_INVALID_ARGUMENT || first.owner.id != 0 || first.origin_status != 0) return 1;
    if (second.status != MGMEM_INVALID_ARGUMENT || second.owner.id != 0 || second.origin_status != 0) return 2;
#ifdef __cplusplus
    const auto third = mgmem::compute::read(nullptr, nullptr, 1, on_error, nullptr);
    const auto fourth = mgmem::media::read(nullptr, nullptr, 2, on_error, nullptr);
    if (third.status != MGMEM_INVALID_ARGUMENT || third.owner.id != 0 || third.origin_status != 0) return 3;
    if (fourth.status != MGMEM_INVALID_ARGUMENT || fourth.owner.id != 0 || fourth.origin_status != 0) return 4;
#endif
    return 0;
}
