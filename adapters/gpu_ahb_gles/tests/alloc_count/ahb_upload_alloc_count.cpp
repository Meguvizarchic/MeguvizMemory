// Heap-allocation census for one AHB upload frame. Counts only scalar/array
// operator-new traffic in this executable (real ahb_upload + gpu_ahb TUs and
// the platform stand-in below). It cannot see allocations inside the Memory
// DLL, a real Compute runtime, or EGL/GLES drivers; mgc_* here are stubs.
#include <mgmem/ahb_upload.h>
#include <meguviz/compute_android.h>
#include "platform.hpp"
#include "upload_internal.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)

namespace {
struct Bucket { std::size_t size = 0; long long attempts = 0; };
struct Counters {
    long long attempts = 0, bytes = 0, frees = 0, live = 0, peak = 0;
    Bucket buckets[16]{};
    long long other_attempts = 0, other_bytes = 0;
};
Counters g_count;
bool g_counting = false;

void note_alloc(std::size_t n) noexcept {
    if (!g_counting) return;
    ++g_count.attempts; g_count.bytes += static_cast<long long>(n);
    g_count.live += static_cast<long long>(n);
    if (g_count.live > g_count.peak) g_count.peak = g_count.live;
    for (auto &b : g_count.buckets) {
        if (b.size == n) { ++b.attempts; return; }
        if (!b.size && !b.attempts) { b.size = n; b.attempts = 1; return; }
    }
    ++g_count.other_attempts; g_count.other_bytes += static_cast<long long>(n);
}
void note_free(std::size_t n) noexcept {
    if (!g_counting) return;
    ++g_count.frees; g_count.live -= static_cast<long long>(n);
}
struct Mark { Counters at; };
Mark mark() noexcept { return {g_count}; }
void report(const char *name, const Mark &from) {
    std::printf("{\"scenario\":\"%s\",\"attempts\":%lld,\"bytes\":%lld,\"frees\":%lld,\"live_delta\":%lld,\"buckets\":[",
                name, g_count.attempts - from.at.attempts, g_count.bytes - from.at.bytes,
                g_count.frees - from.at.frees, g_count.live - from.at.live);
    bool first = true;
    for (int i = 0; i < 16; ++i) {
        const long long d = g_count.buckets[i].attempts - from.at.buckets[i].attempts;
        if (!d) continue;
        std::printf("%s{\"size\":%zu,\"attempts\":%lld}", first ? "" : ",", g_count.buckets[i].size, d);
        first = false;
    }
    const long long od = g_count.other_attempts - from.at.other_attempts;
    if (od) std::printf("%s{\"size\":-1,\"attempts\":%lld,\"bytes\":%lld}", first ? "" : ",", od,
                        g_count.other_bytes - from.at.other_bytes);
    std::printf("]}\n");
}
} // namespace

void *operator new(std::size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    note_alloc(n); return p;
}
void *operator new[](std::size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    note_alloc(n); return p;
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
    void *p = std::malloc(n ? n : 1);
    if (p) note_alloc(n);
    return p;
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
    void *p = std::malloc(n ? n : 1);
    if (p) note_alloc(n);
    return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t n) noexcept { if (p) note_free(n); std::free(p); }
void operator delete[](void *p, std::size_t n) noexcept { if (p) note_free(n); std::free(p); }

// Compute stand-ins (same contract as tests/integration). Event log is a
// fixed char buffer so the stubs themselves never allocate in a window.
struct AHardwareBuffer {};
struct MgcLease {
    MgcLeaseInfo info{};
    AHardwareBuffer buffer{};
    std::atomic<int> refs{1};
    bool reserved = false;
    bool return_busy = false;
};
// Same field layout as the real platform struct in src/platform_gl.cpp
// (pointer-sized EGL handles and entry points); `cleaned` reuses tail padding.
struct mgmem_upload_platform {
    void *display = nullptr, *context = nullptr;
    std::thread::id thread;
    uint64_t thread_token = 0;
    void *client = nullptr, *create_image = nullptr, *destroy_image = nullptr;
    void *target_image = nullptr, *create_sync = nullptr, *destroy_sync = nullptr, *dup_fence = nullptr;
    void *image = nullptr, *fence = nullptr;
    uint32_t texture = 0;
    bool cleaned = false;
};
static char events[256]{};
static std::size_t events_len = 0;
static void log_event(char c) { if (events_len + 1 < sizeof(events)) events[events_len++] = c; }
static unsigned char copied[64]{};
static bool complete = false, fail_open = false;
static int producer_result = 0;
static unsigned errors = 0;
static void on_error(void *, mgmem_status) { ++errors; }

extern "C" void mgc_lease_retain(MgcLease *l) { ++l->refs; }
extern "C" void mgc_lease_release(MgcLease *l) { --l->refs; }
extern "C" MgcStatus mgc_lease_info(MgcLease *l, MgcLeaseInfo *out) { *out = l->info; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_get_buffer(MgcLease *l, AHardwareBuffer **out) { *out = &l->buffer; return MGC_SUCCESS; }
extern "C" MgcStatus mgc_android_release_to_external(MgcLease *l, int32_t *fd) {
    CHECK(!l->reserved); l->reserved = true; ++l->refs; *fd = 11; log_event('B'); return MGC_SUCCESS;
}
extern "C" MgcStatus mgc_android_set_acquire_fence(MgcLease *l, int32_t fd) {
    CHECK(l->reserved && fd == -1); log_event('P');
    if (l->return_busy) return MGC_BUSY;
    l->reserved = false; --l->refs; return MGC_SUCCESS;
}
extern "C" int mgmem_gpu_test_dup(int) { std::abort(); }
extern "C" void mgmem_gpu_test_close(int fd) { log_event(fd == 11 ? 'r' : 'd'); CHECK(fd == 11 || fd == 120); }
extern "C" int mgmem_gpu_test_wait(int, int) { std::abort(); }

mgmem_status mgmem_upload_platform_open(mgmem_upload_platform **out, int32_t *) noexcept {
    log_event('O');
    if (fail_open) return MGMEM_PROVIDER_ERROR;
    *out = new (std::nothrow) mgmem_upload_platform{};
    return *out ? MGMEM_OK : MGMEM_OUT_OF_MEMORY;
}
mgmem_status mgmem_upload_platform_write(mgmem_upload_platform *, const mgmem_gpu_ahb_view &v, uint64_t stride, int32_t *) noexcept {
    CHECK(v.data && v.length == 64 && v.buffer && stride == 16); log_event('W'); std::memcpy(copied, v.data, 64); return MGMEM_OK;
}
mgmem_status mgmem_upload_platform_fence(mgmem_upload_platform *, int *fd, int32_t *) noexcept { log_event('F'); *fd = 20; return MGMEM_OK; }
mgmem_status mgmem_upload_platform_cleanup(mgmem_upload_platform *p, int32_t *) noexcept {
    CHECK(complete && p && !p->cleaned); p->cleaned = true; log_event('C'); return MGMEM_OK;
}
void mgmem_upload_platform_discard(mgmem_upload_platform *p) noexcept { log_event('x'); delete p; }
int mgmem_upload_platform_wait(int fd, int timeout) noexcept {
    CHECK(timeout == 0);
    if (fd == 11) { log_event('R'); return 0; }
    CHECK(fd == 20); log_event('S'); return producer_result;
}
int mgmem_upload_platform_probe(int fd) noexcept { CHECK(fd == 20); log_event('S'); return producer_result; }
void mgmem_upload_platform_close_fd(int fd) noexcept { CHECK(fd == 20); log_event('f'); }

namespace {
struct Fixture {
    mgmem_domain *domain = nullptr;
    mgmem_handle_v1 owner{};
    MgcLease lease{};
    mgmem_ahb_upload_desc desc{};
    Fixture() {
        mgmem_domain_options_v1 options{};
        options.struct_size = sizeof(options); options.abi_major = MGMEM_ABI_MAJOR; options.abi_minor = MGMEM_ABI_MINOR;
        options.mode = MGMEM_SYSTEM; options.budget_bytes = 64;
        CHECK(mgmem_domain_create_v1(&options, &domain) == MGMEM_OK);
        CHECK(mgmem_allocate_v1(domain, 64, 8, &owner) == MGMEM_OK);
        mgmem_borrow_v1 view{}; CHECK(mgmem_borrow_acquire_v1(domain, owner, 0, 64, MGMEM_WRITE, &view) == MGMEM_OK);
        std::memset(view.data, 0x5a, 64); CHECK(mgmem_borrow_release_v1(domain, view.lease) == MGMEM_OK);
        lease.info.header = MGC_HEADER_INIT(MgcLeaseInfo);
        lease.info.width = 4; lease.info.height = 4; lease.info.row_stride_bytes = 32;
        lease.info.capacity_bytes = lease.info.byte_length = 64; lease.info.access = MGC_ACCESS_WRITE; lease.info.format = MGC_FORMAT_RGBA8;
        desc.memory = domain; desc.allocation = owner; desc.length = 64; desc.source_stride = 16;
        desc.width = desc.height = 4; desc.write_lease = &lease; desc.on_error = on_error;
    }
    void close_domain() {
        CHECK(mgmem_domain_begin_close_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_collect_v1(domain) == MGMEM_OK);
        CHECK(mgmem_domain_destroy_v1(domain) == MGMEM_OK);
        domain = nullptr;
    }
};
mgmem_snapshot_v1 snap(mgmem_domain *d) {
    mgmem_snapshot_v1 s{}; CHECK(mgmem_domain_snapshot_v1(d, &s, sizeof(s)) == MGMEM_OK); return s;
}
} // namespace

int main() {
    std::printf("{\"sizeof_upload\":%zu,\"sizeof_platform_replica\":%zu}\n",
                sizeof(mgmem_ahb_upload), sizeof(mgmem_upload_platform));
    {   // success: submit window, then wait window
        Fixture f; events_len = 0; std::memset(events, 0, sizeof(events));
        g_counting = true; auto m = mark();
        auto result = mgmem_ahb_upload_submit(&f.desc);
        g_counting = false;
        CHECK(result.status == MGMEM_OK && result.pending);
        report("success_submit", m);
        for (auto b : copied) CHECK(b == 0x5a);
        CHECK(std::strncmp(events, "OBRWF", 5) == 0);
        auto s = snap(f.domain); CHECK(s.retiring_bytes == 64 || s.active_leases == 1);
        CHECK(mgmem_release_v1(f.domain, f.owner) == MGMEM_OK);
        s = snap(f.domain); CHECK(s.retiring_bytes == 64 && s.active_leases == 1);
        complete = true;
        g_counting = true; m = mark();
        mgmem_status waited = MGMEM_TIMEOUT;
        for (int i = 0; i < 4 && result.pending; ++i) waited = mgmem_ahb_upload_wait(&result.pending, 0);
        g_counting = false;
        CHECK(waited == MGMEM_OK && !result.pending);
        report("success_wait", m);
        s = snap(f.domain); CHECK(s.retiring_bytes == 0 && s.active_leases == 0 && !errors);
        f.close_domain();
    }
    {   // invalid: rejected before any allocation
        Fixture f; auto bad = f.desc; bad.length = 0;
        g_counting = true; auto m = mark();
        auto result = mgmem_ahb_upload_submit(&bad);
        g_counting = false;
        CHECK(result.status == MGMEM_INVALID_ARGUMENT && !result.pending);
        report("invalid", m);
        CHECK(mgmem_release_v1(f.domain, f.owner) == MGMEM_OK);
        f.close_domain();
    }
    {   // open failure: upload struct allocated then destroyed; the stand-in
        // returns before allocating the platform object (real open may differ).
        Fixture f; fail_open = true;
        g_counting = true; auto m = mark();
        auto result = mgmem_ahb_upload_submit(&f.desc);
        g_counting = false; fail_open = false;
        CHECK(result.status == MGMEM_PROVIDER_ERROR && !result.pending);
        report("open_fail", m);
        CHECK(mgmem_release_v1(f.domain, f.owner) == MGMEM_OK);
        f.close_domain();
    }
    std::puts("PASS ahb upload allocation census (stub Compute/platform; DLL and driver allocations excluded)");
}
