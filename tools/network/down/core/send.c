#include "../down.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int down_run(const down_request_t *req, FILE *out) {
    net_conn_t conn;
    net_http_response_t resp;
    const char *ua = "onetool-down/0.2";
    int status_class;

    if (!req->quiet) {
        fprintf(stderr, "down: %s %s://%s:%s%s\n",
                req->method,
                req->use_tls ? "https" : "http",
                req->host,
                req->port,
                req->path);
    }

    if (down_check_robots(req) != 0) {
        return 1;
    }

    memset(&resp, 0, sizeof(resp));
    if (net_conn_open(&conn, req->host, req->port, req->timeout_sec, req->use_tls, req->host, 1) != 0) {
        return 1;
    }

    if (req->verbose && !req->quiet) {
        fprintf(stderr, "down: connected to %s:%s (%s)\n", req->host, req->port, req->use_tls ? "https" : "http");
    }

    if (net_http_send_request(
            &conn,
            req->method,
            req->path,
            req->host,
            (const char **)req->headers,
            req->header_count,
            req->body,
            ua
        ) != 0) {
        net_conn_close(&conn);
        return 1;
    }

    if (net_http_read_response(&conn, out, req->verbose, &resp) != 0) {
        net_conn_close(&conn);
        return 1;
    }

    status_class = resp.status_code / 100;
    if (!req->quiet) {
        if (status_class == 2) {
            fprintf(stderr, "down: HTTP %d OK", resp.status_code);
        } else {
            fprintf(stderr, "down: HTTP %d", resp.status_code);
        }
        if (resp.body_bytes >= 0) {
            fprintf(stderr, ", %lld bytes", resp.body_bytes);
        }
        if (req->output_path != NULL) {
            fprintf(stderr, ", saved to %s", req->output_path);
        }
        fprintf(stderr, "\n");
    } else if (req->verbose) {
        fprintf(stderr, "down: completed with HTTP %d\n", resp.status_code);
    }

    net_conn_close(&conn);
    return 0;
}
