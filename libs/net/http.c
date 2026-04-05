#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    net_conn_t *conn;
    const unsigned char *prefill;
    size_t prefill_len;
    size_t prefill_pos;
} stream_t;

static int send_all(net_conn_t *conn, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = net_conn_write(conn, p + off, len - off);
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

static ssize_t stream_read(stream_t *s, unsigned char *dst, size_t cap) {
    if (s->prefill_pos < s->prefill_len) {
        size_t n = s->prefill_len - s->prefill_pos;
        if (n > cap) {
            n = cap;
        }
        memcpy(dst, s->prefill + s->prefill_pos, n);
        s->prefill_pos += n;
        return (ssize_t)n;
    }
    return net_conn_read(s->conn, dst, cap);
}

static int stream_read_exact(stream_t *s, unsigned char *dst, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = stream_read(s, dst + off, len - off);
        if (n <= 0) {
            return 1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int stream_read_line(stream_t *s, char *line, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        unsigned char c;
        if (stream_read_exact(s, &c, 1) != 0) {
            return 1;
        }
        line[i++] = (char)c;
        if (c == '\n') {
            line[i] = '\0';
            return 0;
        }
    }
    return 1;
}

static int write_all(FILE *out, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t n = fwrite(buf + off, 1, len - off, out);
        if (n == 0) {
            return 1;
        }
        off += n;
    }
    return 0;
}

static int read_chunked(stream_t *s, FILE *out, long long *written) {
    char line[4096];
    unsigned char *tmp = NULL;
    for (;;) {
        char *end = NULL;
        unsigned long long chunk_len;

        if (stream_read_line(s, line, sizeof(line)) != 0) {
            free(tmp);
            return 1;
        }
        chunk_len = strtoull(line, &end, 16);
        if (end == line) {
            free(tmp);
            return 1;
        }
        if (chunk_len == 0) {
            do {
                if (stream_read_line(s, line, sizeof(line)) != 0) {
                    free(tmp);
                    return 1;
                }
            } while (strcmp(line, "\r\n") != 0 && strcmp(line, "\n") != 0);
            break;
        }

        tmp = (unsigned char *)realloc(tmp, (size_t)chunk_len);
        if (tmp == NULL) {
            return 1;
        }
        if (stream_read_exact(s, tmp, (size_t)chunk_len) != 0) {
            free(tmp);
            return 1;
        }
        if (write_all(out, tmp, (size_t)chunk_len) != 0) {
            free(tmp);
            return 1;
        }
        *written += (long long)chunk_len;
        if (stream_read_line(s, line, sizeof(line)) != 0) {
            free(tmp);
            return 1;
        }
    }
    free(tmp);
    return 0;
}

int net_http_send_request(
    net_conn_t *conn,
    const char *method,
    const char *path,
    const char *host,
    const char *headers[],
    int header_count,
    const char *body,
    const char *user_agent
) {
    char head[16384];
    int n;
    size_t used;
    size_t body_len = body ? strlen(body) : 0;

    n = snprintf(
        head,
        sizeof(head),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n",
        method, path, host, user_agent ? user_agent : "onetool-net/0.1"
    );
    if (n < 0 || (size_t)n >= sizeof(head)) {
        return 1;
    }
    used = (size_t)n;

    for (int i = 0; i < header_count; i++) {
        n = snprintf(head + used, sizeof(head) - used, "%s\r\n", headers[i]);
        if (n < 0 || (size_t)n >= sizeof(head) - used) {
            return 1;
        }
        used += (size_t)n;
    }

    if (body_len > 0) {
        n = snprintf(head + used, sizeof(head) - used, "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)n >= sizeof(head) - used) {
            return 1;
        }
        used += (size_t)n;
    }

    n = snprintf(head + used, sizeof(head) - used, "\r\n");
    if (n < 0 || (size_t)n >= sizeof(head) - used) {
        return 1;
    }
    used += (size_t)n;

    if (send_all(conn, head, used) != 0) {
        return 1;
    }
    if (body_len > 0 && send_all(conn, body, body_len) != 0) {
        return 1;
    }
    return 0;
}

int net_http_read_response(net_conn_t *conn, FILE *out, int verbose, net_http_response_t *resp) {
    unsigned char buf[8192];
    unsigned char *head = NULL;
    size_t head_len = 0;
    size_t head_cap = 0;
    const unsigned char *body_start = NULL;
    size_t body_len = 0;
    char *headers_dup = NULL;
    char *line;
    int content_length_found = 0;
    int chunked = 0;
    long long content_length = -1;

    memset(resp, 0, sizeof(*resp));
    resp->content_length = -1;
    resp->body_bytes = 0;

    for (;;) {
        size_t old_len = head_len;
        unsigned char *p;
        ssize_t n = net_conn_read(conn, buf, sizeof(buf));
        if (n <= 0) {
            free(head);
            return 1;
        }

        if (head_len + (size_t)n + 1 > head_cap) {
            size_t new_cap = head_cap == 0 ? 16384 : head_cap * 2;
            while (new_cap < head_len + (size_t)n + 1) {
                new_cap *= 2;
            }
            p = (unsigned char *)realloc(head, new_cap);
            if (p == NULL) {
                free(head);
                return 1;
            }
            head = p;
            head_cap = new_cap;
        }

        memcpy(head + head_len, buf, (size_t)n);
        head_len += (size_t)n;
        head[head_len] = '\0';

        if (head_len >= 4) {
            for (size_t i = old_len > 3 ? old_len - 3 : 0; i + 3 < head_len; i++) {
                if (head[i] == '\r' && head[i + 1] == '\n' && head[i + 2] == '\r' && head[i + 3] == '\n') {
                    body_start = head + i + 4;
                    body_len = head_len - (i + 4);
                    goto headers_ready;
                }
            }
        }
    }

headers_ready:
    headers_dup = strdup((const char *)head);
    if (headers_dup == NULL) {
        free(head);
        return 1;
    }

    line = strtok(headers_dup, "\r\n");
    if (line == NULL || sscanf(line, "HTTP/%*s %d", &resp->status_code) != 1) {
        free(headers_dup);
        free(head);
        return 1;
    }

    while ((line = strtok(NULL, "\r\n")) != NULL) {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *v = line + 15;
            while (*v == ' ' || *v == '\t') v++;
            content_length = atoll(v);
            content_length_found = 1;
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            char *v = line + 18;
            while (*v == ' ' || *v == '\t') v++;
            if (strstr(v, "chunked") != NULL) {
                chunked = 1;
            }
        }
    }

    resp->chunked = chunked;
    resp->content_length = content_length_found ? content_length : -1;

    if (verbose) {
        fprintf(stderr, "net: HTTP %d\n", resp->status_code);
    }

    stream_t s = { .conn = conn, .prefill = body_start, .prefill_len = body_len, .prefill_pos = 0 };

    if (chunked) {
        if (read_chunked(&s, out, &resp->body_bytes) != 0) {
            free(headers_dup);
            free(head);
            return 1;
        }
    } else if (content_length_found) {
        long long remain = content_length;
        if ((long long)body_len > remain) {
            body_len = (size_t)remain;
        }
        if (body_len > 0) {
            if (write_all(out, body_start, body_len) != 0) {
                free(headers_dup);
                free(head);
                return 1;
            }
            remain -= (long long)body_len;
        }
        while (remain > 0) {
            unsigned char io_buf[8192];
            size_t want = remain > (long long)sizeof(io_buf) ? sizeof(io_buf) : (size_t)remain;
            ssize_t n = stream_read(&s, io_buf, want);
            if (n <= 0 || write_all(out, io_buf, (size_t)n) != 0) {
                free(headers_dup);
                free(head);
                return 1;
            }
            remain -= (long long)n;
        }
        resp->body_bytes = content_length;
    } else {
        if (body_len > 0 && write_all(out, body_start, body_len) != 0) {
            free(headers_dup);
            free(head);
            return 1;
        }
        for (;;) {
            unsigned char io_buf[8192];
            ssize_t n = net_conn_read(conn, io_buf, sizeof(io_buf));
            if (n <= 0) {
                break;
            }
            if (write_all(out, io_buf, (size_t)n) != 0) {
                free(headers_dup);
                free(head);
                return 1;
            }
            resp->body_bytes += (long long)n;
        }
        resp->body_bytes += (long long)body_len;
    }

    fflush(out);
    free(headers_dup);
    free(head);
    return 0;
}
