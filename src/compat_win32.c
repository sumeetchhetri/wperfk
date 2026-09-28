// Windows socket layer for wperfk (see compat.h).
//
// ae indexes its per-loop arrays by fd, but Windows SOCKET values are
// opaque handles. Sockets are therefore kept in a process-wide table and
// the rest of wperfk sees only the table index. Indices start at 3 so a
// zero-initialised connection never aliases a live socket, and freed
// indices are reused LIFO, which keeps them below the peak number of open
// sockets (and so below every event loop's setsize).

#ifdef _WIN32

#include "compat.h"
#include <windows.h>

#define WPK_FIRST_FD 3
#define WPK_MAX_FDS  65536

static SOCKET  fds[WPK_MAX_FDS];
static int     free_fds[WPK_MAX_FDS];
static int     free_top;
static int     next_fd = WPK_FIRST_FD;
static SRWLOCK lock    = SRWLOCK_INIT;

int wpk_net_init(void) {
    WSADATA wsa;
    for (int i = 0; i < WPK_MAX_FDS; i++) fds[i] = INVALID_SOCKET;
    return WSAStartup(MAKEWORD(2, 2), &wsa) ? -1 : 0;
}

SOCKET wpk_native(int fd) {
    return (fd >= WPK_FIRST_FD && fd < WPK_MAX_FDS) ? fds[fd] : INVALID_SOCKET;
}

int wpk_socket(int family, int type, int protocol) {
    SOCKET s = socket(family, type, protocol);
    if (s == INVALID_SOCKET) return -1;

    AcquireSRWLockExclusive(&lock);
    int fd = free_top ? free_fds[--free_top]
                      : (next_fd < WPK_MAX_FDS ? next_fd++ : -1);
    if (fd >= 0) fds[fd] = s;
    ReleaseSRWLockExclusive(&lock);

    if (fd < 0) {
        closesocket(s);
        WSASetLastError(WSAEMFILE);
    }
    return fd;
}

int wpk_close(int fd) {
    SOCKET s = INVALID_SOCKET;
    if (fd < WPK_FIRST_FD || fd >= WPK_MAX_FDS) return -1;

    AcquireSRWLockExclusive(&lock);
    if (fds[fd] != INVALID_SOCKET) { // ignore double close
        s = fds[fd];
        fds[fd] = INVALID_SOCKET;
        free_fds[free_top++] = fd;
    }
    ReleaseSRWLockExclusive(&lock);

    return s == INVALID_SOCKET ? -1 : closesocket(s);
}

int wpk_nonblock(int fd) {
    u_long on = 1;
    return ioctlsocket(wpk_native(fd), FIONBIO, &on) == SOCKET_ERROR ? -1 : 0;
}

int wpk_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return connect(wpk_native(fd), addr, len) == SOCKET_ERROR ? -1 : 0;
}

bool wpk_connect_pending(void) {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}

int wpk_nodelay(int fd) {
    int one = 1;
    return setsockopt(wpk_native(fd), IPPROTO_TCP, TCP_NODELAY,
                      (const char *) &one, sizeof(one)) == SOCKET_ERROR ? -1 : 0;
}

ssize_t wpk_recv(int fd, void *buf, size_t len) {
    int r = recv(wpk_native(fd), buf, (int) len, 0);
    return r == SOCKET_ERROR ? -1 : r;
}

ssize_t wpk_send(int fd, const void *buf, size_t len) {
    int r = send(wpk_native(fd), buf, (int) len, 0);
    return r == SOCKET_ERROR ? -1 : r;
}

bool wpk_would_block(void) {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

int wpk_readable(int fd) {
    u_long n = 0;
    return ioctlsocket(wpk_native(fd), FIONREAD, &n) == SOCKET_ERROR ? 0 : (int) n;
}

#endif /* _WIN32 */
