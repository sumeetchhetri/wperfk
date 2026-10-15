// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include "wrk.h"
#include "script.h"
#include "main.h"
#include "hdr_histogram.h"
#include "stats.h"
#include "prometheus.h"

// Max recordable latency of 1 day
#define MAX_LATENCY (24LL * 60 * 60 * 1000000)

static struct config {
    uint64_t threads;
    uint64_t connections;
    uint64_t duration;
    uint64_t timeout;
    uint64_t pipeline;
    uint64_t rate;
    uint64_t delay_ms;
    bool     latency;
    bool     u_latency;
    bool     dynamic;
    bool     record_all_responses;
    bool     json;
    bool     delay;
    bool     duration_set;
    uint64_t requests;
    uint64_t warmup;
    uint64_t bailout;
    char    *method;
    char    *body;
    size_t   body_len;
    char    *host;
    char    *script;
    SSL_CTX *ctx;
    uint64_t progress;
    char    *targets;
    char    *hist_out;
    char    *expect_body;
    bool     expect_status_set;
    bool     expect_status[600];
    bool     status_codes;
    tls_opts tls;
    char    *resolve;
    char    *unix_socket;
    char    *prometheus;
} cfg;

static struct {
    stats *requests;
    pthread_mutex_t mutex;
} statistics;

static struct sock sock = {
    .connect  = sock_connect,
    .close    = sock_close,
    .read     = sock_read,
    .write    = sock_write,
    .readable = sock_readable
};

static llhttp_settings_t parser_settings = {
    .on_message_begin    = response_begin,
    .on_message_complete = response_complete
};

static volatile sig_atomic_t stop = 0;

// Run-level stop reasons and shared counters (updated atomically by threads).
enum { STOP_DURATION, STOP_REQUESTS, STOP_BAILOUT, STOP_SIGNAL, STOP_MERGED };
static const char *stop_names[] = { "duration", "requests", "bailout", "signal", "merged" };
static volatile int stopped_by = STOP_DURATION;
static uint64_t responses_total = 0;
static uint64_t errors_total    = 0;
static uint64_t warmup_end      = 0;
static int      threads_running = 0;
static thread  *all_threads     = NULL;
static char    *run_url         = NULL;

static void handler(int sig) {
    stopped_by = STOP_SIGNAL;
    stop = 1;
}

static void usage() {
    printf("Usage: wperfk <options> <url>                         \n"
           "  Options:                                            \n"
           "    -c, --connections <N>  Connections to keep open   \n"
           "    -d, --duration    <T>  Duration of test           \n"
           "    -t, --threads     <N>  Number of threads to use   \n"
           "                                                      \n"
           "    -s, --script      <S>  Load Lua script file       \n"
           "    -H, --header      <H>  Add header to request      \n"
           "    -L  --latency          Print latency statistics   \n"
           "    -U  --u_latency        Print uncorrected latency statistics\n"
           "        --timeout     <T>  Socket/request timeout     \n"
           "    -B, --batch_latency    Measure latency of whole   \n"
           "                           batches of pipelined ops   \n"
           "                           (as opposed to each op)    \n"
           "    -v, --version          Print version details      \n"
           "    -R, --rate        <T>  constant rate in requests/sec (total).   \n"
           "                           Omit for closed-loop max throughput.     \n"
           "                           Also N/<dur>: 50/1s, 3000/1m, 5/100ms     \n"
           "    -p, --pipeline    <N>  Pipeline N requests per connection       \n"
           "    -n, --requests    <N>  Stop after N responses (no -d: no limit) \n"
           "        --warmup      <T>  Warm up for T, excluded from results     \n"
           "        --bailout     <N>  Stop after N errors (exit code 2)        \n"
           "    -m, --method      <M>  HTTP method (default GET)                \n"
           "        --body        <S>  Request body                             \n"
           "        --body-file   <F>  Request body from file                   \n"
           "        --json             Print JSON summary on stdout; all other   \n"
           "                           output (incl. Lua print) goes to stderr  \n"
           "        --progress    <T>  Print live progress to stderr every T    \n"
           "        --targets     <F>  Request targets: vegeta HTTP format or HAR,\n"
           "                           sent round-robin; URL may be omitted     \n"
           "        --status-codes     Print responses per status code          \n"
           "        --expect-status <L> Allowed codes, e.g. 200,204 or 2xx       \n"
           "        --expect-body <S>  Response body must contain S             \n"
           "                           (mismatches count as errors; exit 3)     \n"
           "        --hist-out    <F>  Save latency histograms for `wperfk merge`\n"
           "        --verify           Verify the server's TLS certificate      \n"
           "        --cacert      <F>  CA bundle for --verify (implies it)      \n"
           "        --cert/--key  <F>  Client certificate and key (mTLS, PEM)   \n"
           "        --sni         <S>  TLS server name (default: URL host)      \n"
           "        --resolve <H:P:A>  Connect to address A for host H port P   \n"
           "        --unix-socket <P>  Connect to a Unix socket; the URL still   \n"
           "                           sets Host, path and scheme               \n"
           "        --prometheus <[H:]P> Serve live metrics for Prometheus      \n"
           "                           (default host 127.0.0.1)                 \n"
           "                                                      \n"
           "  wperfk merge [--json] <hist files...>  Combine --hist-out results \n"
           "                                                      \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
           "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}

int main(int argc, char **argv) {
    char *url, **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};

    if (argc > 1 && !strcmp(argv[1], "merge")) {
        return merge_main(argc - 1, argv + 1);
    }

    if (parse_args(&cfg, &url, &parts, headers, argc, argv)) {
        usage();
        exit(1);
    }

    if (wpk_net_init()) {
        fprintf(stderr, "unable to initialize networking\n");
        exit(1);
    }

    FILE *json_out = NULL;
    if (cfg.json) {
        fflush(stdout);
        json_out = fdopen(dup(STDOUT_FILENO), "w");
        dup2(STDERR_FILENO, STDOUT_FILENO);
    }

    script_request_defaults(cfg.method, cfg.body, cfg.body_len, cfg.pipeline);
    script_targets(cfg.targets);

    char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
    char *host    = copy_url_part(url, &parts, UF_HOST);
    char *port    = copy_url_part(url, &parts, UF_PORT);
    char *service = port ? port : schema;

    if (!strncmp("https", schema, 5)) {
        if ((cfg.ctx = ssl_init(&cfg.tls)) == NULL) {
            fprintf(stderr, "unable to initialize SSL\n");
            ERR_print_errors_fp(stderr);
            exit(1);
        }
        sock.connect  = ssl_connect;
        sock.close    = ssl_close;
        sock.read     = ssl_read;
        sock.write    = ssl_write;
        sock.readable = ssl_readable;
    }
	
    cfg.host = cfg.tls.sni ? (char *) cfg.tls.sni : host;

    char *connect_host = host, *connect_service = service;
    if (cfg.resolve && apply_resolve(cfg.resolve, host, service, schema, &connect_host, &connect_service)) {
        exit(1);
    }
	
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    signal(SIGINT,  SIG_IGN);

    pthread_mutex_init(&statistics.mutex, NULL);
    statistics.requests = stats_alloc(10);
    thread *threads = zcalloc(cfg.threads * sizeof(thread));
    all_threads = threads;
    run_url     = url;

    hdr_init(1, MAX_LATENCY, 3, &(statistics.requests->histogram));


    lua_State *L = script_create(cfg.script, url, headers);
    if (cfg.unix_socket) {
        if (!script_unix_addr(L, cfg.unix_socket)) {
            fprintf(stderr, "unable to connect to unix socket %s\n", cfg.unix_socket);
            exit(1);
        }
    } else if (!script_resolve(L, connect_host, connect_service)) {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to connect to %s:%s %s\n", connect_host, connect_service, msg);
        exit(1);
    }
    
    uint64_t connections = cfg.connections / cfg.threads;
    double throughput    = (double)cfg.rate / cfg.threads;
    if (cfg.requests && !cfg.duration_set) {
        cfg.duration = 100 * 365 * 24 * 3600ULL; // -n without -d: run until N responses
    }
    if (cfg.prometheus) {
        int rc = prom_start(cfg.prometheus, render_metrics);
        if (rc) {
            fprintf(stderr, "unable to serve Prometheus metrics on %s: %s\n", cfg.prometheus,
                    rc == -2 ? "cannot resolve address" : strerror(errno));
            exit(1);
        }
    }

    warmup_end           = time_us() + (cfg.warmup * 1000000);
    uint64_t stop_at     = warmup_end + (cfg.duration * 1000000);

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        t->loop        = aeCreateEventLoop(10 + cfg.connections * 3);
        t->connections = connections;
        t->throughput = throughput;
        t->stop_at     = stop_at;

        t->L = script_create(cfg.script, url, headers);
        script_init(L, t, argc - optind, &argv[optind]);

        if (i == 0) {
            uint64_t pipeline = cfg.pipeline;
            cfg.pipeline = script_verify_request(t->L);
            cfg.dynamic = !script_is_static(t->L);
            if (pipeline > 1 && cfg.dynamic) {
                fprintf(stderr, "warning: -p ignored, script defines request()\n");
            }
            cfg.delay = script_has_delay(t->L);
            if (cfg.delay && cfg.rate) {
                fprintf(stderr, "warning: script delay() ignored in -R mode (the rate scheduler paces requests)\n");
                cfg.delay = false;
            }
            if (script_want_response(t->L)) {
                parser_settings.on_header_field = header_field;
                parser_settings.on_header_value = header_value;
                parser_settings.on_body         = response_body;
            }
            if (cfg.expect_body) {
                parser_settings.on_body = response_body;
            }
        }

        __sync_add_and_fetch(&threads_running, 1);
        if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to create thread %"PRIu64": %s\n", i, msg);
            exit(2);
        }
    }

#ifdef _WIN32
    // Ctrl+C / Ctrl+Break: the CRT runs the handler on its own thread;
    // event loops see `stop` within LIMITS_INTERVAL_MS.
    signal(SIGINT,   handler);
    signal(SIGBREAK, handler);
#else
    struct sigaction sa = {
        .sa_handler = handler,
        .sa_flags   = 0,
    };
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
#endif

    if (cfg.requests && !cfg.duration_set) {
        printf("Running until %"PRIu64" responses @ %s\n", cfg.requests, url);
    } else {
        char *time = format_time_s(cfg.duration);
        printf("Running %s test @ %s\n", time, url);
    }
    if (cfg.warmup) {
        printf("  %"PRIu64"s warmup (excluded from results)\n", cfg.warmup);
    }
    printf("  %"PRIu64" threads and %"PRIu64" connections\n",
            cfg.threads, cfg.connections);
    if (cfg.rate) {
        printf("  Mode: constant rate %"PRIu64" req/s (latency corrected for coordinated omission)\n", cfg.rate);
    } else {
        printf("  Mode: closed loop, max throughput (latency NOT corrected for coordinated omission)\n");
    }

    uint64_t start    = warmup_end;
    uint64_t complete = 0;
    uint64_t bytes    = 0;
    errors errors     = { 0 };
    uint64_t *status_codes = zcalloc(600 * sizeof(uint64_t));

    struct hdr_histogram* latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &latency_histogram);
    struct hdr_histogram* u_latency_histogram;
    hdr_init(1, MAX_LATENCY, 3, &u_latency_histogram);

    if (cfg.progress) {
        progress_loop(threads);
    }

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        pthread_join(t->thread, NULL);
    }

    uint64_t runtime_us = time_us() - start;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        complete += t->complete;
        bytes    += t->bytes;

        errors.connect += t->errors.connect;
        errors.read    += t->errors.read;
        errors.write   += t->errors.write;
        errors.timeout += t->errors.timeout;
        errors.status  += t->errors.status;
        errors.mismatch += t->errors.mismatch;
        for (int s = 0; s < 600; s++) status_codes[s] += t->status_codes[s];

        hdr_add(latency_histogram, t->latency_histogram);
        hdr_add(u_latency_histogram, t->u_latency_histogram);
    }

    long double runtime_s   = runtime_us / 1000000.0;
    long double req_per_s   = complete   / runtime_s;
    long double bytes_per_s = bytes      / runtime_s;

    if (statistics.requests->histogram->total_count == 0) {
        stats_record(statistics.requests, (uint64_t) req_per_s);
    }

    stats *latency_stats = stats_alloc(10);
    latency_stats->min = hdr_min(latency_histogram);
    latency_stats->max = hdr_max(latency_histogram);
    latency_stats->histogram = latency_histogram;

    print_stats_header();
    print_stats("Latency", latency_stats, format_time_us);
    print_stats("Req/Sec", statistics.requests, format_metric);
//    if (cfg.latency) print_stats_latency(latency_stats);

    if (cfg.latency) {
        print_hdr_latency(latency_histogram,
                "Recorded Latency");
        printf("----------------------------------------------------------\n");
    }

    if (cfg.u_latency) {
        printf("\n");
        print_hdr_latency(u_latency_histogram,
                "Uncorrected Latency (measured without taking delayed starts into account)");
        printf("----------------------------------------------------------\n");
    }

    char *runtime_msg = format_time_us(runtime_us);

    printf("  %"PRIu64" requests in %s, %sB read\n",
            complete, runtime_msg, format_binary(bytes));
    if (errors.connect || errors.read || errors.write || errors.timeout) {
        printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
               errors.connect, errors.read, errors.write, errors.timeout);
    }

    if (errors.status) {
        printf("  Non-2xx or 3xx responses: %d\n", errors.status);
    }
    if (errors.mismatch) {
        printf("  Unexpected responses (--expect-*): %d\n", errors.mismatch);
    }
    if (cfg.status_codes) {
        printf("  Status codes:");
        for (int s = 0; s < 600; s++) {
            if (status_codes[s]) printf(" %d=%"PRIu64, s, status_codes[s]);
        }
        printf("\n");
    }

    printf("Requests/sec: %9.2Lf\n", req_per_s);
    printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));

    if (script_has_done(L)) {
        script_summary(L, runtime_us, complete, bytes);
        script_errors(L, &errors);
        script_done(L, latency_stats, statistics.requests);
    }

    if (json_out) {
        print_json(json_out, url, runtime_us, complete, bytes, &errors,
                   latency_histogram, u_latency_histogram, status_codes);
        fclose(json_out);
    }

    if (cfg.hist_out) {
        hist_meta m = {
            .runtime_us = runtime_us, .requests = complete, .bytes = bytes,
            .rate = cfg.rate, .threads = cfg.threads, .connections = cfg.connections,
            .errors = errors,
        };
        if (write_hist(cfg.hist_out, url, &m, status_codes, latency_histogram, u_latency_histogram)) {
            fprintf(stderr, "unable to write %s: %s\n", cfg.hist_out, strerror(errno));
            return 1;
        }
    }

    if (stopped_by == STOP_BAILOUT) {
        fflush(stdout);
        fprintf(stderr, "Bailed out after %"PRIu64" errors\n", errors_total);
        return 2;
    }
    if (errors.mismatch) {
        return 3;
    }
    return 0;
}

void *thread_main(void *arg) {
    thread *thread = arg;
    aeEventLoop *loop = thread->loop;

    thread->cs = zcalloc(thread->connections * sizeof(connection));
    tinymt64_init(&thread->rand, time_us());
    hdr_init(1, MAX_LATENCY, 3, &thread->latency_histogram);
    hdr_init(1, MAX_LATENCY, 3, &thread->u_latency_histogram);

    char *request = NULL;
    size_t length = 0;

    if (!cfg.dynamic) {
        script_request(thread->L, &request, &length);
    }

    double throughput = (thread->throughput / 1000000.0) / thread->connections;

    connection *c = thread->cs;

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        c->thread     = thread;
        c->ssl        = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        c->request    = request;
        c->length     = length;
        c->throughput = throughput;
        c->catch_up_throughput = throughput * 2;
        c->complete   = 0;
        c->caught_up  = true;
        // Rate mode: stagger connects 5 msec apart within thread.
        // Closed loop: connect all at once, like wrk.
        aeCreateTimeEvent(loop, cfg.rate ? i * 5 : 0, delayed_initial_connect, c, NULL);
    }

    uint64_t stagger = cfg.rate ? thread->connections * 5 : 0;
    uint64_t timeout_delay = TIMEOUT_INTERVAL_MS + stagger;

    if (cfg.rate) {
        aeCreateTimeEvent(loop, CALIBRATE_DELAY_MS + stagger, calibrate, thread, NULL);
    } else {
        thread->interval = RECORD_INTERVAL_MS;
        aeCreateTimeEvent(loop, thread->interval, sample_rate, thread, NULL);
    }
    aeCreateTimeEvent(loop, timeout_delay, check_timeouts, thread, NULL);
    aeCreateTimeEvent(loop, LIMITS_INTERVAL_MS, check_limits, thread, NULL);
    if (cfg.warmup) {
        uint64_t now = time_us();
        thread->warming = true;
        aeCreateTimeEvent(loop, warmup_end > now ? (warmup_end - now) / 1000 : 0,
                          end_warmup, thread, NULL);
    }

    thread->start = time_us();
    aeMain(loop);

    aeDeleteEventLoop(loop);
    zfree(thread->cs);

    __sync_sub_and_fetch(&threads_running, 1);
    return NULL;
}

static int connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags;

    fd = wpk_socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (fd < 0) goto error;

    wpk_nonblock(fd);

    if (wpk_connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (!wpk_connect_pending()) goto error;
    }

    wpk_nodelay(fd);

    c->latest_connect = time_us();

    flags = AE_READABLE | AE_WRITABLE;
    if (aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK) {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }

  error:
    thread->errors.connect++;
    if (fd >= 0) wpk_close(fd);
    return -1;
}

static int reconnect_socket(thread *thread, connection *c) {
    if (cfg.bailout) report_errors(thread); // every connect/read/write error path lands here
    aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    sock.close(c);
    wpk_close(c->fd);
    return connect_socket(thread, c);
}

static int delayed_initial_connect(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    c->thread_start = time_us();
    connect_socket(c->thread, c);
    return AE_NOMORE;
}

static int calibrate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    long double mean = hdr_mean(thread->latency_histogram);
    long double latency = hdr_value_at_percentile(
            thread->latency_histogram, 90.0) / 1000.0L;
    long double interval = MAX(latency * 2, 10);

    if (mean == 0) return CALIBRATE_DELAY_MS;

    thread->mean     = (uint64_t) mean;
    hdr_reset(thread->latency_histogram);
    hdr_reset(thread->u_latency_histogram);

    thread->start    = time_us();
    thread->interval = interval;
    thread->requests = 0;

    printf("  Thread calibration: mean lat.: %.3fms, rate sampling interval: %dms\n",
            (thread->mean)/1000.0,
            thread->interval);

    aeCreateTimeEvent(loop, thread->interval, sample_rate, thread, NULL);

    return AE_NOMORE;
}

static void report_errors(thread *thread) {
    errors *e = &thread->errors;
    uint64_t sum = (uint64_t) e->connect + e->read + e->write + e->timeout + e->status + e->mismatch;
    if (sum > thread->errors_reported) {
        uint64_t total = __sync_add_and_fetch(&errors_total, sum - thread->errors_reported);
        thread->errors_reported = sum;
        if (cfg.bailout && total >= cfg.bailout && !stop) {
            stopped_by = STOP_BAILOUT;
            stop = 1;
        }
    }
}

static int check_limits(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    report_errors(thread);
    if (stop) {
        aeStop(loop);
    }
    return LIMITS_INTERVAL_MS;
}

static int end_warmup(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    report_errors(thread); // warmup errors still count towards --bailout

    thread->complete = 0;
    thread->requests = 0;
    thread->bytes    = 0;
    memset(&thread->errors, 0, sizeof(thread->errors));
    memset(thread->status_codes, 0, sizeof(thread->status_codes));
    thread->errors_reported = 0;
    hdr_reset(thread->latency_histogram);
    hdr_reset(thread->u_latency_histogram);
    thread->start   = time_us();
    thread->warming = false;
    return AE_NOMORE;
}

static int check_timeouts(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;
    connection *c  = thread->cs;
    uint64_t now   = time_us();

    uint64_t maxAge = now - (cfg.timeout * 1000);

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        if (maxAge > c->start) {
            thread->errors.timeout++;
        }
    }

    if (stop || now >= thread->stop_at) {
        aeStop(loop);
    }

    return TIMEOUT_INTERVAL_MS;
}

static int sample_rate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
    uint64_t requests = (thread->requests / (double) elapsed_ms) * 1000;

    if (!thread->warming) {
        pthread_mutex_lock(&statistics.mutex);
        stats_record(statistics.requests, requests);
        pthread_mutex_unlock(&statistics.mutex);
    }

    thread->requests = 0;
    thread->start    = time_us();

    return thread->interval;
}

static int header_field(llhttp_t *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == VALUE) {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int header_value(llhttp_t *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == FIELD) {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int response_body(llhttp_t *parser, const char *at, size_t len) {
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}

static uint64_t usec_to_next_send(connection *c) {
    uint64_t now = time_us();

    if (!cfg.rate) return 0; // closed loop: send as soon as possible

    uint64_t next_start_time = c->thread_start + (c->complete / c->throughput);

    bool send_now = true;

    if (next_start_time > now) {
        // We are on pace. Indicate caught_up and don't send now.
        c->caught_up = true;
        send_now = false;
    } else {
        // We are behind
        if (c->caught_up) {
            // This is the first fall-behind since we were last caught up
            c->caught_up = false;
            c->catch_up_start_time = now;
            c->complete_at_catch_up_start = c->complete;
        }

        // Figure out if it's time to send, per catch up throughput:
        uint64_t complete_since_catch_up_start =
                c->complete - c->complete_at_catch_up_start;

        next_start_time = c->catch_up_start_time +
                (complete_since_catch_up_start / c->catch_up_throughput);

        if (next_start_time > now) {
            // Not yet time to send, even at catch-up throughout:
            send_now = false;
        }
    }

    if (send_now) {
        c->latest_should_send_time = now;
        c->latest_expected_start = next_start_time;
    }

    return send_now ? 0 : (next_start_time - now);
}

static int delay_request(aeEventLoop *loop, long long id, void *data) {
    connection* c = data;
    uint64_t time_usec_to_wait = usec_to_next_send(c);
    if (time_usec_to_wait) {
        return round((time_usec_to_wait / 1000.0L) + 0.5); /* don't send, wait */
    }
    aeCreateFileEvent(c->thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}

static int response_begin(llhttp_t *parser) {
    connection *c = parser->data;
    c->in_response = true;
    return 0;
}

static int response_complete(llhttp_t *parser) {
    connection *c = parser->data;
    thread *thread = c->thread;
    uint64_t now = time_us();
    int status = parser->status_code;
    bool last = false;

    c->in_response = false;

    if (stop) {
        aeStop(thread->loop);
        goto done;
    }

    if (cfg.requests && !thread->warming) {
        uint64_t n = __sync_add_and_fetch(&responses_total, 1);
        if (n > cfg.requests) {
            aeStop(thread->loop);
            goto done;
        }
        last = (n == cfg.requests);
    }

    thread->complete++;
    thread->requests++;

    if (status > 399) {
        thread->errors.status++;
        if (cfg.bailout) report_errors(thread);
    }
    if (status > 0 && status < 600) {
        thread->status_codes[status]++;
    }

    if ((cfg.expect_status_set && (status <= 0 || status >= 600 || !cfg.expect_status[status])) ||
        (cfg.expect_body && !contains(c->body.buffer, c->body.cursor - c->body.buffer, cfg.expect_body))) {
        thread->errors.mismatch++;
        if (cfg.bailout) report_errors(thread);
    }

    if (c->headers.buffer) {
        *c->headers.cursor++ = '\0';
        script_response(thread->L, status, &c->headers, &c->body);
        c->state = FIELD;
    } else if (cfg.expect_body) {
        buffer_reset(&c->body);
    }

    if (now >= thread->stop_at) {
        aeStop(thread->loop);
        goto done;
    }

    // Count all responses (including pipelined ones:)
    c->complete++;

    // Note that expected start time is computed based on the completed
    // response count seen at the beginning of the last request batch sent.
    // A single request batch send may contain multiple requests, and
    // result in multiple responses. If we incorrectly calculated expect
    // start time based on the completion count of these individual pipelined
    // requests we can easily end up "gifting" them time and seeing
    // negative latencies.
    uint64_t expected_latency_start = cfg.rate
            ? c->thread_start + (c->complete_at_last_batch_start / c->throughput)
            : c->actual_latency_start;

    int64_t expected_latency_timing = now - expected_latency_start;

    if (expected_latency_timing < 0) {
        printf("\n\n ---------- \n\n");
        printf("We are about to crash and die (recoridng a negative #)");
        printf("This wil never ever ever happen...");
        printf("But when it does. The following information will help in debugging");
        printf("response_complete:\n");
        printf("  expected_latency_timing = %lld\n", (long long) expected_latency_timing);
        printf("  now = %lld\n", (long long) now);
        printf("  expected_latency_start = %lld\n", (long long) expected_latency_start);
        printf("  c->thread_start = %lld\n", (long long) c->thread_start);
        printf("  c->complete = %lld\n", (long long) c->complete);
        printf("  throughput = %g\n", c->throughput);
        printf("  latest_should_send_time = %lld\n", (long long) c->latest_should_send_time);
        printf("  latest_expected_start = %lld\n", (long long) c->latest_expected_start);
        printf("  latest_connect = %lld\n", (long long) c->latest_connect);
        printf("  latest_write = %lld\n", (long long) c->latest_write);

        expected_latency_start = c->thread_start +
                ((c->complete ) / c->throughput);
        printf("  next expected_latency_start = %lld\n", (long long) expected_latency_start);
    }

    c->latest_should_send_time = 0;
    c->latest_expected_start = 0;

    if (--c->pending == 0) {
        c->has_pending = false;
        aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    // Record if needed, either last in batch or all, depending in cfg:
    if (cfg.record_all_responses || !c->has_pending) {
        hdr_record_value(thread->latency_histogram, expected_latency_timing);

        uint64_t actual_latency_timing = now - c->actual_latency_start;
        hdr_record_value(thread->u_latency_histogram, actual_latency_timing);
    }

    if (last) {
        stopped_by = STOP_REQUESTS;
        stop = 1;
        aeStop(thread->loop);
    }


    if (!llhttp_should_keep_alive(parser)) {
        reconnect_socket(thread, c);
        goto done;
    }
    // llhttp continues with the next response on this connection by itself.

  done:
    return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;

    switch (sock.connect(c, cfg.host)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    llhttp_init(&c->parser, HTTP_RESPONSE, &parser_settings);
    c->in_response = false;
    llhttp_set_lenient_optional_cr_before_lf(&c->parser, 1); // http_parser accepted bare LF
    c->parser.data = c;
    c->written = 0;

    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);

    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);

    return;

  error:
    c->thread->errors.connect++;
    reconnect_socket(c->thread, c);

}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    thread *thread = c->thread;

    if (!c->written) {
        uint64_t time_usec_to_wait = usec_to_next_send(c);
        if (time_usec_to_wait) {
            int msec_to_wait = round((time_usec_to_wait / 1000.0L) + 0.5);

            // Not yet time to send. Delay:
            aeDeleteFileEvent(loop, fd, AE_WRITABLE);
            aeCreateTimeEvent(
                    thread->loop, msec_to_wait, delay_request, c, NULL);
            return;
        }
        if (cfg.delay && !c->delayed) {
            uint64_t msec = script_delay(thread->L);
            if (msec > 0) {
                c->delayed = true;
                aeDeleteFileEvent(loop, fd, AE_WRITABLE);
                aeCreateTimeEvent(thread->loop, msec, delay_request, c, NULL);
                return;
            }
        }
        c->delayed = false;
        c->latest_write = time_us();
    }

    if (!c->written && cfg.dynamic) {
        script_request(thread->L, &c->request, &c->length);
    }

    char  *buf = c->request + c->written;
    size_t len = c->length  - c->written;
    size_t n;

    if (!c->written) {
        c->start = time_us();
        if (!c->has_pending) {
            c->actual_latency_start = c->start;
            c->complete_at_last_batch_start = c->complete;
            c->has_pending = true;
        }
        c->pending = cfg.pipeline;
    }

    switch (sock.write(c, buf, len, &n)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    c->written += n;
    if (c->written == c->length) {
        c->written = 0;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }

    return;

  error:
    thread->errors.write++;
    reconnect_socket(thread, c);
}


static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    size_t n;

    do {
        switch (sock.read(c, &n)) {
            case OK:    break;
            case ERROR: goto error;
            case RETRY: return;
        }

        if (n == 0) {
            // EOF completes a close-delimited response (response_complete then
            // reconnects, unless the run is stopping). EOF anywhere else, idle
            // or mid-body, means the server closed on us: a read error.
            bool completes = c->in_response && llhttp_message_needs_eof(&c->parser);
            if (llhttp_finish(&c->parser) != HPE_OK || !completes) goto error;
            return;
        }
        if (llhttp_execute(&c->parser, c->buf, n) != HPE_OK) goto error;
        c->thread->bytes += n;
    } while (n == RECVBUF && sock.readable(c) > 0);

    return;

  error:
    c->thread->errors.read++;
    reconnect_socket(c->thread, c);
}

static uint64_t time_us() {
    // Monotonic: immune to NTP/wall-clock steps during a run.
#ifdef _WIN32
    LARGE_INTEGER count, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return ((uint64_t) (count.QuadPart / freq.QuadPart) * 1000000) +
           ((uint64_t) (count.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t) t.tv_sec * 1000000) + (t.tv_nsec / 1000);
#endif
}

static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field) {
    char *part = NULL;

    if (parts->field_set & (1 << field)) {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}

static struct option longopts[] = {
    { "connections",    required_argument, NULL, 'c' },
    { "duration",       required_argument, NULL, 'd' },
    { "threads",        required_argument, NULL, 't' },
    { "script",         required_argument, NULL, 's' },
    { "header",         required_argument, NULL, 'H' },
    { "latency",        no_argument,       NULL, 'L' },
    { "u_latency",      no_argument,       NULL, 'U' },
    { "batch_latency",  no_argument,       NULL, 'B' },
    { "timeout",        required_argument, NULL, 'T' },
    { "help",           no_argument,       NULL, 'h' },
    { "version",        no_argument,       NULL, 'v' },
    { "rate",           required_argument, NULL, 'R' },
    { "method",         required_argument, NULL, 'm' },
    { "body",           required_argument, NULL, OPT_BODY },
    { "body-file",      required_argument, NULL, OPT_BODY_FILE },
    { "json",           no_argument,       NULL, OPT_JSON },
    { "pipeline",       required_argument, NULL, 'p' },
    { "requests",       required_argument, NULL, 'n' },
    { "warmup",         required_argument, NULL, OPT_WARMUP },
    { "bailout",        required_argument, NULL, OPT_BAILOUT },
    { "progress",       required_argument, NULL, OPT_PROGRESS },
    { "targets",        required_argument, NULL, OPT_TARGETS },
    { "hist-out",       required_argument, NULL, OPT_HIST_OUT },
    { "expect-status",  required_argument, NULL, OPT_EXPECT_STATUS },
    { "expect-body",    required_argument, NULL, OPT_EXPECT_BODY },
    { "status-codes",   no_argument,       NULL, OPT_STATUS_CODES },
    { "verify",         no_argument,       NULL, OPT_VERIFY },
    { "cacert",         required_argument, NULL, OPT_CACERT },
    { "cert",           required_argument, NULL, OPT_CERT },
    { "key",            required_argument, NULL, OPT_KEY },
    { "sni",            required_argument, NULL, OPT_SNI },
    { "resolve",        required_argument, NULL, OPT_RESOLVE },
    { "unix-socket",    required_argument, NULL, OPT_UNIX },
    { "prometheus",     required_argument, NULL, OPT_PROMETHEUS },
    { NULL,             0,                 NULL,  0  }
};

static int parse_args(struct config *cfg, char **url, struct http_parser_url *parts, char **headers, int argc, char **argv) {
    int c;
    bool version = false;
    char **header = headers;

    memset(cfg, 0, sizeof(struct config));
    cfg->threads     = 2;
    cfg->connections = 10;
    cfg->duration    = 10;
    cfg->timeout     = SOCKET_TIMEOUT_MS;
    cfg->rate        = 0;
    cfg->record_all_responses = true;

    while ((c = getopt_long(argc, argv, "t:c:d:s:H:T:R:m:p:n:LUBrv?", longopts, NULL)) != -1) {
        switch (c) {
            case 't':
                if (scan_metric(optarg, &cfg->threads)) return -1;
                break;
            case 'c':
                if (scan_metric(optarg, &cfg->connections)) return -1;
                break;
            case 'd':
                if (scan_time(optarg, &cfg->duration)) return -1;
                cfg->duration_set = true;
                break;
            case 's':
                cfg->script = optarg;
                break;
            case 'H':
                *header++ = optarg;
                break;
            case 'L':
                cfg->latency = true;
                break;
            case 'B':
                cfg->record_all_responses = false;
                break;
            case 'U':
                cfg->latency = true;
                cfg->u_latency = true;
                break;
            case 'T':
                if (scan_time(optarg, &cfg->timeout)) return -1;
                cfg->timeout *= 1000;
                break;
            case 'R':
                if (scan_rate(optarg, &cfg->rate)) {
                    fprintf(stderr, "invalid rate %s (use N or N/<dur>, e.g. 50/1s, 3000/1m; >= 1 req/s)\n", optarg);
                    return -1;
                }
                break;
            case 'p':
                if (scan_metric(optarg, &cfg->pipeline) || !cfg->pipeline) return -1;
                break;
            case 'n':
                if (scan_metric(optarg, &cfg->requests) || !cfg->requests) return -1;
                break;
            case OPT_WARMUP:
                if (scan_time(optarg, &cfg->warmup)) return -1;
                break;
            case OPT_BAILOUT:
                if (scan_metric(optarg, &cfg->bailout) || !cfg->bailout) return -1;
                break;
            case OPT_PROGRESS:
                if (scan_time(optarg, &cfg->progress) || !cfg->progress) return -1;
                break;
            case OPT_TARGETS:
                cfg->targets = optarg;
                break;
            case OPT_HIST_OUT:
                cfg->hist_out = optarg;
                break;
            case OPT_EXPECT_STATUS:
                if (parse_expect_status(optarg, cfg->expect_status)) {
                    fprintf(stderr, "invalid --expect-status %s (use e.g. 200,204 or 2xx)\n", optarg);
                    return -1;
                }
                cfg->expect_status_set = true;
                break;
            case OPT_EXPECT_BODY:
                cfg->expect_body = optarg;
                break;
            case OPT_STATUS_CODES:
                cfg->status_codes = true;
                break;
            case OPT_VERIFY:
                cfg->tls.verify = true;
                break;
            case OPT_CACERT:
                cfg->tls.cacert = optarg;
                cfg->tls.verify = true;
                break;
            case OPT_CERT:
                cfg->tls.cert = optarg;
                break;
            case OPT_KEY:
                cfg->tls.key = optarg;
                break;
            case OPT_SNI:
                cfg->tls.sni = optarg;
                break;
            case OPT_RESOLVE:
                cfg->resolve = optarg;
                break;
            case OPT_UNIX:
                cfg->unix_socket = optarg;
                break;
            case OPT_PROMETHEUS:
                cfg->prometheus = optarg;
                break;
            case 'm':
                cfg->method = optarg;
                break;
            case OPT_BODY:
                cfg->body     = optarg;
                cfg->body_len = strlen(optarg);
                break;
            case OPT_BODY_FILE:
                if (read_file(optarg, &cfg->body, &cfg->body_len)) {
                    fprintf(stderr, "unable to read body file %s: %s\n", optarg, strerror(errno));
                    return -1;
                }
                break;
            case OPT_JSON:
                cfg->json = true;
                break;
            case 'v':
                printf("wperfk %s [%s]\n", VERSION, aeGetApiName());
                printf("Based on wrk2 (C) 2014 Gil Tene, Mike Barker and wrk (C) 2012 Will Glozer and Extended/Updated by Sumeet Chhetri 2026\n");
                version = true;
                break;
            case 'h':
            case '?':
            case ':':
            default:
                return -1;
        }
    }

    if (version && optind == argc) exit(0); // plain `wperfk -v`

    char *target_url = argv[optind];
    if (optind == argc && cfg->targets) {
        if (!(target_url = targets_first_url(cfg->targets))) return -1;
    }

    if (!target_url || !cfg->threads || !cfg->duration) return -1;

    if (!script_parse_url(target_url, parts)) {
        fprintf(stderr, "invalid URL: %s\n", target_url);
        return -1;
    }

    if (!cfg->connections || cfg->connections < cfg->threads) {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    *url    = target_url;
    *header = NULL;

    return 0;
}

static void print_stats_header() {
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double)) {
    uint64_t max = stats->max;
    long double mean  = stats_summarize(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(max,   fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}

static void print_hdr_latency(struct hdr_histogram* histogram, const char* description) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0};
    printf("  Latency Distribution (HdrHistogram - %s)\n", description);
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        int64_t n = hdr_value_at_percentile(histogram, p);
        printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
    printf("\n%s\n", "  Detailed Percentile spectrum:");
    hdr_percentiles_print(histogram, stdout, 5, 1000.0, CLASSIC);
}

static void print_stats_latency(stats *stats) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0, 99.9, 99.99, 99.999, 100.0 };
    printf("  Latency Distribution\n");
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        uint64_t n = stats_percentile(stats, p);
        printf("%7.3Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
}

static int scan_rate(char *s, uint64_t *rate) {
    char *slash = strchr(s, '/');
    if (!slash) {
        return scan_metric(s, rate) || *rate == 0 ? -1 : 0;
    }

    uint64_t n;
    *slash = '\0';
    int rc = scan_metric(s, &n);
    *slash = '/';
    if (rc) return -1;

    char *unit = slash + 1, *end;
    double per = isdigit((unsigned char) *unit) ? strtod(unit, &end) : (end = unit, 1.0);
    double scale;
    if      (!strcmp(end, "ms")) scale = 0.001;
    else if (!strcmp(end, "s"))  scale = 1;
    else if (!strcmp(end, "m"))  scale = 60;
    else if (!strcmp(end, "h"))  scale = 3600;
    else return -1;
    if (per <= 0) return -1;

    double r = n / (per * scale);
    if (r < 1) return -1;
    *rate = (uint64_t) (r + 0.5);
    return 0;
}

static int read_file(const char *path, char **data, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 4096, n = 0, r;
    char *buf = zmalloc(cap);
    while ((r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) buf = zrealloc(buf, cap *= 2);
    }
    int err = ferror(f);
    fclose(f);
    if (err) { zfree(buf); return -1; }
    *data = buf;
    *len  = n;
    return 0;
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; s++) {
        unsigned char ch = (unsigned char) *s;
        if (ch == '"' || ch == '\\') fprintf(out, "\\%c", ch);
        else if (ch < 0x20)          fprintf(out, "\\u%04x", ch);
        else                         fputc(ch, out);
    }
    fputc('"', out);
}

static void json_latency(FILE *out, const char *name, struct hdr_histogram *h) {
    static const double pct[]   = { 50, 75, 90, 99, 99.9, 99.99, 99.999 };
    static const char  *label[] = { "p50", "p75", "p90", "p99", "p99_9", "p99_99", "p99_999" };
    fprintf(out, "\"%s\":{\"min\":%"PRId64",\"max\":%"PRId64",\"mean\":%.2f,\"stdev\":%.2f",
            name, h->total_count ? hdr_min(h) : 0, hdr_max(h),
            h->total_count ? hdr_mean(h) : 0.0, h->total_count ? hdr_stddev(h) : 0.0);
    for (size_t i = 0; i < sizeof(pct) / sizeof(pct[0]); i++) {
        fprintf(out, ",\"%s\":%"PRId64, label[i], hdr_value_at_percentile(h, pct[i]));
    }
    fputc('}', out);
}

static void print_json(FILE *out, char *url, uint64_t runtime_us, uint64_t complete,
                       uint64_t bytes, errors *e, struct hdr_histogram *latency,
                       struct hdr_histogram *u_latency, uint64_t *status_codes) {
    double runtime_s = runtime_us / 1000000.0;
    uint64_t total_errors = (uint64_t) e->connect + e->read + e->write + e->timeout + e->status + e->mismatch;

    fprintf(out, "{\"tool\":\"wperfk\",\"version\":");
    json_string(out, VERSION);
    fprintf(out, ",\"url\":");
    json_string(out, url);
    fprintf(out, ",\"mode\":\"%s\",\"rate\":%"PRIu64
                 ",\"threads\":%"PRIu64",\"connections\":%"PRIu64",\"duration_s\":%"PRIu64,
            cfg.rate ? "rate" : "closed", cfg.rate, cfg.threads, cfg.connections,
            cfg.duration_set || !cfg.requests ? cfg.duration : 0);
    fprintf(out, ",\"pipeline\":%"PRIu64",\"warmup_s\":%"PRIu64",\"requests_limit\":%"PRIu64
                 ",\"bailout\":%"PRIu64",\"stopped_by\":\"%s\"",
            cfg.pipeline, cfg.warmup, cfg.requests, cfg.bailout, stop_names[stopped_by]);
    fprintf(out, ",\"runtime_us\":%"PRIu64",\"requests\":%"PRIu64",\"bytes\":%"PRIu64
                 ",\"requests_per_sec\":%.2f,\"bytes_per_sec\":%.2f",
            runtime_us, complete, bytes,
            runtime_s > 0 ? complete / runtime_s : 0.0, runtime_s > 0 ? bytes / runtime_s : 0.0);
    fprintf(out, ",\"errors\":{\"total\":%"PRIu64",\"connect\":%u,\"read\":%u,\"write\":%u"
                 ",\"timeout\":%u,\"status\":%u,\"mismatch\":%u}",
            total_errors, e->connect, e->read, e->write, e->timeout, e->status, e->mismatch);
    fprintf(out, ",\"status_codes\":{");
    for (int s = 0, first = 1; s < 600; s++) {
        if (!status_codes[s]) continue;
        fprintf(out, "%s\"%d\":%"PRIu64, first ? "" : ",", s, status_codes[s]);
        first = 0;
    }
    fprintf(out, "},\"latency_unit\":\"us\",");
    json_latency(out, "latency", latency);
    fputc(',', out);
    json_latency(out, "latency_uncorrected", u_latency);
    fprintf(out, "}\n");
}

// ---------------------------------------------------------------------------
// P5 helpers: expectations, targets, --resolve, progress, histogram files.

static bool contains(const char *hay, size_t len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return true;
    if (!hay || len < n) return false;
    for (size_t i = 0; i + n <= len; i++) {
        if (hay[i] == needle[0] && !memcmp(hay + i, needle, n)) return true;
    }
    return false;
}

// "200,204", "2xx", "2xx,304"
static int parse_expect_status(char *spec, bool *allowed) {
    char *copy = strdup(spec), *save = NULL;
    int rc = 0;
    for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strlen(tok) == 3 && isdigit((unsigned char) tok[0]) &&
            tolower((unsigned char) tok[1]) == 'x' && tolower((unsigned char) tok[2]) == 'x') {
            int base = (tok[0] - '0') * 100;
            if (base < 100 || base > 500) { rc = -1; break; }
            for (int s = base; s < base + 100; s++) allowed[s] = true;
        } else {
            char *end;
            long s = strtol(tok, &end, 10);
            if (*end || s < 100 || s > 599) { rc = -1; break; }
            allowed[s] = true;
        }
    }
    free(copy);
    return rc;
}

// HAR: the first "url" string is log.entries[0].request.url.
static char *har_first_url(FILE *f) {
    size_t len = 0, cap = 1 << 16, n;
    char *buf = zmalloc(cap), *found = NULL;
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 == cap) buf = zrealloc(buf, cap *= 2);
    }
    buf[len] = '\0';
    for (char *p = strstr(buf, "\"url\""); p && !found; p = strstr(p + 5, "\"url\"")) {
        char *q = p + 5;
        while (isspace((unsigned char) *q)) q++;
        if (*q++ != ':') continue;
        while (isspace((unsigned char) *q)) q++;
        if (*q++ != '"') continue;
        char *out = found = zmalloc(strlen(q) + 2);
        for (; *q && *q != '"'; q++) {
            if (*q == '\\' && q[1]) q++;   // "\/" -> "/" (other escapes don't occur in URLs)
            *out++ = *q;
        }
        *out = '\0';
        // Only the origin matters (targets carry their own paths): keep scheme://authority/
        char *auth = strstr(found, "://");
        if (auth) {
            char *end = auth + 3 + strcspn(auth + 3, "/?#");
            end[0] = '/';
            end[1] = '\0';
        }
    }
    zfree(buf);
    return found;
}

// URL of the first target, used when no URL argument is given.
static char *targets_first_url(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "unable to read targets %s: %s\n", path, strerror(errno));
        return NULL;
    }
    int first = fgetc(f);
    while (first != EOF && isspace(first)) first = fgetc(f);
    if (first == '{') {
        char *url = har_first_url(f);
        fclose(f);
        if (!url) fprintf(stderr, "%s: no request URL found in HAR file\n", path);
        return url;
    }
    rewind(f);
    char line[8192], method[32], url[8192 - 32];
    char *found = NULL;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char) *p)) p++;
        if (!*p || *p == '#') continue;
        if (sscanf(p, "%31s %8159s", method, url) == 2) found = strdup(url);
        break;
    }
    fclose(f);
    if (!found) fprintf(stderr, "%s: no target found (expected 'METHOD URL')\n", path);
    return found;
}

// curl-style HOST:PORT:ADDR. Returns 1 on a malformed spec.
static int apply_resolve(char *spec, char *host, char *service, char *schema,
                         char **chost, char **cservice) {
    char *s = strdup(spec);
    char *port = strchr(s, ':');
    char *addr = port ? strchr(port + 1, ':') : NULL;
    if (!port || !addr || !addr[1]) {
        fprintf(stderr, "invalid --resolve %s (expected HOST:PORT:ADDRESS)\n", spec);
        return 1;
    }
    *port++ = '\0';
    *addr++ = '\0';
    size_t alen = strlen(addr);
    if (addr[0] == '[' && addr[alen - 1] == ']') {
        addr[alen - 1] = '\0';
        addr++;
    }
    const char *want = isdigit((unsigned char) service[0]) ? service
                     : (!strncmp(schema, "https", 5) ? "443" : "80");
    if (strcasecmp(s, host) || strcmp(port, want)) {
        fprintf(stderr, "warning: --resolve %s ignored, target is %s:%s\n", spec, host, want);
        return 0;
    }
    *chost    = addr;
    *cservice = port;
    return 0;
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

// Live progress on stderr. Counters are read without locks: approximate by design.
static void progress_loop(thread *threads) {
    uint64_t interval = cfg.progress * 1000000;
    uint64_t last_t = time_us(), next = last_t + interval, last_req = 0;

    while (__sync_add_and_fetch(&threads_running, 0) > 0) {
        sleep_ms(50);
        uint64_t now = time_us();
        if (now < next) continue;
        next += interval;

        uint64_t req = 0, err = 0;
        for (uint64_t i = 0; i < cfg.threads; i++) {
            errors *e = &threads[i].errors;
            req += threads[i].complete;
            err += (uint64_t) e->connect + e->read + e->write + e->timeout + e->status + e->mismatch;
        }
        if (req < last_req) last_req = 0; // counters reset when warmup ended

        bool warming = now < warmup_end;
        double elapsed = warming ? cfg.warmup - (warmup_end - now) / 1e6 : (now - warmup_end) / 1e6;
        fprintf(stderr, "  [%s %7.1fs] %"PRIu64" requests, %.0f req/s, %"PRIu64" errors\n",
                warming ? "warmup" : "run", elapsed, req,
                (req - last_req) / ((now - last_t) / 1e6), err);
        last_req = req;
        last_t   = now;
    }
}

// Histogram file: exact bucket counts plus run totals, mergeable across machines.
static void write_hist_counts(FILE *f, const char *name, struct hdr_histogram *h) {
    fprintf(f, "hist %s %"PRId64" %"PRId64" %"PRId64" %d\n", name,
            h->lowest_trackable_value, h->highest_trackable_value,
            h->significant_figures, h->counts_len);
    for (int i = 0; i < h->counts_len; i++) {
        if (h->counts[i]) fprintf(f, "%d %"PRId64"\n", i, h->counts[i]);
    }
    fprintf(f, "end\n");
}

static int write_hist(const char *path, const char *url, hist_meta *m, uint64_t *codes,
                      struct hdr_histogram *latency, struct hdr_histogram *u_latency) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    errors *e = &m->errors;
    fprintf(f, "wperfk-hist 1\nurl %s\nrate %"PRIu64"\nthreads %"PRIu64"\nconnections %"PRIu64"\n"
               "runtime_us %"PRIu64"\nrequests %"PRIu64"\nbytes %"PRIu64"\n"
               "errors %u %u %u %u %u %u\n",
            url, m->rate, m->threads, m->connections, m->runtime_us, m->requests, m->bytes,
            e->connect, e->read, e->write, e->timeout, e->status, e->mismatch);
    for (int s = 0; s < 600; s++) {
        if (codes[s]) fprintf(f, "status %d %"PRIu64"\n", s, codes[s]);
    }
    write_hist_counts(f, "latency", latency);
    write_hist_counts(f, "u_latency", u_latency);
    return fclose(f);
}

static int read_hist_counts(FILE *f, const char *path, char *line, struct hdr_histogram *h) {
    char name[32];
    int64_t lo, hi, sig;
    int len;
    if (sscanf(line, "hist %31s %"SCNd64" %"SCNd64" %"SCNd64" %d", name, &lo, &hi, &sig, &len) != 5 ||
        lo != h->lowest_trackable_value || hi != h->highest_trackable_value ||
        sig != h->significant_figures || len != h->counts_len) {
        fprintf(stderr, "%s: incompatible histogram (%s)\n", path, name);
        return -1;
    }
    char buf[128];
    while (fgets(buf, sizeof(buf), f) && strncmp(buf, "end", 3)) {
        int idx;
        int64_t count;
        if (sscanf(buf, "%d %"SCNd64, &idx, &count) != 2 || idx < 0 || idx >= len || count < 0) {
            fprintf(stderr, "%s: bad histogram line: %s", path, buf);
            return -1;
        }
        h->counts[idx]  += count;
        h->total_count  += count;
    }
    return 0;
}

// wperfk merge [--json] <files...>
static int merge_main(int argc, char **argv) {
    bool json = false;
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "--json")) { json = true; first = 2; }
    if (first >= argc) {
        fprintf(stderr, "Usage: wperfk merge [--json] <hist files from --hist-out...>\n");
        return 1;
    }

    struct hdr_histogram *latency, *u_latency;
    hdr_init(1, MAX_LATENCY, 3, &latency);
    hdr_init(1, MAX_LATENCY, 3, &u_latency);
    hist_meta total = { 0 };
    uint64_t codes[600] = { 0 };
    char url[4096] = "", line[4096];

    for (int i = first; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "unable to read %s: %s\n", argv[i], strerror(errno));
            return 1;
        }
        if (!fgets(line, sizeof(line), f) || strncmp(line, "wperfk-hist 1", 13)) {
            fprintf(stderr, "%s: not a wperfk --hist-out file\n", argv[i]);
            return 1;
        }
        hist_meta m = { 0 };
        errors *e = &m.errors;
        char u[4096];
        int code, rc = 0;
        uint64_t count;
        while (!rc && fgets(line, sizeof(line), f)) {
            if      (sscanf(line, "url %4095s", u) == 1) {
                if (!url[0]) strcpy(url, u);
                else if (strcmp(url, u)) strcpy(url, "(multiple)");
            }
            else if (sscanf(line, "rate %"SCNu64, &m.rate) == 1) ;
            else if (sscanf(line, "threads %"SCNu64, &m.threads) == 1) ;
            else if (sscanf(line, "connections %"SCNu64, &m.connections) == 1) ;
            else if (sscanf(line, "runtime_us %"SCNu64, &m.runtime_us) == 1) ;
            else if (sscanf(line, "requests %"SCNu64, &m.requests) == 1) ;
            else if (sscanf(line, "bytes %"SCNu64, &m.bytes) == 1) ;
            else if (sscanf(line, "errors %u %u %u %u %u %u", &e->connect, &e->read, &e->write,
                            &e->timeout, &e->status, &e->mismatch) == 6) ;
            else if (sscanf(line, "status %d %"SCNu64, &code, &count) == 2 && code > 0 && code < 600)
                codes[code] += count;
            else if (!strncmp(line, "hist latency ", 13))
                rc = read_hist_counts(f, argv[i], line, latency);
            else if (!strncmp(line, "hist u_latency ", 15))
                rc = read_hist_counts(f, argv[i], line, u_latency);
        }
        fclose(f);
        if (rc) return 1;

        total.rate        += m.rate;
        total.threads     += m.threads;
        total.connections += m.connections;
        total.requests    += m.requests;
        total.bytes       += m.bytes;
        total.runtime_us   = MAX(total.runtime_us, m.runtime_us); // runs are concurrent
        total.errors.connect  += e->connect;
        total.errors.read     += e->read;
        total.errors.write    += e->write;
        total.errors.timeout  += e->timeout;
        total.errors.status   += e->status;
        total.errors.mismatch += e->mismatch;
    }

    FILE *out = stdout;
    if (json) {
        cfg.rate = total.rate;
        cfg.threads = total.threads;
        cfg.connections = total.connections;
        cfg.duration = total.runtime_us / 1000000;
        cfg.duration_set = true;
        stopped_by = STOP_MERGED;
        print_json(out, url, total.runtime_us, total.requests, total.bytes, &total.errors,
                   latency, u_latency, codes);
        return 0;
    }

    double runtime_s = total.runtime_us / 1e6;
    printf("Merged %d results for %s\n", argc - first, url);
    printf("  %"PRIu64" threads and %"PRIu64" connections in total\n", total.threads, total.connections);
    print_hdr_latency(latency, "Recorded Latency");
    printf("----------------------------------------------------------\n");
    printf("  %"PRIu64" requests in %s, %sB read\n", total.requests,
           format_time_us(total.runtime_us), format_binary(total.bytes));
    errors *e = &total.errors;
    if (e->connect || e->read || e->write || e->timeout) {
        printf("  Socket errors: connect %u, read %u, write %u, timeout %u\n",
               e->connect, e->read, e->write, e->timeout);
    }
    if (e->status)   printf("  Non-2xx or 3xx responses: %u\n", e->status);
    if (e->mismatch) printf("  Unexpected responses (--expect-*): %u\n", e->mismatch);
    printf("Requests/sec: %9.2f\n", runtime_s > 0 ? total.requests / runtime_s : 0.0);
    printf("Transfer/sec: %10sB\n", format_binary(runtime_s > 0 ? total.bytes / runtime_s : 0));
    return 0;
}

// --prometheus: live counters (unlocked reads: approximate while running;
// counters restart when --warmup ends).
static void prom_label(char **out, const char *s) {
    for (; *s; s++) {
        if (*s == '\\' || *s == '"') aprintf(out, "\\%c", *s);
        else if (*s == '\n') aprintf(out, "\\n");
        else aprintf(out, "%c", *s);
    }
}

// hdr_add() iterates until the source's total_count, which a worker thread
// may move past the counts it has already scanned: it can then walk off the
// end of the array. Copy live histograms with a loop bounded by counts_len.
static void hist_add_live(struct hdr_histogram *dst, struct hdr_histogram *src) {
    for (int i = 0; i < dst->counts_len && i < src->counts_len; i++) {
        int64_t n = src->counts[i];
        dst->counts[i]   += n;
        dst->total_count += n;
    }
}

static char *render_metrics(size_t *len) {
    char *out = NULL;
    uint64_t req = 0, bytes = 0, codes[600] = { 0 };
    errors e = { 0 };
    struct hdr_histogram *h;
    hdr_init(1, MAX_LATENCY, 3, &h);

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &all_threads[i];
        if (!t->latency_histogram) continue;   // not started yet
        req   += t->complete;
        bytes += t->bytes;
        e.connect  += t->errors.connect;
        e.read     += t->errors.read;
        e.write    += t->errors.write;
        e.timeout  += t->errors.timeout;
        e.status   += t->errors.status;
        e.mismatch += t->errors.mismatch;
        for (int s = 0; s < 600; s++) codes[s] += t->status_codes[s];
        hist_add_live(h, t->latency_histogram);
    }

    uint64_t now = time_us();
    aprintf(&out, "# HELP wperfk_info Run configuration.\n# TYPE wperfk_info gauge\n");
    aprintf(&out, "wperfk_info{version=\"%s\",mode=\"%s\",url=\"", VERSION, cfg.rate ? "rate" : "closed");
    prom_label(&out, run_url ? run_url : "");
    aprintf(&out, "\"} 1\n");
    aprintf(&out, "# HELP wperfk_running 1 while the load test runs.\n# TYPE wperfk_running gauge\n"
                  "wperfk_running %d\n", __sync_add_and_fetch(&threads_running, 0) > 0);
    aprintf(&out, "# HELP wperfk_elapsed_seconds Measured time so far (0 during warmup).\n"
                  "# TYPE wperfk_elapsed_seconds gauge\nwperfk_elapsed_seconds %.3f\n",
            now > warmup_end ? (now - warmup_end) / 1e6 : 0.0);
    aprintf(&out, "# HELP wperfk_connections Configured connections.\n# TYPE wperfk_connections gauge\n"
                  "wperfk_connections %"PRIu64"\n", cfg.connections);
    aprintf(&out, "# HELP wperfk_target_rate Configured -R rate in requests/s (0: closed loop).\n"
                  "# TYPE wperfk_target_rate gauge\nwperfk_target_rate %"PRIu64"\n", cfg.rate);
    aprintf(&out, "# HELP wperfk_requests_total Completed responses.\n# TYPE wperfk_requests_total counter\n"
                  "wperfk_requests_total %"PRIu64"\n", req);
    aprintf(&out, "# HELP wperfk_bytes_total Bytes read.\n# TYPE wperfk_bytes_total counter\n"
                  "wperfk_bytes_total %"PRIu64"\n", bytes);
    aprintf(&out, "# HELP wperfk_errors_total Errors by type.\n# TYPE wperfk_errors_total counter\n");
    const char *names[] = { "connect", "read", "write", "timeout", "status", "mismatch" };
    uint32_t vals[] = { e.connect, e.read, e.write, e.timeout, e.status, e.mismatch };
    for (int i = 0; i < 6; i++) aprintf(&out, "wperfk_errors_total{type=\"%s\"} %u\n", names[i], vals[i]);
    aprintf(&out, "# HELP wperfk_responses_total Responses by status code.\n# TYPE wperfk_responses_total counter\n");
    for (int s = 0; s < 600; s++) {
        if (codes[s]) aprintf(&out, "wperfk_responses_total{code=\"%d\"} %"PRIu64"\n", s, codes[s]);
    }
    aprintf(&out, "# HELP wperfk_latency_seconds Response latency (coordinated-omission corrected with -R).\n"
                  "# TYPE wperfk_latency_seconds summary\n");
    const double q[] = { 0.5, 0.9, 0.99, 0.999 };
    for (int i = 0; i < 4; i++) {
        double v = h->total_count ? hdr_value_at_percentile(h, q[i] * 100) / 1e6 : 0;
        aprintf(&out, "wperfk_latency_seconds{quantile=\"%g\"} %.6f\n", q[i], v);
    }
    aprintf(&out, "wperfk_latency_seconds_sum %.6f\nwperfk_latency_seconds_count %"PRId64"\n",
            h->total_count ? hdr_mean(h) * h->total_count / 1e6 : 0.0, h->total_count);

    free(h);
    *len = out ? strlen(out) : 0;
    return out;
}
