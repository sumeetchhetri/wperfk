#ifndef COMPAT_H
#define COMPAT_H

// Portable socket layer.
//
// POSIX: thin inline wrappers over the usual calls.
// Windows: implemented in compat_win32.c. Each SOCKET handle is mapped to a
// small int "fd" so the ae event loop can keep indexing its arrays by fd;
// wpk_native() returns the real handle for wepoll and OpenSSL.

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

int     wpk_net_init(void);
int     wpk_socket(int family, int type, int protocol);
int     wpk_close(int fd);
int     wpk_nonblock(int fd);
int     wpk_connect(int fd, const struct sockaddr *addr, socklen_t len);
bool    wpk_connect_pending(void);
int     wpk_nodelay(int fd);
ssize_t wpk_recv(int fd, void *buf, size_t len);
ssize_t wpk_send(int fd, const void *buf, size_t len);
bool    wpk_would_block(void);
int     wpk_readable(int fd);
SOCKET  wpk_native(int fd);
#define wpk_ssl_fd(fd) ((int) wpk_native(fd))

#else

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static inline int wpk_net_init(void) { return 0; }

static inline int wpk_socket(int family, int type, int protocol) {
    return socket(family, type, protocol);
}

static inline int wpk_close(int fd) { return close(fd); }

static inline int wpk_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int wpk_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return connect(fd, addr, len);
}

static inline bool wpk_connect_pending(void) { return errno == EINPROGRESS; }

static inline int wpk_nodelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static inline ssize_t wpk_recv(int fd, void *buf, size_t len) { return read(fd, buf, len); }
static inline ssize_t wpk_send(int fd, const void *buf, size_t len) { return write(fd, buf, len); }

static inline bool wpk_would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

static inline int wpk_readable(int fd) {
    int n;
    return ioctl(fd, FIONREAD, &n) == -1 ? 0 : n;
}

static inline int wpk_native(int fd) { return fd; }
#define wpk_ssl_fd(fd) (fd)

#endif

#endif /* COMPAT_H */
