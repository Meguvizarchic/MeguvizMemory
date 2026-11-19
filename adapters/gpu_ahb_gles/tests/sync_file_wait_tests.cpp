#include "../../common/sync_file_wait.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
/* The tested template receives injected functions; native entry points must
 * never be called by this deterministic test. */
extern "C" int poll(struct pollfd *, nfds_t, int) { std::abort(); }
extern "C" int ioctl(int, unsigned long, ...) { std::abort(); }

int main() {
    using clock = std::chrono::steady_clock;
    int ms = 0, polls = 0, inspections = 0;
    std::vector<int> timeouts;
    auto now = [&] { return clock::time_point{} + std::chrono::milliseconds(ms); };
    auto never_poll = [&](pollfd *, int, int) -> int { std::abort(); };
    auto never_inspect = [&](int, unsigned long, void *) -> int { std::abort(); };
    CHECK(mgmem_detail::sync_file_wait_impl(-1, 0, now, never_poll, never_inspect) == 0);
    CHECK(mgmem_detail::sync_file_wait_impl(-2, 0, now, never_poll, never_inspect) < 0);
    CHECK(mgmem_detail::sync_file_wait_impl(10, -2, now, never_poll, never_inspect) < 0);

    auto interrupted_poll = [&](pollfd *p, int n, int timeout) {
        CHECK(p && p->fd == 10 && n == 1); timeouts.push_back(timeout); ++polls;
        if (polls == 1) { ms = 3; errno = EINTR; return -1; }
        p->revents = POLLIN; return 1;
    };
    auto success = [&](int fd, unsigned long request, void *value) {
        CHECK(fd == 10 && request == SYNC_IOC_FILE_INFO); ++inspections;
        static_cast<sync_file_info *>(value)->status = 1; return 0;
    };
    CHECK(mgmem_detail::sync_file_wait_impl(10, 10, now, interrupted_poll, success) == 0);
    CHECK(polls == 2 && inspections == 1 && timeouts.size() == 2 && timeouts[0] == 10 && timeouts[1] == 7);

    polls = 0;
    auto fractional_now = [&] { return clock::time_point{} + std::chrono::microseconds(polls ? 9500 : 0); };
    auto retry_last_fraction = [&](pollfd *, int, int timeout) {
        if (++polls == 1) { CHECK(timeout == 10); errno = EAGAIN; return -1; }
        CHECK(timeout == 1); return 0;
    };
    CHECK(mgmem_detail::sync_file_wait_impl(10, 10, fractional_now, retry_last_fraction, never_inspect) == 1);
    CHECK(polls == 2);

    ms = polls = inspections = 0;
    auto ready = [&](pollfd *p, int, int) { ++polls; p->revents = POLLIN; return 1; };
    auto interrupted_inspection = [&](int, unsigned long, void *) {
        ++inspections; ms = inspections == 1 ? 5 : 11; errno = EINTR; return -1;
    };
    CHECK(mgmem_detail::sync_file_wait_impl(10, 10, now, ready, interrupted_inspection) == 2);
    CHECK(polls == 1 && inspections == 2);

    ms = polls = inspections = 0;
    auto interrupted_legacy = [&](int, unsigned long request, void *) {
        ++inspections;
        if (request == SYNC_IOC_FILE_INFO) { errno = ENOTTY; return -1; }
        CHECK(request == mgmem_detail::legacy_info_ioctl);
        ms = 11; errno = EINTR; return -1;
    };
    CHECK(mgmem_detail::sync_file_wait_impl(10, 10, now, ready, interrupted_legacy) == 2);
    CHECK(polls == 1 && inspections == 2);

    ms = polls = 0;
    auto immediate_interrupt = [&](pollfd *, int, int timeout) {
        CHECK(timeout == 0); ++polls; errno = EINTR; return -1;
    };
    CHECK(mgmem_detail::sync_file_wait_impl(10, 0, now, immediate_interrupt, never_inspect) == 1 && polls == 1);

    /* A non-blocking status probe must not poll or mistake an active/error
     * fence for success. A modern success requires exactly one ioctl. */
    int probe_calls = 0, probe_status = 1;
    auto probe_modern = [&](int fd, unsigned long request, void *value) {
        CHECK(fd == 10 && request == SYNC_IOC_FILE_INFO); ++probe_calls;
        auto *info = static_cast<sync_file_info *>(value);
        CHECK(!info->flags && !info->num_fences && !info->sync_fence_info);
        info->status = probe_status; return 0;
    };
    CHECK(mgmem_detail::sync_file_probe_impl(-1, never_inspect) == 0);
    CHECK(mgmem_detail::sync_file_probe_impl(-2, never_inspect) < 0);
    CHECK(mgmem_detail::sync_file_probe_impl(10, probe_modern) == 0 && probe_calls == 1);
    probe_status = 0; CHECK(mgmem_detail::sync_file_probe_impl(10, probe_modern) == 1 && probe_calls == 2);
    probe_status = -5; CHECK(mgmem_detail::sync_file_probe_impl(10, probe_modern) == -2 && probe_calls == 3);
    probe_status = 3; CHECK(mgmem_detail::sync_file_probe_impl(10, probe_modern) == -1 && probe_calls == 4);

    int legacy_calls = 0, legacy_status = 1;
    auto probe_legacy = [&](int fd, unsigned long request, void *value) {
        CHECK(fd == 10); ++legacy_calls;
        if (request == SYNC_IOC_FILE_INFO) { errno = ENOTTY; return -1; }
        CHECK(request == mgmem_detail::legacy_info_ioctl);
        auto *info = static_cast<mgmem_detail::legacy_fence_buffer *>(value);
        CHECK(info->header.length == sizeof(*info));
        info->header.length = sizeof(info->header); info->header.status = legacy_status; return 0;
    };
    CHECK(mgmem_detail::sync_file_probe_impl(10, probe_legacy) == 0 && legacy_calls == 2);
    legacy_status = 0; CHECK(mgmem_detail::sync_file_probe_impl(10, probe_legacy) == 1 && legacy_calls == 4);
    legacy_status = -5; CHECK(mgmem_detail::sync_file_probe_impl(10, probe_legacy) == -2 && legacy_calls == 6);
    auto interrupted_status = [&](int, unsigned long, void *) { errno = EINTR; return -1; };
    CHECK(mgmem_detail::sync_file_probe_impl(10, interrupted_status) == 2);
    auto denied_status = [&](int, unsigned long, void *) { errno = EPERM; return -1; };
    CHECK(mgmem_detail::sync_file_probe_impl(10, denied_status) == -1);
    auto interrupted_legacy_status = [&](int, unsigned long request, void *) {
        errno = request == SYNC_IOC_FILE_INFO ? ENOTTY : EINTR; return -1;
    };
    CHECK(mgmem_detail::sync_file_probe_impl(10, interrupted_legacy_status) == 2);
    auto unsupported_status = [&](int, unsigned long, void *) { errno = ENOTTY; return -1; };
    CHECK(mgmem_detail::sync_file_probe_impl(10, unsupported_status) == -1);
    auto invalid_legacy_size = [&](int, unsigned long request, void *value) {
        if (request == SYNC_IOC_FILE_INFO) { errno = ENOTTY; return -1; }
        static_cast<mgmem_detail::legacy_fence_buffer *>(value)->header.length = 1; return 0;
    };
    CHECK(mgmem_detail::sync_file_probe_impl(10, invalid_legacy_size) < 0);
    std::puts("PASS deterministic sync-file deadlines and interrupted syscalls");
}
