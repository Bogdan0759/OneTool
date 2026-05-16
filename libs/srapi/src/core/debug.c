#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int srapi = 0;
static char srapi_error[256] = "ok";
static uint32_t srapi_backend_checks[] = {
    [SRAPI_BACKEND_AUTO] = SRAPI_BACKEND_CHECK_ALL,
    [SRAPI_BACKEND_CPU] = SRAPI_BACKEND_CHECK_ALL,
    [SRAPI_BACKEND_GPU] = SRAPI_BACKEND_CHECK_ALL,
    [SRAPI_BACKEND_FBDEV] = SRAPI_BACKEND_CHECK_ALL,
};

static int backend_index(srapi_backend_t backend, size_t *out) {
    if (backend == SRAPI_BACKEND_AUTO) {
        backend = SRAPI_BACKEND_CPU;
    }
    if (backend != SRAPI_BACKEND_CPU &&
        backend != SRAPI_BACKEND_GPU &&
        backend != SRAPI_BACKEND_FBDEV) {
        return 0;
    }

    *out = (size_t)backend;
    return 1;
}

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

srapi_result_t srapi_get_backend_config(srapi_backend_t backend, srapi_backend_config_t *out) {
    size_t index;

    if (out == NULL || !backend_index(backend, &index)) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->backend = (srapi_backend_t)index;
    out->checks = srapi_backend_checks[index];
    return SRAPI_OK;
}

srapi_result_t srapi_set_backend_config(const srapi_backend_config_t *config) {
    size_t index;

    if (config == NULL || !backend_index(config->backend, &index)) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((config->checks & ~SRAPI_BACKEND_CHECK_ALL) != 0) {
        srapi_set_error("backend config: unknown check flags 0x%x",
                        config->checks & ~SRAPI_BACKEND_CHECK_ALL);
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_backend_checks[index] = config->checks;
    srapi_debugf("backend config set backend=%s checks=0x%x",
                 srapi_backend_name((srapi_backend_t)index), config->checks);
    return SRAPI_OK;
}

void srapi_reset_backend_config(srapi_backend_t backend) {
    size_t index;

    if (!backend_index(backend, &index)) {
        return;
    }

    srapi_backend_checks[index] = SRAPI_BACKEND_CHECK_ALL;
    srapi_debugf("backend config reset backend=%s",
                 srapi_backend_name((srapi_backend_t)index));
}

uint32_t srapi_backend_enabled_checks(srapi_backend_t backend) {
    size_t index;

    if (!backend_index(backend, &index)) {
        return 0;
    }
    return srapi_backend_checks[index];
}

int srapi_backend_check_enabled(srapi_backend_t backend, uint32_t check) {
    return (srapi_backend_enabled_checks(backend) & check) != 0;
}
