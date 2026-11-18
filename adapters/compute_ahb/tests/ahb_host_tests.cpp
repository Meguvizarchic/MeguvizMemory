#include <mgmem/ahb_slot.h>
#include <meguviz/compute_android.h>
#include <android/hardware_buffer.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); std::abort(); } } while (0)

struct AHardwareBuffer {
    AHardwareBuffer_Desc desc{4, 4, 1, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                             AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, 8, 0, 0};
    unsigned char bytes[128]{};
    bool locked = false;
    int lock_error = 0;
    int unlock_error = 0;
    int lock_count = 0;
    int unlock_count = 0;
    uint64_t last_usage = 0;
};
struct MgcLease {
    AHardwareBuffer *buffer;
    std::atomic<int> refs{1};
    MgcLeaseInfo info{};
    bool reserved = false;
    int released_fd = 11;
    int returned_fd = -99;
    MgcStatus return_error = MGC_SUCCESS;
};
static int waited = 0, wait_result = 0, closed = 0, close_fd_value = -1;
static int last_fault_stage = 0, last_fault_status = 0, faults = 0;
static void fault(void *, int32_t stage, int32_t status) {
    ++faults;
    last_fault_stage = stage;
    last_fault_status = status;
}
extern "C" int mgmem_ahb_test_wait(int fd, int) { CHECK(fd == 11); ++waited; return wait_result; }
extern "C" void mgmem_ahb_test_close(int fd) { ++closed; close_fd_value = fd; }
extern "C" void AHardwareBuffer_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *out) { *out = buffer->desc; }
extern "C" int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fd, const void *rect, void **data) {
    CHECK((usage == AHARDWAREBUFFER_USAGE_CPU_READ_RARELY ||
           usage == (AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY)) && fd == -1 && !rect);
    buffer->last_usage = usage;
    ++buffer->lock_count;
    if (buffer->lock_error) return buffer->lock_error;
    CHECK(!buffer->locked);
    buffer->locked = true;
    *data = buffer->bytes;
    return 0;
}
extern "C" int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fd) {
    CHECK(buffer->locked && !fd);
    ++buffer->unlock_count;
    if (buffer->unlock_error) return buffer->unlock_error;
    buffer->locked = false;
    return 0;
}
extern "C" void mgc_lease_retain(MgcLease *lease) { ++lease->refs; }
extern "C" void mgc_lease_release(MgcLease *lease) { --lease->refs; }
extern "C" MgcStatus mgc_lease_info(MgcLease *lease, MgcLeaseInfo *info) { *info = lease->info; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_get_buffer(MgcLease *lease, AHardwareBuffer **out) { *out = lease->buffer; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_release_to_external(MgcLease *lease, int32_t *out) {
    if (lease->reserved) return MGC_BUSY;
    lease->reserved = true;
    ++lease->refs;
    *out = lease->released_fd;
    return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_set_acquire_fence(MgcLease *lease, int32_t fd) {
    CHECK(lease->reserved);
    CHECK(!lease->buffer->locked);
    lease->returned_fd = fd;
    if (lease->return_error != MGC_SUCCESS) return lease->return_error;
    lease->reserved = false;
    --lease->refs;
    return MGC_SUCCESS;
}

static mgmem_domain *make_domain(uint64_t budget = 128) {
    mgmem_domain_options_v1 opts{};
    opts.struct_size = sizeof(opts);
    opts.abi_major = MGMEM_ABI_MAJOR;
    opts.abi_minor = MGMEM_ABI_MINOR;
    opts.mode = MGMEM_SYSTEM;
    opts.budget_bytes = budget;
    mgmem_domain *domain = nullptr;
    CHECK(mgmem_domain_create_v1(&opts, &domain) == MGMEM_OK);
    return domain;
}
static void close_domain(mgmem_domain *domain) {
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_drain_v1(domain, 0) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
}
static void init_lease(MgcLease &lease) {
    lease.info.header = MGC_HEADER_INIT(MgcLeaseInfo);
    lease.info.width = 4;
    lease.info.height = 4;
    lease.info.row_stride_bytes = 32;
    lease.info.capacity_bytes = 256;
    lease.info.byte_length = 256;
    lease.info.access = MGC_ACCESS_READ;
    lease.info.format = MGC_FORMAT_RGBA8;
}

struct Outcome {
    mgmem_handle_v1 owner{};
    mgmem_ahb_layout layout{};
    mgmem_ahb_recovery *recovery = nullptr;
    int32_t stage = -1, status = -1;
    mgmem_status call(mgmem_domain *domain, MgcLease &lease, uint64_t id, int timeout = 0) {
        return mgmem_ahb_read(domain, &lease, id, timeout, fault, nullptr, &owner, &layout, &recovery, &stage, &status);
    }
    mgmem_status write(mgmem_domain *domain, MgcLease &lease, uint64_t id, int timeout = 0) {
        return mgmem_ahb_write(domain, &lease, id, timeout, fault, nullptr, &owner, &layout, &recovery, &stage, &status);
    }
};

int main(int argc, char **argv) {
    const bool slot_mode = argc == 2 && std::strcmp(argv[1], "--slot") == 0;
    const bool slot_failure = argc == 2 && std::strcmp(argv[1], "--slot-failure") == 0;
    const bool wait_error = argc == 2 && std::strcmp(argv[1], "--wait-error") == 0;
    const bool slot_wait_error = argc == 2 && std::strcmp(argv[1], "--slot-wait-error") == 0;
    const bool wait_unverified = argc == 2 && std::strcmp(argv[1], "--wait-unverified") == 0;
    const bool slot_wait_unverified = argc == 2 && std::strcmp(argv[1], "--slot-wait-unverified") == 0;
    const bool write_failure = argc == 2 && (std::strcmp(argv[1], "--write-unlock-failure") == 0 || std::strcmp(argv[1], "--write-return-failure") == 0);
    const bool unlock_failure = argc == 2 && (std::strcmp(argv[1], "--unlock-failure") == 0 || std::strcmp(argv[1], "--write-unlock-failure") == 0);
    const bool return_failure = argc == 2 && (std::strcmp(argv[1], "--return-failure") == 0 || std::strcmp(argv[1], "--write-return-failure") == 0);
    const bool write = argc == 2 && std::strcmp(argv[1], "--write") == 0;
    CHECK(argc == 1 || unlock_failure || return_failure || write || slot_mode || slot_failure || wait_error || slot_wait_error || wait_unverified || slot_wait_unverified);
    AHardwareBuffer buffer;
    MgcLease lease{&buffer};
    init_lease(lease);
    auto *domain = make_domain();
    Outcome out;

    if (wait_error || slot_wait_error || wait_unverified || slot_wait_unverified) {
        mgmem_ahb_slot *slot = nullptr;
        const bool use_slot = slot_wait_error || slot_wait_unverified;
        if (use_slot) CHECK(mgmem_ahb_slot_create(&slot) == MGMEM_OK);
        wait_result = (wait_unverified || slot_wait_unverified) ? 2 : -1;
        auto status = use_slot
            ? mgmem_ahb_slot_map_v1(slot, domain, &lease, 200, 0, fault, nullptr,
                &out.owner, &out.layout, &out.recovery, &out.stage, &out.status, 0)
            : out.call(domain, lease, 200);
        CHECK(status == MGMEM_PROVIDER_ERROR && out.recovery && out.stage == MGMEM_AHB_STAGE_WAIT);
        CHECK(faults == 1 && last_fault_stage == MGMEM_AHB_STAGE_WAIT && last_fault_status == wait_result);
        CHECK(lease.reserved && lease.refs == 3 && lease.returned_fd == -99 && closed == 0 && !buffer.locked);
        CHECK(mgmem_ahb_recover(&out.recovery) == MGMEM_PROVIDER_ERROR && out.recovery);
        if (slot) CHECK(mgmem_ahb_slot_close(&slot) == MGMEM_BUSY && slot);
        std::puts("PASS failed producer fence remains quarantined");
        std::fflush(stdout); std::_Exit(0);
    }

    if (slot_mode || slot_failure) {
        mgmem_ahb_slot *slot = nullptr;
        CHECK(mgmem_ahb_slot_create(&slot) == MGMEM_OK && slot);
        auto mapped = mgmem_ahb_slot_read(slot, domain, &lease, 201, 0, fault, nullptr);
        CHECK(mapped.status == MGMEM_OK && mapped.owner.id && lease.reserved && buffer.locked);
        CHECK(mgmem_ahb_slot_close(&slot) == MGMEM_BUSY && slot);
        CHECK(mgmem_ahb_slot_read(slot, domain, &lease, 202, 0, fault, nullptr).status == MGMEM_BUSY);
        if (slot_failure) {
            buffer.unlock_error = -22;
            CHECK(mgmem_release_v1(domain, mapped.owner) == MGMEM_PROVIDER_ERROR);
            CHECK(faults == 1 && last_fault_stage == MGMEM_AHB_STAGE_UNLOCK);
            CHECK(mgmem_ahb_slot_close(&slot) == MGMEM_BUSY && slot && lease.reserved);
            CHECK(mgmem_ahb_slot_read(slot, domain, &lease, 203, 0, fault, nullptr).status == MGMEM_BUSY);
            std::puts("PASS uncertain cleanup quarantines AHB slot");
            std::fflush(stdout); std::_Exit(0);
        }
        mgmem_borrow_v1 held{};
        CHECK(mgmem_borrow_acquire_v1(domain, mapped.owner, 0, 128, MGMEM_READ, &held) == MGMEM_OK);
        CHECK(mgmem_release_v1(domain, mapped.owner) == MGMEM_OK);
        CHECK(mgmem_ahb_slot_close(&slot) == MGMEM_BUSY && lease.reserved && buffer.locked);
        std::thread last([&] { CHECK(mgmem_borrow_release_v1(domain, held.lease) == MGMEM_OK); });
        last.join();
        CHECK(!lease.reserved && !buffer.locked && lease.refs == 1);
        lease.info.access = MGC_ACCESS_WRITE;
        buffer.desc.usage |= AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
        mapped = mgmem::ahb::write(slot, domain, &lease, 204, 0, fault, nullptr);
        CHECK(mapped.status == MGMEM_OK && lease.reserved && buffer.locked);
        CHECK(mgmem_release_v1(domain, mapped.owner) == MGMEM_OK);
        CHECK(!lease.reserved && !buffer.locked);
        wait_result = 1; lease.return_error = MGC_BUSY;
        mapped = mgmem_ahb_slot_write(slot, domain, &lease, 205, 0, fault, nullptr);
        CHECK(mapped.status == MGMEM_PROVIDER_ERROR && mapped.recovery && lease.reserved);
        CHECK(mgmem_ahb_slot_close(&slot) == MGMEM_BUSY && slot);
        lease.return_error = MGC_SUCCESS;
        CHECK(mgmem_ahb_recover(&mapped.recovery) == MGMEM_OK && !mapped.recovery && !lease.reserved);
        CHECK(mgmem_ahb_slot_close(&slot) == MGMEM_OK && !slot);
        close_domain(domain);
        std::puts("PASS reusable AHB slot retains reservations through final release");
        return 0;
    }

    if (write) {
        CHECK(out.write(domain, lease, 100) == MGMEM_UNSUPPORTED && !lease.reserved);
        lease.info.access = MGC_ACCESS_WRITE;
        CHECK(out.write(domain, lease, 101) == MGMEM_UNSUPPORTED && !lease.reserved);
        buffer.desc.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
        CHECK(out.write(domain, lease, 102) == MGMEM_UNSUPPORTED && !lease.reserved);
        buffer.desc.usage |= AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
        wait_result = 1;
        CHECK(out.write(domain, lease, 103) == MGMEM_TIMEOUT && !lease.reserved && !buffer.locked);
        CHECK(lease.returned_fd == 11 && lease.refs == 1);
        lease.return_error = MGC_BUSY;
        CHECK(out.write(domain, lease, 105) == MGMEM_PROVIDER_ERROR && out.recovery && lease.reserved);
        lease.return_error = MGC_SUCCESS;
        CHECK(mgmem_ahb_recover(&out.recovery) == MGMEM_OK && !lease.reserved && !out.recovery);
        wait_result = 0;
        buffer.lock_error = -5;
        CHECK(out.write(domain, lease, 106) == MGMEM_PROVIDER_ERROR && out.stage == MGMEM_AHB_STAGE_LOCK);
        CHECK(!out.recovery && !lease.reserved && !buffer.locked && lease.returned_fd == -1 && lease.refs == 1);
        buffer.lock_error = 0;
        CHECK(mgmem_domain_set_budget_v1(domain, 127) == MGMEM_OK);
        const int unlocks_before_budget_rejection = buffer.unlock_count;
        CHECK(out.write(domain, lease, 107) == MGMEM_OUT_OF_BUDGET);
        CHECK(!out.recovery && !lease.reserved && !buffer.locked && lease.refs == 1);
        CHECK(buffer.unlock_count == unlocks_before_budget_rejection + 1);
        CHECK(mgmem_domain_set_budget_v1(domain, 128) == MGMEM_OK);
        CHECK(out.write(domain, lease, 104) == MGMEM_OK && !out.recovery);
        CHECK(buffer.last_usage == (AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY));
        CHECK(out.layout.cpu_bytes == 128 && lease.reserved && buffer.locked);
        mgmem_borrow_v1 view{}, other{};
        CHECK(mgmem_borrow_acquire_v1(domain, out.owner, 0, 128, MGMEM_READ, &view) == MGMEM_OK);
        CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
        CHECK(mgmem_borrow_acquire_v1(domain, out.owner, 0, 128, MGMEM_WRITE, &view) == MGMEM_OK);
        CHECK(mgmem_borrow_acquire_v1(domain, out.owner, 0, 1, MGMEM_READ, &other) == MGMEM_BUSY);
        std::memset(view.data, 0xa5, static_cast<size_t>(view.length));
        CHECK(mgmem_release_v1(domain, out.owner) == MGMEM_OK);
        CHECK(lease.reserved && buffer.locked);
        std::thread last([&] { CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK); });
        last.join();
        CHECK(!lease.reserved && !buffer.locked && lease.returned_fd == -1 && lease.refs == 1);
        for (unsigned char byte : buffer.bytes) CHECK(byte == 0xa5);
        close_domain(domain);
        std::puts("PASS AHB WRITE usage, synchronization and lifetime");
        return 0;
    }

    if (unlock_failure || return_failure) {
        if (write_failure) {
            lease.info.access = MGC_ACCESS_WRITE;
            buffer.desc.usage |= AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
        }
        CHECK((write_failure ? out.write(domain, lease, 91) : out.call(domain, lease, 91)) == MGMEM_OK);
        if (unlock_failure) buffer.unlock_error = -22;
        if (return_failure) lease.return_error = MGC_BUSY;
        CHECK(mgmem_release_v1(domain, out.owner) == MGMEM_PROVIDER_ERROR);
        CHECK(faults == 1 && last_fault_stage == (unlock_failure ? MGMEM_AHB_STAGE_UNLOCK : MGMEM_AHB_STAGE_RETURN));
        CHECK(lease.reserved && lease.refs.load() == 3);
        mgmem_import_snapshot_v1 snapshot{};
        CHECK(mgmem_domain_import_snapshot_v1(domain, &snapshot, sizeof(snapshot)) == MGMEM_OK);
        CHECK(snapshot.poisoned && snapshot.external_backing_bytes == 128);
        CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_PROVIDER_ERROR);
        std::puts("PASS uncertain cleanup retains reservation and Memory claim");
        std::fflush(stdout);
        std::_Exit(0);
    }

    lease.info.access = MGC_ACCESS_WRITE;
    CHECK(out.call(domain, lease, 1) == MGMEM_UNSUPPORTED && !lease.reserved);
    lease.info.access = MGC_ACCESS_READ;
    buffer.desc.usage = AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    CHECK(out.call(domain, lease, 2) == MGMEM_UNSUPPORTED && !lease.reserved);
    buffer.desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;
    buffer.desc.stride = 9;
    CHECK(out.call(domain, lease, 3) == MGMEM_UNSUPPORTED && !lease.reserved);
    buffer.desc.stride = 8;

    wait_result = 1;
    CHECK(out.call(domain, lease, 4) == MGMEM_TIMEOUT && out.stage == MGMEM_AHB_STAGE_WAIT);
    CHECK(lease.returned_fd == 11 && closed == 1 && close_fd_value == 11 && !lease.reserved && lease.refs == 1);
    lease.return_error = MGC_BUSY;
    CHECK(out.call(domain, lease, 5) == MGMEM_PROVIDER_ERROR && out.recovery && out.stage == MGMEM_AHB_STAGE_RETURN);
    CHECK(lease.reserved && lease.refs == 3 && closed == 1);
    lease.return_error = MGC_SUCCESS;
    CHECK(mgmem_ahb_recover(&out.recovery) == MGMEM_OK && !out.recovery);
    CHECK(!lease.reserved && lease.returned_fd == 11 && closed == 2 && lease.refs == 1);

    wait_result = 0;
    buffer.lock_error = -5;
    CHECK(out.call(domain, lease, 6) == MGMEM_PROVIDER_ERROR && out.stage == MGMEM_AHB_STAGE_LOCK);
    CHECK(!lease.reserved && lease.returned_fd == -1 && !buffer.locked && closed == 3 && lease.refs == 1);
    buffer.lock_error = 0;
    CHECK(mgmem_domain_set_budget_v1(domain, 127) == MGMEM_OK);
    CHECK(out.call(domain, lease, 7) == MGMEM_OUT_OF_BUDGET && !out.recovery && !lease.reserved && !buffer.locked);
    CHECK(mgmem_domain_set_budget_v1(domain, 128) == MGMEM_OK);

    lease.released_fd = -1;
    CHECK(out.call(domain, lease, 70) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, out.owner) == MGMEM_OK);
    CHECK(!lease.reserved && lease.returned_fd == -1 && !buffer.locked && lease.refs == 1);
    lease.released_fd = 11;

    for (unsigned i = 0; i < sizeof(buffer.bytes); ++i) buffer.bytes[i] = static_cast<unsigned char>(i);
    CHECK(out.call(domain, lease, 8) == MGMEM_OK && !out.recovery && out.owner.id);
    CHECK(out.layout.cpu_bytes == 128 && out.layout.compute_bytes == 256 && out.layout.row_stride == 32);
    CHECK(lease.reserved && buffer.locked && lease.refs == 3);
    mgmem_borrow_v1 view{};
    CHECK(mgmem_borrow_acquire_v1(domain, out.owner, 0, 128, MGMEM_WRITE, &view) != MGMEM_OK);
    CHECK(mgmem_borrow_acquire_v1(domain, out.owner, 0, 128, MGMEM_READ, &view) == MGMEM_OK);
    CHECK(mgmem_release_v1(domain, out.owner) == MGMEM_OK);
    CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
    CHECK(lease.reserved && buffer.locked);
    for (unsigned i = 0; i < view.length; ++i) CHECK(static_cast<const unsigned char *>(view.data)[i] == i);
    std::thread last([&] { CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK); });
    last.join();
    CHECK(!lease.reserved && !buffer.locked && lease.returned_fd == -1 && lease.refs == 1);
    CHECK(waited == 5 && closed == 5);
    CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
    CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
    std::puts("PASS AHB READ ownership, timeout, recovery and budget");
}
