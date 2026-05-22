#include "internal.h"

#include <stdint.h>
#include <time.h>

uint64_t srapi_time_now_us(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

void srapi_clock_init(srapi_clock_t *clock) {
    uint64_t now;

    if (clock == NULL) {
        return;
    }
    now = srapi_time_now_us();
    clock->start_us = now;
    clock->last_us = now;
}

float srapi_clock_tick(srapi_clock_t *clock) {
    uint64_t now;
    uint64_t delta_us;

    if (clock == NULL) {
        return 0.0f;
    }
    now = srapi_time_now_us();
    if (clock->last_us == 0) {
        clock->start_us = now;
        clock->last_us = now;
        return 0.0f;
    }
    delta_us = now - clock->last_us;
    clock->last_us = now;
    return (float)delta_us / 1000000.0f;
}

float srapi_clock_elapsed(const srapi_clock_t *clock) {
    uint64_t now;
    uint64_t elapsed_us;

    if (clock == NULL || clock->start_us == 0) {
        return 0.0f;
    }
    now = srapi_time_now_us();
    elapsed_us = now - clock->start_us;
    return (float)elapsed_us / 1000000.0f;
}
