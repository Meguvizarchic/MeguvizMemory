#ifndef MGMEM_FAKE_IOCTL_H
#define MGMEM_FAKE_IOCTL_H
#define _IOWR(type, nr, size) (0x1000ul + (nr))
extern "C" int ioctl(int, unsigned long, ...);
#endif
