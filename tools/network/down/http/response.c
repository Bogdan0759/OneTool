#include "../down.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    down_conn_t *conn;
    const unsigned char *prefill;
    size_t prefill_len;
    size_t prefill_pos;
} down_stream_t;

static ssize_t stream_read(down_stream_t *s, unsigned char *dst, size_t cap) {
    if (s->prefill_pos < s->prefill_len) {
        size_t n = s->prefill_len - s->prefill_pos;
        if (n > cap) {
            n = cap;
        }
        memcpy(dst, s->prefill + s->prefill_pos, n);
        s->prefill_pos += n;
        return (ssize_t)n;
    }
    for (;;) {
        ssize_t n = down_conn_read(s->conn, dst, cap);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

static int stream_read_exact(down_stream_t *s, unsigned char *dst, size_t len) {
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

static int stream_read_line(down_stream_t *s, char *line, size_t cap) {
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
    line[cap - 1] = '\0';
    return 1;
}

static int write_all_file(FILE *out, const unsigned char *buf, size_t len) {
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

static int decode_chunked(down_stream_t *s, FILE *out) {
    char line[4096];
    unsigned char *tmp = NULL;

    for (;;) {
        char *end = NULL;
        unsigned long long chunk_len;

        if (stream_read_line(s, line, sizeof(line)) != 0) {
            fprintf(stderr, "down: failed to read chunk size\n");
            free(tmp);
            return 1;
        }

        chunk_len = strtoull(line, &end, 16);
        if (end == line) {
            fprintf(stderr, "down: invalid chunk size\n");
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

        if (chunk_len > (1024ULL * 1024ULL * 1024ULL)) {
            fprintf(stderr, "down: chunk too large\n");
            free(tmp);
            return 1;
        }

        tmp = (unsigned char *)realloc(tmp, (size_t)chunk_len);
        if (tmp == NULL) {
            fprintf(stderr, "down: out of memory\n");
            return 1;
        }

        if (stream_read_exact(s, tmp, (size_t)chunk_len) != 0) {
            fprintf(stderr, "down: failed to read chunk data\n");
            free(tmp);
            return 1;
        }

        if (write_all_file(out, tmp, (size_t)chunk_len) != 0) {
            fprintf(stderr, "down: failed to write output\n");
            free(tmp);
            return 1;
        }

        if (stream_read_line(s, line, sizeof(line)) != 0) {
            free(tmp);
            return 1;
        }
    }

    free(tmp);
    return 0;
}

int down_read_response(down_conn_t *conn, FILE *out, int verbose, down_response_t *resp) {
    unsigned char buf[8192];
    unsigned char *head = NULL;
    size_t head_len = 0;
    size_t head_cap = 0;
    ssize_t n;
    const unsigned char *body_start;
    size_t body_len;
    int content_length_found = 0;
    int chunked = 0;
    long long content_length = -1;
    char *headers_dup = NULL;
    char *line;
    down_stream_t stream;

    memset(resp, 0, sizeof(*resp));
    resp->status_code = 0;
    resp->content_length = -1;

    for (;;) {
        size_t old_len = head_len;
        unsigned char *p;

        n = down_conn_read(conn, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            fprintf(stderr, "down: failed to read response headers\n");
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
                fprintf(stderr, "down: out of memory\n");
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
        fprintf(stderr, "down: out of memory\n");
        free(head);
        return 1;
    }

    line = strtok(headers_dup, "\r\n");
    if (line == NULL || sscanf(line, "HTTP/%*s %d", &resp->status_code) != 1) {
        fprintf(stderr, "down: invalid HTTP status line\n");
        free(headers_dup);
        free(head);
        return 1;
    }

    for (;;) {
        line = strtok(NULL, "\r\n");
        if (line == NULL) {
            break;
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            char *v = line + 15;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            content_length = atoll(v);
            content_length_found = 1;
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            char *v = line + 18;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            if (strstr(v, "chunked") != NULL) {
                chunked = 1;
            }
        }
    }

    resp->chunked = chunked;
    resp->content_length = content_length_found ? content_length : -1;

    if (verbose) {
        fprintf(stderr, "down: HTTP %d\n", resp->status_code);
        if (chunked) {
            fprintf(stderr, "down: transfer-encoding: chunked\n");
        } else if (content_length_found) {
            fprintf(stderr, "down: content-length: %lld\n", content_length);
        }
    }

    stream.conn = conn;
    stream.prefill = body_start;
    stream.prefill_len = body_len;
    stream.prefill_pos = 0;

    if (chunked) {
        if (decode_chunked(&stream, out) != 0) {
            free(headers_dup);
            free(head);
            return 1;
        }
    } else if (content_length_found) {
        long long remain = content_length;
        unsigned char io_buf[8192];

        if ((long long)body_len > remain) {
            body_len = (size_t)remain;
        }
        if (body_len > 0) {
            if (write_all_file(out, body_start, body_len) != 0) {
                free(headers_dup);
                free(head);
                return 1;
            }
            remain -= (long long)body_len;
        }
        while (remain > 0) {
            size_t want = remain > (long long)sizeof(io_buf) ? sizeof(io_buf) : (size_t)remain;
            ssize_t rn = stream_read(&stream, io_buf, want);
            if (rn <= 0) {
                fprintf(stderr, "down: response ended early\n");
                free(headers_dup);
                free(head);
                return 1;
            }
            if (write_all_file(out, io_buf, (size_t)rn) != 0) {
                free(headers_dup);
                free(head);
                return 1;
            }
            remain -= (long long)rn;
        }
    } else {
        unsigned char io_buf[8192];
        if (body_len > 0 && write_all_file(out, body_start, body_len) != 0) {
            free(headers_dup);
            free(head);
            return 1;
        }
        for (;;) {
            ssize_t rn = down_conn_read(conn, io_buf, sizeof(io_buf));
            if (rn < 0 && errno == EINTR) {
                continue;
            }
            if (rn <= 0) {
                break;
            }
            if (write_all_file(out, io_buf, (size_t)rn) != 0) {
                free(headers_dup);
                free(head);
                return 1;
            }
        }
    }

    fflush(out);
    free(headers_dup);
    free(head);
    return 0;
}
