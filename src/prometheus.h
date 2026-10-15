#ifndef PROMETHEUS_H
#define PROMETHEUS_H

#include <stddef.h>

// Returns a malloc'd exposition-format body; the exporter frees it.
typedef char *(*prom_render_fn)(size_t *len);

// Serve metrics on [HOST:]PORT (default host 127.0.0.1) from a background
// thread. Returns 0 on success, -2 if the address doesn't resolve, -1 on
// other errors (see errno).
int prom_start(const char *spec, prom_render_fn render);

#endif /* PROMETHEUS_H */
