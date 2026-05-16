#include "internal.h"

#include <stdarg.h>
#include <stdio.h>

int srapi = 0;
static char srapi_error[256] = "ok";

const char *srapi_last_error(void) {
    return srapi_error;
}

void srapi_set_error(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(srapi_error, sizeof(srapi_error), fmt, args);
    va_end(args);

    srapi_debugf("error: %s", srapi_error);
}

void srapi_debugf(const char *fmt, ...) {
    va_list args;

    if (!srapi) {
        return;
    }

    fprintf(stderr, "[srapi] ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
