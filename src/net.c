// Copyright (C) 2013 - Will Glozer.  All rights reserved.

#include "net.h"
#include "compat.h"

status sock_connect(connection *c, char *host) {
    return OK;
}

status sock_close(connection *c) {
    return OK;
}

status sock_read(connection *c, size_t *n) {
    ssize_t r = wpk_recv(c->fd, c->buf, sizeof(c->buf));
    if (r == -1 && wpk_would_block()) return RETRY;
    *n = (size_t) r;
    return r >= 0 ? OK : ERROR;
}

status sock_write(connection *c, char *buf, size_t len, size_t *n) {
    ssize_t r;
    if ((r = wpk_send(c->fd, buf, len)) == -1) {
        return wpk_would_block() ? RETRY : ERROR;
    }
    *n = (size_t) r;
    return OK;
}

size_t sock_readable(connection *c) {
    return wpk_readable(c->fd);
}
