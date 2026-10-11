#ifndef SSL_H
#define SSL_H

#include <stdbool.h>
#include "net.h"

typedef struct {
    bool        verify;   // verify the server certificate and host name
    const char *cacert;   // CA bundle (PEM); default: system store
    const char *cert;     // client certificate chain (PEM), for mTLS
    const char *key;      // client private key (PEM); default: cert file
    const char *sni;      // server name for SNI and verification
} tls_opts;

SSL_CTX *ssl_init(const tls_opts *);

status ssl_connect(connection *, char *);
status ssl_close(connection *);
status ssl_read(connection *, size_t *);
status ssl_write(connection *, char *, size_t, size_t *);
size_t ssl_readable(connection *);

#endif /* SSL_H */
