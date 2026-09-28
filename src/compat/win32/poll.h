/* Windows shim for <poll.h>: only on the include path of Windows builds.
 * Used only by ae.c's aeWait(), which wperfk never calls. */
#ifndef WPK_SHIM_POLL_H
#define WPK_SHIM_POLL_H
#include <winsock2.h>
#define poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))
#endif
