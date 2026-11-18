#ifndef MGMEM_INTERNAL_SYNC_FILE_WAIT_HPP
#define MGMEM_INTERNAL_SYNC_FILE_WAIT_HPP

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>

namespace mgmem_detail {
/* Legacy Android sync UAPI predates Linux 4.7. Fixed header and ioctl number
 * match AOSP libsync; the kernel may append point records. */
struct legacy_fence_info { uint32_t length; char name[32]; int32_t status; };
struct legacy_fence_buffer { legacy_fence_info header; uint8_t data[4096 - sizeof(legacy_fence_info)]; };
static_assert(sizeof(legacy_fence_info) == 40 && sizeof(legacy_fence_buffer) == 4096, "legacy sync UAPI layout");
constexpr unsigned long legacy_info_ioctl = _IOWR(SYNC_IOC_MAGIC, 2, legacy_fence_info);

/* A non-blocking status query for a caller-owned, stable sync-file fd. This
 * does not call poll: 0 means confirmed success, 1 means still active,
 * 2 means status inspection was interrupted, -2 means the sync-file itself
 * reported a negative failure status, and -1 means invalid or unverifiable
 * status. Input fd -1 denotes an already satisfied dependency. An active
 * result can race a later signal, so callers must retry or wait. Never use a
 * result other than 0 as proof of completed successful work. */
template<class Ioctl>
int sync_file_probe_impl(int fd, Ioctl inspect) noexcept {
    if (fd < -1) return -1;
    if (fd == -1) return 0;
    sync_file_info info{};
    if (inspect(fd, SYNC_IOC_FILE_INFO, &info) == 0)
        return info.status == 1 ? 0 : info.status == 0 ? 1 : info.status < 0 ? -2 : -1;
    if (errno == EINTR) return 2;
    if (errno != ENOTTY) return -1;
    legacy_fence_buffer legacy{};
    legacy.header.length = sizeof(legacy);
    if (inspect(fd, legacy_info_ioctl, &legacy) != 0)
        return errno == EINTR ? 2 : -1;
    if (legacy.header.length < sizeof(legacy.header) || legacy.header.length > sizeof(legacy)) return -1;
    return legacy.header.status == 1 ? 0 : legacy.header.status == 0 ? 1 : legacy.header.status < 0 ? -2 : -1;
}

inline int sync_file_probe(int fd) noexcept {
    return sync_file_probe_impl(fd,
        [](int value, unsigned long request, void *data) { return ::ioctl(value, request, data); });
}

/* fd is borrowed. -1 is already satisfied; 0 means confirmed successful
 * sync-file, 1 poll timeout, 2 readiness observed but status ioctl interrupted
 * through deadline, -2 confirmed fence failure, -1 invalid/unverifiable status. Poll and
 * interrupted status ioctls share the caller's original deadline. Return 2 is
 * NOT proof of a successful fence and MUST NOT be passed on as completed. */
template<class Now, class Poll, class Ioctl>
int sync_file_wait_impl(int fd, int timeout_ms, Now now, Poll poll_once, Ioctl inspect) noexcept {
    if (fd < -1 || timeout_ms < -1) return -1;
    if (fd == -1) return 0;
    using clock = std::chrono::steady_clock;
    const auto deadline = timeout_ms >= 0 ? now() + std::chrono::milliseconds(timeout_ms) : clock::time_point::max();
    int remaining = timeout_ms;
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        const int rc = poll_once(&p, 1, remaining);
        if (rc > 0) {
            if (!(p.revents & POLLIN) || (p.revents & (POLLERR | POLLNVAL))) return -1;
            for (;;) {
                sync_file_info info{};
                const int result = inspect(fd, SYNC_IOC_FILE_INFO, &info);
                if (result == 0) return info.status == 1 ? 0 : info.status < 0 ? -2 : -1;
                if (errno == EINTR) {
                    if (timeout_ms >= 0 && now() >= deadline) return 2;
                    continue;
                }
                if (errno != ENOTTY) return -1;
                break;
            }
            for (;;) {
                legacy_fence_buffer info{};
                info.header.length = sizeof(info);
                const int result = inspect(fd, legacy_info_ioctl, &info);
                if (result == 0) {
                    if (info.header.length < sizeof(info.header) || info.header.length > sizeof(info)) return -1;
                    return info.header.status == 1 ? 0 : info.header.status < 0 ? -2 : -1;
                }
                if (errno != EINTR) return -1;
                if (timeout_ms >= 0 && now() >= deadline) return 2;
            }
        }
        if (!rc) return 1;
        if (errno != EINTR && errno != EAGAIN) return -1;
        if (timeout_ms >= 0) {
            const auto current = now();
            if (current >= deadline) return 1;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - current).count();
            remaining = static_cast<int>(left >= INT_MAX ? INT_MAX : left + 1);
        }
    }
}

inline int sync_file_wait(int fd, int timeout_ms) noexcept {
    return sync_file_wait_impl(fd, timeout_ms,
        [] { return std::chrono::steady_clock::now(); },
        [](pollfd *p, int count, int timeout) { return ::poll(p, count, timeout); },
        [](int value, unsigned long request, void *data) { return ::ioctl(value, request, data); });
}
}
#endif
