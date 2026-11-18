#ifndef MGMEM_TEST_HARDWARE_BUFFER_H
#define MGMEM_TEST_HARDWARE_BUFFER_H
#include <stdint.h>
typedef struct AHardwareBuffer AHardwareBuffer;
typedef struct AHardwareBuffer_Desc {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} AHardwareBuffer_Desc;
enum { AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM = 1 };
enum { AHARDWAREBUFFER_USAGE_CPU_READ_RARELY = 2, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN = 3,
       AHARDWAREBUFFER_USAGE_CPU_READ_MASK = 15, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY = 0x20,
       AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN = 0x30, AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK = 0xf0,
       AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT = 1 << 14 };
extern "C" void AHardwareBuffer_describe(const AHardwareBuffer *, AHardwareBuffer_Desc *);
extern "C" int AHardwareBuffer_lock(AHardwareBuffer *, uint64_t, int32_t, const void *, void **);
extern "C" int AHardwareBuffer_unlock(AHardwareBuffer *, int32_t *);
#endif
