#ifndef URL_H
#define URL_H

// Absolute-URL parser, API-compatible with http_parser's http_parser_parse_url
// (wperfk's HTTP parsing moved to llhttp, which has no URL parser).
// Differences from http_parser: '_' is allowed in host names, and a fragment
// may follow the host directly ("http://h#f").

#include <stddef.h>
#include <stdint.h>

enum http_parser_url_fields {
    UF_SCHEMA   = 0,
    UF_HOST     = 1,
    UF_PORT     = 2,
    UF_PATH     = 3,
    UF_QUERY    = 4,
    UF_FRAGMENT = 5,
    UF_USERINFO = 6,
    UF_MAX      = 7
};

struct http_parser_url {
    uint16_t field_set;   // bitmask of (1 << UF_*)
    uint16_t port;        // converted UF_PORT
    struct {
        uint16_t off;
        uint16_t len;
    } field_data[UF_MAX];
};

// Returns 0 on success, 1 on error. `is_connect` must be 0.
int http_parser_parse_url(const char *buf, size_t buflen, int is_connect,
                          struct http_parser_url *u);

#endif /* URL_H */
