// Copyright (C) 2013 - Will Glozer.  All rights reserved.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ssl.h"
#include "compat.h"

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define TLS_client_method SSLv23_client_method
static pthread_mutex_t *locks;

static void ssl_lock(int mode, int n, const char *file, int line) {
    pthread_mutex_t *lock = &locks[n];
    if (mode & CRYPTO_LOCK) {
        pthread_mutex_lock(lock);
    } else {
        pthread_mutex_unlock(lock);
    }
}

static unsigned long ssl_id() {
    return (unsigned long) pthread_self();
}
#endif

static bool verify_peer = false;
static volatile int verify_reported = 0;

// Static release binaries can't rely on OpenSSL's compiled-in CA path, so
// also try the usual system bundles (Debian/Ubuntu/Alpine, RHEL/Fedora,
// openSUSE, macOS/BSD, FreeBSD), and the certificate store on Windows.
static int load_system_cas(SSL_CTX *ctx) {
    int ok = SSL_CTX_set_default_verify_paths(ctx);
#if defined(_WIN32) && OPENSSL_VERSION_NUMBER >= 0x30200000L
    ok |= SSL_CTX_load_verify_store(ctx, "org.openssl.winstore:");
#else
    static const char *bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/ssl/cert.pem",
        "/usr/local/share/certs/ca-root-nss.crt",
    };
    if (!getenv("SSL_CERT_FILE")) {
        for (size_t i = 0; i < sizeof(bundles) / sizeof(bundles[0]); i++) {
            if (access(bundles[i], R_OK) == 0 && SSL_CTX_load_verify_locations(ctx, bundles[i], NULL) == 1) {
                return 1;
            }
        }
    }
#endif
    return ok;
}

SSL_CTX *ssl_init(const tls_opts *opts) {
    SSL_CTX *ctx = NULL;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    if ((locks = calloc(CRYPTO_num_locks(), sizeof(pthread_mutex_t)))) {
        for (int i = 0; i < CRYPTO_num_locks(); i++) {
            pthread_mutex_init(&locks[i], NULL);
        }

        CRYPTO_set_locking_callback(ssl_lock);
        CRYPTO_set_id_callback(ssl_id);
#else
    /* OpenSSL >= 1.1.0 initialises itself and is thread-safe. */
    {
#endif
        if ((ctx = SSL_CTX_new(TLS_client_method()))) {
            if (opts->verify) {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
                int ok = opts->cacert ? SSL_CTX_load_verify_locations(ctx, opts->cacert, NULL)
                                      : load_system_cas(ctx);
                if (ok != 1) goto fail;
            } else {
                SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
                SSL_CTX_set_verify_depth(ctx, 0);
            }
            if (opts->cert) {
                if (SSL_CTX_use_certificate_chain_file(ctx, opts->cert) != 1 ||
                    SSL_CTX_use_PrivateKey_file(ctx, opts->key ? opts->key : opts->cert, SSL_FILETYPE_PEM) != 1 ||
                    SSL_CTX_check_private_key(ctx) != 1) goto fail;
            }
            SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
            SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
            verify_peer = opts->verify;
        }
    }

    return ctx;

  fail:
    SSL_CTX_free(ctx);
    return NULL;
}

status ssl_connect(connection *c, char *host) {
    int r;
    SSL_set_fd(c->ssl, wpk_ssl_fd(c->fd));
    SSL_set_tlsext_host_name(c->ssl, host);
    if (verify_peer) {
        // IP literal -> check IP SANs, otherwise the DNS name.
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(c->ssl), host) != 1) {
            SSL_set1_host(c->ssl, host);
        }
    }
    if ((r = SSL_connect(c->ssl)) != 1) {
        switch (SSL_get_error(c->ssl, r)) {
            case SSL_ERROR_WANT_READ:  return RETRY;
            case SSL_ERROR_WANT_WRITE: return RETRY;
            default: {
                long v = verify_peer ? SSL_get_verify_result(c->ssl) : X509_V_OK;
                if (v != X509_V_OK && __sync_bool_compare_and_swap(&verify_reported, 0, 1)) {
                    fprintf(stderr, "TLS verification failed for %s: %s\n", host,
                            X509_verify_cert_error_string(v));
                }
                return ERROR;
            }
        }
    }
    return OK;
}

status ssl_close(connection *c) {
    SSL_shutdown(c->ssl);
    SSL_clear(c->ssl);
    return OK;
}

status ssl_read(connection *c, size_t *n) {
    int r;
    if ((r = SSL_read(c->ssl, c->buf, sizeof(c->buf))) <= 0) {
        switch (SSL_get_error(c->ssl, r)) {
            case SSL_ERROR_WANT_READ:  return RETRY;
            case SSL_ERROR_WANT_WRITE: return RETRY;
            default:                   return ERROR;
        }
    }
    *n = (size_t) r;
    return OK;
}

status ssl_write(connection *c, char *buf, size_t len, size_t *n) {
    int r;
    if ((r = SSL_write(c->ssl, buf, len)) <= 0) {
        switch (SSL_get_error(c->ssl, r)) {
            case SSL_ERROR_WANT_READ:  return RETRY;
            case SSL_ERROR_WANT_WRITE: return RETRY;
            default:                   return ERROR;
        }
    }
    *n = (size_t) r;
    return OK;
}

size_t ssl_readable(connection *c) {
    return SSL_pending(c->ssl);
}
