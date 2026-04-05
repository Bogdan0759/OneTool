#include "../down.h"

#include <stdio.h>

int down_check_robots(const down_request_t *req) {
    int allowed = 1;

    if (req->ignore_robots) {
        return 0;
    }

    if (net_robots_is_allowed(
            req->host,
            req->port,
            req->use_tls,
            req->timeout_sec,
            req->path,
            "onetool-down",
            req->verbose,
            &allowed
        ) != 0) {
        if (req->verbose) {
            fprintf(stderr, "down: robots check skipped (fetch failed)\n");
        }
        return 0;
    }

    if (!allowed) {
        fprintf(stderr, "down: blocked by robots.txt %s\n", req->path);
        return 1;
    }

    return 0;
}
