#include "../down.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int down_send_all(down_conn_t *conn, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = down_conn_write(conn, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }
        if (n == 0) {
            return 1;
        }
        off += (size_t)n;
    }

    return 0;
}

int down_send_request(down_conn_t *conn, const down_request_t *req) {
    char head[16384];
    int n;
    size_t body_len = req->body ? strlen(req->body) : 0;

    n = snprintf(
        head,
        sizeof(head),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: onetool-down/0.1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n",
        req->method,
        req->path,
        req->host
    );
    if (n < 0 || (size_t)n >= sizeof(head)) {
        fprintf(stderr, "down: request header is too large\n");
        return 1;
    }

    size_t used = (size_t)n;

    for (int i = 0; i < req->header_count; i++) {
        n = snprintf(head + used, sizeof(head) - used, "%s\r\n", req->headers[i]);
        if (n < 0 || (size_t)n >= sizeof(head) - used) {
            fprintf(stderr, "down: request header is too large\n");
            return 1;
        }
        used += (size_t)n;
    }

    if (body_len > 0) {
        n = snprintf(head + used, sizeof(head) - used, "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)n >= sizeof(head) - used) {
            fprintf(stderr, "down: request header is too large\n");
            return 1;
        }
        used += (size_t)n;
    }

    n = snprintf(head + used, sizeof(head) - used, "\r\n");
    if (n < 0 || (size_t)n >= sizeof(head) - used) {
        fprintf(stderr, "down: request header is too large\n");
        return 1;
    }
    used += (size_t)n;

    if (down_send_all(conn, head, used) != 0) {
        fprintf(stderr, "down: send request headers failed: %s\n", strerror(errno));
        return 1;
    }

    if (body_len > 0 && down_send_all(conn, req->body, body_len) != 0) {
        fprintf(stderr, "down: send request body failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

int down_run(const down_request_t *req, FILE *out) {
    down_conn_t conn;
    down_response_t resp;

    if (down_check_robots(req) != 0) {
        return 1;
    }

    memset(&resp, 0, sizeof(resp));
    if (down_socket_connect(req, &conn) != 0) {
        return 1;
    }

    if (req->verbose) {
        fprintf(stderr, "down: connected to %s:%s (%s)\n", req->host, req->port, req->use_tls ? "https" : "http");
    }

    if (down_send_request(&conn, req) != 0) {
        down_socket_close(&conn);
        return 1;
    }

    if (down_read_response(&conn, out, req->verbose, &resp) != 0) {
        down_socket_close(&conn);
        return 1;
    }

    if (req->verbose) {
        fprintf(stderr, "down: completed with HTTP %d\n", resp.status_code);
    }

    down_socket_close(&conn);
    return 0;
}
