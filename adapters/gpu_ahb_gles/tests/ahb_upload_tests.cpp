#include <mgmem/ahb_upload.h>
#include "../src/platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

struct mgmem_gpu_ahb { int value; };
struct mgmem_upload_platform { int value; };
static mgmem_gpu_ahb reservation{};
static unsigned char source[64]{};
static std::string calls;
static int ready_result, completion_result, cancel_fail, publish_fail;
static mgmem_status begin_status = MGMEM_OK, open_status = MGMEM_OK;
static mgmem_status write_status = MGMEM_OK, fence_status = MGMEM_OK;
static mgmem_status cleanup_status = MGMEM_OK, retire_status = MGMEM_OK;
static bool hard_begin;
static unsigned width = 4, height = 4;
static uint64_t view_length = 64, expected_stride = 16;

mgmem_status mgmem_upload_platform_open(mgmem_upload_platform **out, int32_t *) noexcept {
    calls += 'O';
    if (open_status != MGMEM_OK) { *out = nullptr; return open_status; }
    *out = new mgmem_upload_platform{};
    return MGMEM_OK;
}
mgmem_status mgmem_upload_platform_write(mgmem_upload_platform *p, const mgmem_gpu_ahb_view &v,
                                         uint64_t stride, int32_t *) noexcept {
    calls += 'W'; CHECK(p && v.data == source && v.length == view_length && stride == expected_stride);
    return write_status;
}
mgmem_status mgmem_upload_platform_fence(mgmem_upload_platform *p, int *fd, int32_t *) noexcept {
    calls += 'F'; CHECK(p); *fd = fence_status == MGMEM_OK ? 20 : -1; return fence_status;
}
mgmem_status mgmem_upload_platform_cleanup(mgmem_upload_platform *p, int32_t *) noexcept {
    calls += 'C'; CHECK(p); return cleanup_status;
}
void mgmem_upload_platform_discard(mgmem_upload_platform *p) noexcept { calls += 'D'; delete p; }
int mgmem_upload_platform_wait(int fd, int) noexcept {
    if (fd == 11) { calls += 'R'; return ready_result; }
    CHECK(fd == 20); calls += 'S'; return completion_result;
}
int mgmem_upload_platform_probe(int fd) noexcept {
    CHECK(fd == 20); calls += 'S'; return completion_result;
}
void mgmem_upload_platform_close_fd(int fd) noexcept { CHECK(fd == 20); calls += 'X'; }

extern "C" mgmem_gpu_ahb_result mgmem_gpu_ahb_begin(mgmem_domain *, mgmem_handle_v1,
    uint64_t, uint64_t, MgcLease *, mgmem_gpu_ahb_error_fn, void *) {
    calls += 'B'; mgmem_gpu_ahb_result r{}; r.status = begin_status;
    if (begin_status == MGMEM_OK) {
        r.pending = &reservation;
        r.view = {source, view_length, reinterpret_cast<AHardwareBuffer *>(1), 32, width, height, 11};
    } else if (hard_begin) r.pending = &reservation;
    return r;
}
extern "C" mgmem_status mgmem_gpu_ahb_publish(mgmem_gpu_ahb *p, int32_t fd, int32_t *status) {
    calls += 'P'; CHECK(p == &reservation && fd == -1 && status);
    if (publish_fail) { *status = 1; return MGMEM_BUSY; }
    *status = 0; return MGMEM_OK;
}
extern "C" mgmem_status mgmem_gpu_ahb_retire(mgmem_gpu_ahb **p, int32_t timeout) {
    calls += 'T'; CHECK(p && *p == &reservation && timeout == 0);
    if (retire_status == MGMEM_OK) *p = nullptr;
    return retire_status;
}
extern "C" mgmem_status mgmem_gpu_ahb_cancel(mgmem_gpu_ahb **p, int32_t *status) {
    calls += 'A'; CHECK(p && *p == &reservation && status);
    if (cancel_fail == 2) { *status = 0; return MGMEM_PROVIDER_ERROR; }
    if (cancel_fail) { *status = 1; return MGMEM_BUSY; }
    *status = 0; *p = nullptr; return MGMEM_OK;
}
static void fault(void *, mgmem_status) { std::abort(); }
static mgmem_ahb_upload_desc desc() {
    mgmem_ahb_upload_desc d{};
    d.memory = reinterpret_cast<mgmem_domain *>(1); d.write_lease = reinterpret_cast<MgcLease *>(1);
    d.length = 64; d.source_stride = 16; d.width = 4; d.height = 4;
    d.ready_timeout_ms = 0; d.on_error = fault;
    return d;
}

int main(int argc, char **argv) {
    auto d = desc();
    if (argc == 2 && std::strcmp(argv[1], "sizes") == 0) {
        const unsigned dimensions[][2] = {{1280, 720}, {2560, 1440}};
        for (const auto &size : dimensions) {
            width = size[0]; height = size[1];
            expected_stride = uint64_t(width + 2) * 4;
            view_length = uint64_t(height - 1) * expected_stride + uint64_t(width) * 4;
            d.width = width; d.height = height; d.source_stride = expected_stride; d.length = view_length - 1;
            calls.clear();
            CHECK(mgmem_ahb_upload_submit(&d).status == MGMEM_INVALID_ARGUMENT && calls.empty());
            d.length = view_length; d.source_stride = UINT64_MAX - 3;
            CHECK(mgmem_ahb_upload_submit(&d).status == MGMEM_INVALID_ARGUMENT && calls.empty());
            d.source_stride = expected_stride;
            auto r = mgmem_ahb_upload_submit(&d);
            CHECK(r.status == MGMEM_OK && r.pending && r.phase == MGMEM_UPLOAD_PRODUCING);
            CHECK(calls == "OBRWF");
            CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_OK && !r.pending && calls == "OBRWFSCPTXD");
        }
        std::puts("PASS 720p and 1440p geometry acceptance; no device or pixel execution");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "quarantine") == 0) {
        write_status = MGMEM_PROVIDER_ERROR;
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_PROVIDER_ERROR && r.pending && r.phase == MGMEM_UPLOAD_QUARANTINED);
        CHECK(calls == "OBRW");
        int32_t origin = 0;
        CHECK(mgmem_ahb_upload_cancel(&r.pending, &origin) == MGMEM_BUSY);
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR);
        CHECK(calls == "OBRW" && r.pending);
        std::puts("PASS post-write failure quarantines resources");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "fence") == 0) {
        fence_status = MGMEM_PROVIDER_ERROR;
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_PROVIDER_ERROR && r.pending && r.phase == MGMEM_UPLOAD_QUARANTINED);
        CHECK(calls == "OBRWF");
        std::puts("PASS fence failure quarantines resources");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "hard") == 0) {
        begin_status = MGMEM_PROVIDER_ERROR; hard_begin = true;
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_PROVIDER_ERROR && r.pending && r.phase == MGMEM_UPLOAD_QUARANTINED);
        CHECK(calls == "OB");
        std::puts("PASS hard reservation failure quarantines token");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "cleanup") == 0) {
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_OK && r.pending && r.phase == MGMEM_UPLOAD_PRODUCING);
        cleanup_status = MGMEM_PROVIDER_ERROR;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && r.pending);
        CHECK(calls == "OBRWFSC");
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && calls == "OBRWFSC");
        std::puts("PASS uncertain cleanup is never retried");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "retire") == 0) {
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_OK && r.pending && r.phase == MGMEM_UPLOAD_PRODUCING);
        retire_status = MGMEM_PROVIDER_ERROR;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && r.pending);
        CHECK(calls == "OBRWFSCPT");
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && calls == "OBRWFSCPT");
        std::puts("PASS uncertain final release remains quarantined");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "cancel_release") == 0) {
        ready_result = 1; cancel_fail = 2;
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.pending && r.phase == MGMEM_UPLOAD_QUARANTINED && calls == "OBRA");
        CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_QUARANTINED);
        int32_t origin = 0;
        CHECK(mgmem_ahb_upload_cancel(&r.pending, &origin) == MGMEM_BUSY);
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && calls == "OBRA");
        std::puts("PASS final release failure during cancel is quarantined");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "ready_error") == 0) {
        ready_result = -1;
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_PROVIDER_ERROR && r.pending && r.phase == MGMEM_UPLOAD_QUARANTINED && calls == "OBR");
        int32_t origin = 0;
        CHECK(mgmem_ahb_upload_cancel(&r.pending, &origin) == MGMEM_BUSY && calls == "OBR");
        std::puts("PASS failed or unverifiable ready fence remains quarantined");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "ready_unverified") == 0) {
        ready_result = 2; /* poll ready, status ioctl interrupted */
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_PROVIDER_ERROR && r.pending && r.phase == MGMEM_UPLOAD_QUARANTINED && calls == "OBR");
        int32_t origin = 0;
        CHECK(mgmem_ahb_upload_cancel(&r.pending, &origin) == MGMEM_BUSY && calls == "OBR");
        std::puts("PASS unverified ready status cannot be returned to Compute");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "producer_error") == 0) {
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_OK && r.pending && r.phase == MGMEM_UPLOAD_PRODUCING && calls == "OBRWF");
        completion_result = -1;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && r.pending && calls == "OBRWFS");
        CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_QUARANTINED);
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_PROVIDER_ERROR && calls == "OBRWFS");
        std::puts("PASS failed or unverifiable producer is never published");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "producer_unverified") == 0) {
        auto r = mgmem_ahb_upload_submit(&d);
        CHECK(r.status == MGMEM_OK && r.pending && r.phase == MGMEM_UPLOAD_PRODUCING && calls == "OBRWF");
        completion_result = 2;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_TIMEOUT && r.pending && calls == "OBRWFS");
        CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_PRODUCING);
        completion_result = 0;
        CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_OK && !r.pending && calls == "OBRWFSSCPTXD");
        std::puts("PASS unverified producer status remains retryable without early publication");
        return 0;
    }
    CHECK(argc == 1);
    d.source_stride = 12;
    CHECK(mgmem_ahb_upload_submit(&d).status == MGMEM_INVALID_ARGUMENT && calls.empty());
    d = desc(); d.length = 63;
    CHECK(mgmem_ahb_upload_submit(&d).status == MGMEM_INVALID_ARGUMENT && calls.empty());
    d = desc(); open_status = MGMEM_UNSUPPORTED;
    CHECK(mgmem_ahb_upload_submit(&d).status == MGMEM_UNSUPPORTED && calls == "OD");
    open_status = MGMEM_OK; calls.clear();
    ready_result = 1;
    auto timeout = mgmem_ahb_upload_submit(&d);
    CHECK(timeout.status == MGMEM_TIMEOUT && !timeout.pending && calls == "OBRAD");
    calls.clear(); cancel_fail = 1;
    timeout = mgmem_ahb_upload_submit(&d);
    CHECK(timeout.status == MGMEM_TIMEOUT && timeout.pending && timeout.phase == MGMEM_UPLOAD_CANCEL && calls == "OBRA");
    int32_t origin = 0; cancel_fail = 0;
    CHECK(mgmem_ahb_upload_cancel(&timeout.pending, &origin) == MGMEM_OK && !timeout.pending && calls == "OBRAAD");
    calls.clear(); ready_result = 0; publish_fail = 1;
    auto r = mgmem_ahb_upload_submit(&d);
    CHECK(r.status == MGMEM_OK && r.pending && r.phase == MGMEM_UPLOAD_PRODUCING && calls == "OBRWF");
    CHECK(mgmem_ahb_upload_cancel(&r.pending, &origin) == MGMEM_BUSY);
    completion_result = 1;
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_TIMEOUT && r.pending && calls == "OBRWFS");
    completion_result = 0; cleanup_status = MGMEM_BUSY;
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_BUSY && r.pending && calls == "OBRWFSSC");
    cleanup_status = MGMEM_OK;
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_BUSY && r.pending && calls == "OBRWFSSCCP");
    CHECK(mgmem_ahb_upload_phase(r.pending) == MGMEM_UPLOAD_PUBLISH);
    publish_fail = 0;
    CHECK(mgmem_ahb_upload_retry(r.pending, &origin) == MGMEM_OK && calls == "OBRWFSSCCPP");
    CHECK(mgmem_ahb_upload_cancel(&r.pending, &origin) == MGMEM_BUSY);
    CHECK(mgmem_ahb_upload_wait(&r.pending, 0) == MGMEM_OK && !r.pending && calls == "OBRWFSSCCPPTXD");
    std::puts("PASS ready wait, retry and physical retirement ordering");
}
