// Minimal Prometheus exporter: one listener thread, one response per
// connection, body produced by the caller's render function.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "prometheus.h"

#ifdef _WIN32
typedef SOCKET psock;
#define PSOCK_INVALID INVALID_SOCKET
#define psock_close closesocket
#define psock_sleep_ms(ms) Sleep(ms)
#else
#include <netdb.h>
#include <time.h>
typedef int psock;
#define PSOCK_INVALID (-1)
#define psock_close close
static void psock_sleep_ms(int ms) {
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

static psock listener = PSOCK_INVALID;
static prom_render_fn render;

static void send_all(psock s, const char *buf, size_t len) {
    while (len > 0) {
        int n = send(s, buf, (int) len, 0);
        if (n <= 0) return;
        buf += n;
        len -= (size_t) n;
    }
}

static void *serve(void *arg) {
    (void) arg;
    for (;;) {
        psock c = accept(listener, NULL, NULL);
        if (c == PSOCK_INVALID) {
            psock_sleep_ms(10);   // e.g. out of file descriptors: don't spin
            continue;
        }
        char req[2048];
        (void) recv(c, req, sizeof(req), 0);   // any request gets the metrics

        size_t len = 0;
        char *body = render(&len);
        char head[160];
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n", len);
        send_all(c, head, (size_t) n);
        send_all(c, body, len);
        free(body);
        psock_close(c);
    }
    return NULL;
}

int prom_start(const char *spec, prom_render_fn fn) {
    // [HOST:]PORT, HOST may be [IPv6]; default host 127.0.0.1
    char buf[256], *host = "127.0.0.1", *port = buf;
    if (strlen(spec) >= sizeof(buf)) return -1;
    strcpy(buf, spec);
    char *colon = strrchr(buf, ':');
    if (colon) {
        *colon = '\0';
        port = colon + 1;
        host = buf;
        size_t hl = strlen(host);
        if (hl >= 2 && host[0] == '[' && host[hl - 1] == ']') {
            host[hl - 1] = '\0';
            host++;
        }
    }

    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &ai) != 0) return -2;

    listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int one = 1;
    int ok = listener != PSOCK_INVALID &&
             setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one)) == 0 &&
             bind(listener, ai->ai_addr, (socklen_t) ai->ai_addrlen) == 0 &&
             listen(listener, 16) == 0;
    freeaddrinfo(ai);
    if (!ok) return -1;

    render = fn;
    pthread_t t;
    if (pthread_create(&t, NULL, serve, NULL)) return -1;
    pthread_detach(t);
    return 0;
}
