#ifndef MGMEM_FAKE_SYNC_FILE_H
#define MGMEM_FAKE_SYNC_FILE_H
#include <stdint.h>
#include <sys/ioctl.h>
struct sync_file_info {
    char name[32]; int32_t status; uint32_t flags; uint32_t num_fences; uint32_t pad; uint64_t sync_fence_info;
};
#define SYNC_IOC_MAGIC '>'
#define SYNC_IOC_FILE_INFO _IOWR(SYNC_IOC_MAGIC, 4, struct sync_file_info)
#endif
