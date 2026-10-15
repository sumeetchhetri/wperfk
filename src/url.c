// URL parser: scheme://[userinfo@]host[:port][/path][?query][#fragment], or /path[?query][#fragment]
// See url.h for how it relates to http_parser_parse_url.

#include <ctype.h>
#include <string.h>
#include "url.h"

static void set(struct http_parser_url *u, int field, size_t off, size_t len) {
    u->field_set |= 1 << field;
    u->field_data[field].off = (uint16_t) off;
    u->field_data[field].len = (uint16_t) len;
}

static int is_host_char(unsigned char c)     { return isalnum(c) || c == '.' || c == '-' || c == '_'; }
static int is_ipv6_char(unsigned char c)     { return isxdigit(c) || c == ':' || c == '.'; }
static int is_url_char(unsigned char c)      { return c > 0x20 && c < 0x7f; }
static int is_userinfo_char(unsigned char c) {
    return isalnum(c) || strchr("-_.!~*'()%;:&=+$,", c) != NULL;
}

int http_parser_parse_url(const char *buf, size_t len, int is_connect, struct http_parser_url *u) {
    memset(u, 0, sizeof(*u));
    if (is_connect || len > UINT16_MAX) return 1;
    if (len == 0) return 0;

    size_t i = 0;

    // origin-form ("/path?query"), as http_parser accepts
    if (buf[0] == '/') goto path;

    // scheme
    while (i < len && isalpha((unsigned char) buf[i])) i++;
    if (i == 0 || len - i < 3 || memcmp(buf + i, "://", 3)) return 1;
    set(u, UF_SCHEMA, 0, i);
    i += 3;

    // authority ends at '/', '?', '#' or the end
    size_t auth = i, auth_end = i;
    while (auth_end < len && buf[auth_end] != '/' && buf[auth_end] != '?' && buf[auth_end] != '#') auth_end++;

    // userinfo@ (the last '@' in the authority)
    size_t at = auth_end;
    for (size_t k = auth; k < auth_end; k++) if (buf[k] == '@') at = k;
    if (at != auth_end) {
        for (size_t k = auth; k < at; k++) if (!is_userinfo_char((unsigned char) buf[k])) return 1;
        if (at > auth) set(u, UF_USERINFO, auth, at - auth);
        auth = at + 1;
    }

    // host: [ipv6] or name
    size_t h = auth, host_end;
    if (h < auth_end && buf[h] == '[') {
        size_t close = h + 1;
        while (close < auth_end && buf[close] != ']') {
            if (!is_ipv6_char((unsigned char) buf[close])) return 1;
            close++;
        }
        if (close >= auth_end || close == h + 1) return 1;
        set(u, UF_HOST, h + 1, close - h - 1);
        host_end = close + 1;
    } else {
        host_end = h;
        while (host_end < auth_end && buf[host_end] != ':') {
            if (!is_host_char((unsigned char) buf[host_end])) return 1;
            host_end++;
        }
        if (host_end == h) return 1;
        set(u, UF_HOST, h, host_end - h);
    }

    // :port
    if (host_end < auth_end) {
        if (buf[host_end] != ':') return 1;
        size_t p = host_end + 1;
        if (p == auth_end) return 1;
        unsigned long port = 0;
        for (size_t k = p; k < auth_end; k++) {
            if (!isdigit((unsigned char) buf[k])) return 1;
            port = port * 10 + (buf[k] - '0');
            if (port > 65535) return 1;   // value, not digit count: "008080" is 8080
        }
        set(u, UF_PORT, p, auth_end - p);
        u->port = (uint16_t) port;
    }

    i = auth_end;

  path:;
    // path, ?query, #fragment (empty query/fragment are left unset)
    size_t start = i;
    while (i < len && buf[i] != '?' && buf[i] != '#') {
        if (!is_url_char((unsigned char) buf[i])) return 1;
        i++;
    }
    if (i > start) set(u, UF_PATH, start, i - start);

    if (i < len && buf[i] == '?') {
        start = ++i;
        while (i < len && buf[i] != '#') {
            if (!is_url_char((unsigned char) buf[i])) return 1;
            i++;
        }
        if (i > start) set(u, UF_QUERY, start, i - start);
    }

    if (i < len && buf[i] == '#') {
        while (i < len && buf[i] == '#') i++;   // http_parser skips repeated '#'
        start = i;
        while (i < len) {
            if (!is_url_char((unsigned char) buf[i])) return 1;
            i++;
        }
        if (i > start) set(u, UF_FRAGMENT, start, i - start);
    }
    return 0;
}
