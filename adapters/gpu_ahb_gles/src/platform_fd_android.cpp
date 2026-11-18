#include "platform.hpp"
#include "../../common/sync_file_wait.hpp"
#include <unistd.h>

int mgmem_upload_platform_wait(int fd, int timeout_ms) noexcept {
    return mgmem_detail::sync_file_wait(fd, timeout_ms);
}
int mgmem_upload_platform_probe(int fd) noexcept {
    return mgmem_detail::sync_file_probe(fd);
}
void mgmem_upload_platform_close_fd(int fd) noexcept { if (fd >= 0) (void)close(fd); }
