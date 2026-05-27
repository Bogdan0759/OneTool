#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *js_strdup(const char *s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (out == NULL) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

char *js_strndup(const char *s, size_t len) {
    char *out = (char *)malloc(len + 1);
    if (out == NULL) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

char *js_format_error(const char *fmt, const char *a, const char *b) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf), fmt,
                     a != NULL ? a : "", b != NULL ? b : "");
    if (n < 0) return js_strdup("format error");
    if ((size_t)n < sizeof(buf)) return js_strdup(buf);
    char *out = (char *)malloc((size_t)n + 1);
    if (out == NULL) return NULL;
    snprintf(out, (size_t)n + 1, fmt, a != NULL ? a : "", b != NULL ? b : "");
    return out;
}
