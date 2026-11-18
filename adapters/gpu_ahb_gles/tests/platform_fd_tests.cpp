#include "../src/platform.hpp"
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
struct LegacyInfo { uint32_t len; char name[32]; int32_t status; };
#define LEGACY_IOCTL _IOWR(SYNC_IOC_MAGIC, 2, LegacyInfo)
enum class Mode { modern, legacy, timeout, poll_error, poll_hup, poll_invalid, active, failed,
                  unsupported, denied, legacy_error, legacy_active, poll_eintr, ioctl_eintr };
static Mode mode = Mode::modern;
static int polls, modern, legacy, closes;

extern "C" int poll(struct pollfd *fds, nfds_t count, int timeout) {
    CHECK(count == 1 && fds && fds->fd == 10 && fds->events == POLLIN && timeout >= -1); ++polls;
    if (mode == Mode::timeout) return 0;
    if (mode == Mode::poll_eintr && polls == 1) { errno = EINTR; return -1; }
    fds->revents = mode == Mode::poll_error ? POLLIN | POLLERR : mode == Mode::poll_hup ? POLLHUP :
        mode == Mode::poll_invalid ? POLLIN | POLLNVAL : POLLIN;
    return 1;
}
extern "C" int ioctl(int fd, unsigned long request, ...) {
    CHECK(fd == 10); va_list args; va_start(args, request); void *arg = va_arg(args, void *); va_end(args);
    if (request == SYNC_IOC_FILE_INFO) {
        ++modern;
        if (mode == Mode::ioctl_eintr && modern == 1) { errno = EINTR; return -1; }
        if (mode == Mode::legacy || mode == Mode::legacy_error || mode == Mode::legacy_active || mode == Mode::unsupported) { errno = ENOTTY; return -1; }
        if (mode == Mode::denied) { errno = EPERM; return -1; }
        auto *info = static_cast<sync_file_info *>(arg);
        CHECK(info && !info->flags && !info->num_fences && !info->sync_fence_info);
        info->status = mode == Mode::active ? 0 : mode == Mode::failed ? -EIO : 1;
        return 0;
    }
    CHECK(request == LEGACY_IOCTL); ++legacy;
    if (mode == Mode::unsupported) { errno = ENOTTY; return -1; }
    auto *info = static_cast<LegacyInfo *>(arg);
    CHECK(info && info->len >= sizeof(LegacyInfo));
    info->len = sizeof(LegacyInfo);
    info->status = mode == Mode::legacy_error ? -EIO : mode == Mode::legacy_active ? 0 : 1;
    return 0;
}
extern "C" int close(int fd) { CHECK(fd == 10); ++closes; return 0; }

static void reset(Mode m) { mode = m; polls = modern = legacy = closes = 0; }
int main() {
    reset(Mode::modern);
    CHECK(mgmem_upload_platform_wait(-1, 0) == 0 && !polls);
    CHECK(mgmem_upload_platform_wait(-2, 0) < 0 && !polls);
    CHECK(mgmem_upload_platform_wait(10, 0) == 0 && polls == 1 && modern == 1 && !legacy && !closes);
    reset(Mode::timeout); CHECK(mgmem_upload_platform_wait(10, 0) == 1 && polls == 1 && !modern);
    reset(Mode::poll_error); CHECK(mgmem_upload_platform_wait(10, 0) < 0 && !modern);
    reset(Mode::poll_hup); CHECK(mgmem_upload_platform_wait(10, 0) < 0 && !modern);
    reset(Mode::poll_invalid); CHECK(mgmem_upload_platform_wait(10, 0) < 0 && !modern);
    reset(Mode::active); CHECK(mgmem_upload_platform_wait(10, 0) < 0 && modern == 1);
    reset(Mode::failed); CHECK(mgmem_upload_platform_wait(10, 0) == -2 && modern == 1);
    reset(Mode::failed); CHECK(mgmem_upload_platform_wait(10, 1000) == -2 && modern == 1);
    reset(Mode::legacy); CHECK(mgmem_upload_platform_wait(10, 0) == 0 && modern == 1 && legacy == 1);
    reset(Mode::legacy_error); CHECK(mgmem_upload_platform_wait(10, 0) == -2 && modern == 1 && legacy == 1);
    reset(Mode::legacy_error); CHECK(mgmem_upload_platform_wait(10, 1000) == -2 && modern == 1 && legacy == 1);
    reset(Mode::legacy_active); CHECK(mgmem_upload_platform_wait(10, 0) < 0 && modern == 1 && legacy == 1);
    reset(Mode::unsupported); CHECK(mgmem_upload_platform_wait(10, 0) == -1 && modern == 1 && legacy == 1);
    reset(Mode::denied); CHECK(mgmem_upload_platform_wait(10, 0) == -1 && modern == 1 && !legacy);
    reset(Mode::denied); CHECK(mgmem_upload_platform_wait(10, 1000) == -1 && modern == 1 && !legacy);
    reset(Mode::poll_eintr); CHECK(mgmem_upload_platform_wait(10, 1000) == 0 && polls == 2 && modern == 1);
    reset(Mode::ioctl_eintr); CHECK(mgmem_upload_platform_wait(10, 1000) == 0 && modern == 2);
    reset(Mode::ioctl_eintr); CHECK(mgmem_upload_platform_wait(10, 0) == 2 && modern == 1 && !legacy);
    reset(Mode::modern); CHECK(mgmem_upload_platform_probe(10) == 0 && !polls && modern == 1);
    reset(Mode::active); CHECK(mgmem_upload_platform_probe(10) == 1 && !polls && modern == 1);
    reset(Mode::failed); CHECK(mgmem_upload_platform_probe(10) == -2 && !polls && modern == 1);
    reset(Mode::legacy); CHECK(mgmem_upload_platform_probe(10) == 0 && !polls && modern == 1 && legacy == 1);
    reset(Mode::legacy_active); CHECK(mgmem_upload_platform_probe(10) == 1 && !polls && modern == 1 && legacy == 1);
    reset(Mode::legacy_error); CHECK(mgmem_upload_platform_probe(10) == -2 && !polls && modern == 1 && legacy == 1);
    reset(Mode::unsupported); CHECK(mgmem_upload_platform_probe(10) == -1 && !polls && modern == 1 && legacy == 1);
    reset(Mode::denied); CHECK(mgmem_upload_platform_probe(10) == -1 && !polls && modern == 1 && !legacy);
    reset(Mode::ioctl_eintr); CHECK(mgmem_upload_platform_probe(10) == 2 && !polls && modern == 1);
    CHECK(!closes); mgmem_upload_platform_close_fd(10); CHECK(closes == 1);
    std::puts("PASS modern and legacy fence status, error, unsupported and timeout paths");
}
