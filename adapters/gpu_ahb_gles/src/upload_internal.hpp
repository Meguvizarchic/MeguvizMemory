#ifndef MGMEM_AHB_UPLOAD_INTERNAL_HPP
#define MGMEM_AHB_UPLOAD_INTERNAL_HPP

#include <mgmem/ahb_upload.h>
#include "platform.hpp"

/* Private state is shared only by this adapter's translation units. A live
 * reservation retains write_lease; callers must serialize token operations. */
struct mgmem_ahb_upload {
    mgmem_gpu_ahb *reservation = nullptr;
    mgmem_upload_platform *platform = nullptr;
    MgcLease *write_lease = nullptr;
    int producer_fd = -1;
    uint32_t phase = MGMEM_UPLOAD_NONE;
    bool producer_verified = false;
    bool platform_cleaned = false;
    /* Installed only by the optional guarded adapter, which binds this
     * callback to the same Compute runtime as the reservation. */
    mgmem_status (*guard_finish)(MgcLease *, uint64_t, bool, int32_t *) = nullptr;
    mgmem_status (*guard_retire)(mgmem_gpu_ahb **, int32_t) = nullptr;
    uint64_t guard_generation = 0;
    bool guarded_published = false;
};

#endif
