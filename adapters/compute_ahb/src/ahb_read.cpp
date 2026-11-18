#include <mgmem/ahb_slot.h>
#include <meguviz/compute_android.h>
#include <android/hardware_buffer.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <new>

#if defined(MGMEM_AHB_TESTING)
extern "C" int mgmem_ahb_test_wait(int fd, int timeout_ms);
extern "C" void mgmem_ahb_test_close(int fd);
#else
#include "../../common/sync_file_wait.hpp"
#include <unistd.h>
#endif

struct mgmem_ahb_recovery {
    std::atomic<unsigned> references{1};
    std::atomic<bool> transferred{false};
    std::atomic<int32_t> error_stage{MGMEM_AHB_STAGE_NONE};
    std::atomic<int32_t> error_status{0};
    MgcLease *lease;
    AHardwareBuffer *buffer;
    mgmem_ahb_error_fn on_error;
    void *error_context;
    int ready_fd = -1;
    bool locked = false;
    bool uncertain = false;
    struct mgmem_ahb_slot *slot = nullptr;
};

struct mgmem_ahb_slot {
    mgmem_ahb_recovery state{};
    std::atomic<bool> busy{false};
};

namespace {
void close_fd(int fd) noexcept {
    if (fd < 0) return;
#if defined(MGMEM_AHB_TESTING)
    mgmem_ahb_test_close(fd);
#else
    (void)close(fd);
#endif
}

/* 0: confirmed, 1: timeout before signal, 2: signaled but unverified,
 * negative: error. The caller retains fd ownership. */
int wait_fd(int fd, int timeout_ms) noexcept {
    if (fd == -1) return 0;
    if (fd < -1) return -1;
#if defined(MGMEM_AHB_TESTING)
    return mgmem_ahb_test_wait(fd, timeout_ms);
#else
    return mgmem_detail::sync_file_wait(fd, timeout_ms);
#endif
}

void notify(mgmem_ahb_recovery *state, int32_t stage, int32_t status) noexcept {
    state->error_status.store(status, std::memory_order_release);
    state->error_stage.store(stage, std::memory_order_release);
    state->on_error(state->error_context, stage, status);
}

void release_reference(mgmem_ahb_recovery *state) noexcept {
    if (state->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    auto *slot = state->slot;
    if (slot) slot->busy.store(false, std::memory_order_release);
    else delete state;
}

mgmem_status retain_mapping(void *context) noexcept {
    auto *state = static_cast<mgmem_ahb_recovery *>(context);
    state->references.fetch_add(1, std::memory_order_relaxed);
    state->transferred.store(true, std::memory_order_release);
    return MGMEM_OK;
}

/* A ready fd is returned to Compute if the wait did not complete. */
bool finish(mgmem_ahb_recovery *state) noexcept {
    if (state->uncertain) return false;
    if (state->locked) {
        const int status = AHardwareBuffer_unlock(state->buffer, nullptr);
        if (status != 0) {
            state->uncertain = true;
            notify(state, MGMEM_AHB_STAGE_UNLOCK, status);
            return false;
        }
        state->locked = false;
    }
    const MgcStatus status = mgc_android_set_acquire_fence(state->lease, state->ready_fd);
    if (status != MGC_SUCCESS) {
        notify(state, MGMEM_AHB_STAGE_RETURN, status);
        return false;
    }
    close_fd(state->ready_fd);
    state->ready_fd = -1;
    mgc_lease_release(state->lease);
    state->lease = nullptr;
    return true;
}

mgmem_status release_mapping(void *context) noexcept {
    auto *state = static_cast<mgmem_ahb_recovery *>(context);
    if (!finish(state)) return MGMEM_PROVIDER_ERROR;
    release_reference(state);
    return MGMEM_OK;
}

mgmem_status compute_status(MgcStatus status) noexcept {
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

static mgmem_status map_ahb(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context,
    mgmem_handle_v1 *out, mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery,
    int32_t *error_stage, int32_t *error_status, bool write, mgmem_ahb_slot *slot) {
    if (out) *out = {};
    if (layout) *layout = {};
    if (recovery) *recovery = nullptr;
    if (error_stage) *error_stage = MGMEM_AHB_STAGE_NONE;
    if (error_status) *error_status = 0;
    if (!domain || !lease || !external_id || timeout_ms < -1 || !on_error || !out || !layout || !recovery || !error_stage || !error_status)
        return MGMEM_INVALID_ARGUMENT;
    if (!(mgmem_capabilities_v1() & MGMEM_CAP_CHECKED_EXTERNAL_RELEASE)) return MGMEM_UNSUPPORTED;

    MgcLeaseInfo info{};
    info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    MgcStatus status = mgc_lease_info(lease, &info);
    if (status != MGC_SUCCESS) {
        *error_stage = MGMEM_AHB_STAGE_COMPUTE;
        *error_status = status;
        return compute_status(status);
    }
    if (info.header.struct_size < sizeof(info) || info.header.abi_major != MGC_ABI_MAJOR ||
        info.access != (write ? MGC_ACCESS_WRITE : MGC_ACCESS_READ) || info.format != MGC_FORMAT_RGBA8 ||
        info.byte_offset != 0 || !info.capacity_bytes || info.byte_length != info.capacity_bytes)
        return MGMEM_UNSUPPORTED;

    AHardwareBuffer *buffer = nullptr;
    status = mgc_android_get_buffer(lease, &buffer);
    if (status != MGC_SUCCESS || !buffer) {
        *error_stage = MGMEM_AHB_STAGE_COMPUTE;
        *error_status = status;
        return status == MGC_SUCCESS ? MGMEM_PROVIDER_ERROR : compute_status(status);
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    const uint64_t stride = static_cast<uint64_t>(desc.stride) * 4;
    const uint64_t read_usage = desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK;
    const uint64_t write_usage = desc.usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK;
    if (!desc.width || !desc.height || desc.layers != 1 || desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        desc.stride < desc.width || (desc.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) ||
        (read_usage != AHARDWAREBUFFER_USAGE_CPU_READ_RARELY && read_usage != AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN) ||
        (write && write_usage != AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY && write_usage != AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN) ||
        desc.width != info.width || desc.height != info.height || stride != info.row_stride_bytes ||
        stride > std::numeric_limits<uint64_t>::max() / desc.height || stride * desc.height > info.capacity_bytes)
        return MGMEM_UNSUPPORTED;

    mgmem_ahb_recovery *state = nullptr;
    if (slot) {
        bool expected = false;
        if (!slot->busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return MGMEM_BUSY;
        state = &slot->state;
        state->references.store(1, std::memory_order_relaxed);
        state->transferred.store(false, std::memory_order_relaxed);
        state->error_stage.store(MGMEM_AHB_STAGE_NONE, std::memory_order_relaxed);
        state->error_status.store(0, std::memory_order_relaxed);
        state->ready_fd = -1;
        state->locked = false;
        state->uncertain = false;
        state->slot = slot;
    } else {
        state = new (std::nothrow) mgmem_ahb_recovery{};
        if (!state) return MGMEM_OUT_OF_MEMORY;
    }
    state->lease = lease;
    state->buffer = buffer;
    state->on_error = on_error;
    state->error_context = error_context;
    mgc_lease_retain(lease);
    status = mgc_android_release_to_external(lease, &state->ready_fd);
    if (status != MGC_SUCCESS) {
        close_fd(state->ready_fd);
        mgc_lease_release(lease);
        release_reference(state);
        *error_stage = MGMEM_AHB_STAGE_COMPUTE;
        *error_status = status;
        return compute_status(status);
    }

    mgmem_status result = MGMEM_OK;
    const int waited = wait_fd(state->ready_fd, timeout_ms);
    if (waited != 0 && waited != 1) {
        // A failed or uninspectable producer fence is not evidence that its
        // GPU work completed. Returning it to a poll-only consumer could
        // publish corrupted or still-busy content, so retain the reservation.
        state->uncertain = true;
        *error_stage = MGMEM_AHB_STAGE_WAIT;
        *error_status = waited;
        notify(state, MGMEM_AHB_STAGE_WAIT, waited);
        *recovery = state;
        return MGMEM_PROVIDER_ERROR;
    }
    if (waited != 0) {
        *error_stage = MGMEM_AHB_STAGE_WAIT;
        *error_status = waited;
        result = MGMEM_TIMEOUT;
    } else {
        close_fd(state->ready_fd);
        state->ready_fd = -1;
        void *data = nullptr;
        const uint64_t usage = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
            (write ? AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY : 0);
        const int locked = AHardwareBuffer_lock(buffer, usage, -1, nullptr, &data);
        if (locked != 0 || !data) {
            state->locked = locked == 0;
            *error_stage = MGMEM_AHB_STAGE_LOCK;
            *error_status = locked != 0 ? locked : -1;
            result = MGMEM_PROVIDER_ERROR;
        } else {
            state->locked = true;
            mgmem_external_buffer_checked_v1 external{};
            external.struct_size = sizeof(external);
            external.abi_major = MGMEM_ABI_MAJOR;
            external.abi_minor = MGMEM_ABI_MINOR;
            external.data = data;
            external.capacity_bytes = stride * desc.height;
            external.alignment = 1;
            external.external_id = external_id;
            external.access = write ? MGMEM_WRITE : MGMEM_READ;
            external.context = state;
            external.retain = retain_mapping;
            external.release_checked = release_mapping;
            external.managing_runtime_origin = mgmem_runtime_origin_v1();
            result = mgmem_import_external_checked_v1(domain, &external, out);
            if (result == MGMEM_OK) {
                *layout = {external.capacity_bytes, info.capacity_bytes, stride, desc.width, desc.height};
            }
        }
    }
    if (!state->transferred.load(std::memory_order_acquire)) {
        if (!finish(state)) {
            *error_stage = state->error_stage.load(std::memory_order_acquire);
            *error_status = state->error_status.load(std::memory_order_acquire);
            *recovery = state;
            return MGMEM_PROVIDER_ERROR;
        }
    } else {
        const auto stage = state->error_stage.load(std::memory_order_acquire);
        if (stage != MGMEM_AHB_STAGE_NONE) {
            *error_stage = stage;
            *error_status = state->error_status.load(std::memory_order_acquire);
        }
    }
    release_reference(state);
    return result;
}

extern "C" mgmem_status mgmem_ahb_read(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context,
    mgmem_handle_v1 *out, mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery,
    int32_t *error_stage, int32_t *error_status) {
    return map_ahb(domain, lease, external_id, timeout_ms, on_error, error_context,
        out, layout, recovery, error_stage, error_status, false, nullptr);
}

extern "C" mgmem_status mgmem_ahb_write(mgmem_domain *domain, MgcLease *lease, uint64_t external_id,
    int32_t timeout_ms, mgmem_ahb_error_fn on_error, void *error_context,
    mgmem_handle_v1 *out, mgmem_ahb_layout *layout, mgmem_ahb_recovery **recovery,
    int32_t *error_stage, int32_t *error_status) {
    return map_ahb(domain, lease, external_id, timeout_ms, on_error, error_context,
        out, layout, recovery, error_stage, error_status, true, nullptr);
}

extern "C" mgmem_status mgmem_ahb_slot_create(mgmem_ahb_slot **out) {
    if (!out) return MGMEM_INVALID_ARGUMENT;
    *out = new (std::nothrow) mgmem_ahb_slot{};
    return *out ? MGMEM_OK : MGMEM_OUT_OF_MEMORY;
}

extern "C" mgmem_status mgmem_ahb_slot_close(mgmem_ahb_slot **slot) {
    if (!slot || !*slot) return MGMEM_INVALID_ARGUMENT;
    if ((*slot)->busy.load(std::memory_order_acquire)) return MGMEM_BUSY;
    delete *slot;
    *slot = nullptr;
    return MGMEM_OK;
}

extern "C" mgmem_status mgmem_ahb_slot_map_v1(mgmem_ahb_slot *slot, mgmem_domain *domain,
    MgcLease *lease, uint64_t external_id, int32_t timeout_ms, mgmem_ahb_error_fn on_error,
    void *error_context, mgmem_handle_v1 *out, mgmem_ahb_layout *layout,
    mgmem_ahb_recovery **recovery, int32_t *error_stage, int32_t *error_status, uint32_t write) {
    if (!slot || write > 1) {
        if (out) *out = {};
        if (layout) *layout = {};
        if (recovery) *recovery = nullptr;
        if (error_stage) *error_stage = MGMEM_AHB_STAGE_NONE;
        if (error_status) *error_status = 0;
        return MGMEM_INVALID_ARGUMENT;
    }
    return map_ahb(domain, lease, external_id, timeout_ms, on_error, error_context,
        out, layout, recovery, error_stage, error_status, write != 0, slot);
}

extern "C" mgmem_status mgmem_ahb_recover(mgmem_ahb_recovery **recovery) {
    if (!recovery || !*recovery) return MGMEM_INVALID_ARGUMENT;
    auto *state = *recovery;
    if (!finish(state)) return MGMEM_PROVIDER_ERROR;
    *recovery = nullptr;
    release_reference(state);
    return MGMEM_OK;
}
