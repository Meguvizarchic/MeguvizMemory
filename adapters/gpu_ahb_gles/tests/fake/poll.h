#ifndef MGMEM_FAKE_POLL_H
#define MGMEM_FAKE_POLL_H
typedef unsigned long nfds_t;
struct pollfd { int fd; short events; short revents; };
#define POLLIN 0x0001
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
extern "C" int poll(struct pollfd *, nfds_t, int);
#endif
