#ifndef ONETOOL_TOOLS_GUI_BROWSER_NET_FETCH_H
#define ONETOOL_TOOLS_GUI_BROWSER_NET_FETCH_H

#include <stddef.h>

/*
 * Fetch the given URL into a freshly malloc'd buffer.
 *
 *  url       - http://, https://, or file:// URL.
 *  out_body  - on success receives a malloc'd, NUL-terminated buffer.
 *  out_len   - on success receives the body length in bytes (without NUL).
 *  out_final - on success receives a malloc'd string with the final URL
 *              (after redirects). Caller may pass NULL.
 *  err       - on failure receives a static error string; NULL on success.
 *
 * Returns 0 on success, non-zero on failure (out_body untouched).
 */
int browser_fetch_url(const char *url,
                      char **out_body,
                      size_t *out_len,
                      char **out_final,
                      const char **err);

#endif
