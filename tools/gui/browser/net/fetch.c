/*
 * HTTP/HTTPS fetcher built on libs/net's transport layer. We bypass
 * libs/net/http.c's response reader so we can capture the response
 * headers ourselves (needed for redirect "Location" handling) and read
 * the body directly into a memory buffer instead of a FILE*.
 *
 * Also supports file:// URLs (no validation - convenience for local
 * testing).
 */
#include "fetch.h"

#include "net.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FETCH_MAX_REDIRECTS  5
#define FETCH_TIMEOUT_SEC    10
#define FETCH_MAX_HEAD       65536
#define FETCH_MAX_BODY       (8 * 1024 * 1024)
#define FETCH_USER_AGENT     "onetool-browser/0.1"

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} membuf_t;

static int mb_reserve(membuf_t *m, size_t extra) {
    if (m->len + extra + 1 <= m->cap) return 0;
    size_t want = m->cap == 0 ? 8192 : m->cap;
    while (want < m->len + extra + 1) {
        if (want > (size_t)-1 / 2) return -1;
        want *= 2;
    }
    char *p = (char *)realloc(m->data, want);
    if (p == NULL) return -1;
    m->data = p;
    m->cap = want;
    return 0;
}

static int mb_append(membuf_t *m, const void *src, size_t n) {
    if (mb_reserve(m, n) != 0) return -1;
    memcpy(m->data + m->len, src, n);
    m->len += n;
    m->data[m->len] = '\0';
    return 0;
}

static void mb_free(membuf_t *m) {
    free(m->data);
    m->data = NULL;
    m->len = m->cap = 0;
}

/* Read until we've seen \r\n\r\n. Returns 0 on success, -1 on error.
 * On success, *body_off receives the offset where body starts. */
static int read_headers(net_conn_t *conn, membuf_t *m, size_t *body_off) {
    unsigned char buf[4096];
    for (;;) {
        ssize_t n = net_conn_read(conn, buf, sizeof(buf));
        if (n <= 0) return -1;
        if (mb_append(m, buf, (size_t)n) != 0) return -1;
        if (m->len > FETCH_MAX_HEAD) return -1;
        const char *s = m->data;
        for (size_t i = 0; i + 3 < m->len; i++) {
            if (s[i] == '\r' && s[i + 1] == '\n' &&
                s[i + 2] == '\r' && s[i + 3] == '\n') {
                *body_off = i + 4;
                return 0;
            }
        }
    }
}

/* Case-insensitive line-prefix match. */
static const char *header_value(const char *headers, size_t len,
                                const char *name) {
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = eol != NULL ? (size_t)(eol - p) : (size_t)(end - p);
        if (line_len > name_len && p[name_len] == ':' &&
            strncasecmp(p, name, name_len) == 0) {
            const char *v = p + name_len + 1;
            while (v < p + line_len && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
        if (eol == NULL) break;
        p = eol + 1;
    }
    return NULL;
}

static size_t value_len(const char *value) {
    size_t n = 0;
    while (value[n] != '\0' && value[n] != '\r' && value[n] != '\n') n++;
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t')) n--;
    return n;
}

static int parse_status(const char *headers, size_t len, int *code) {
    /* "HTTP/1.1 200 OK\r\n..." */
    const char *eol = memchr(headers, '\n', len);
    if (eol == NULL) return -1;
    const char *sp = memchr(headers, ' ', (size_t)(eol - headers));
    if (sp == NULL) return -1;
    *code = atoi(sp + 1);
    return 0;
}

static int read_body_content_length(net_conn_t *conn, membuf_t *body,
                                    long long content_len,
                                    const char *prefix, size_t prefix_len) {
    if (prefix_len > 0) {
        size_t take = prefix_len;
        if ((long long)take > content_len) take = (size_t)content_len;
        if (mb_append(body, prefix, take) != 0) return -1;
        if (body->len > FETCH_MAX_BODY) return -1;
    }
    long long remain = content_len - (long long)body->len;
    unsigned char buf[8192];
    while (remain > 0) {
        size_t want = remain > (long long)sizeof(buf) ? sizeof(buf) : (size_t)remain;
        ssize_t n = net_conn_read(conn, buf, want);
        if (n <= 0) return -1;
        if (mb_append(body, buf, (size_t)n) != 0) return -1;
        if (body->len > FETCH_MAX_BODY) return -1;
        remain -= n;
    }
    return 0;
}

static int read_body_close_delim(net_conn_t *conn, membuf_t *body,
                                 const char *prefix, size_t prefix_len) {
    if (prefix_len > 0) {
        if (mb_append(body, prefix, prefix_len) != 0) return -1;
        if (body->len > FETCH_MAX_BODY) return -1;
    }
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = net_conn_read(conn, buf, sizeof(buf));
        if (n < 0) return -1;
        if (n == 0) return 0;
        if (mb_append(body, buf, (size_t)n) != 0) return -1;
        if (body->len > FETCH_MAX_BODY) return -1;
    }
}

/* Simple stream over (prefix bytes, then conn). Used by chunked decoder. */
typedef struct {
    net_conn_t  *conn;
    const char  *pre;
    size_t       pre_len;
    size_t       pre_pos;
} pre_stream_t;

static ssize_t ps_read(pre_stream_t *s, unsigned char *dst, size_t cap) {
    if (s->pre_pos < s->pre_len) {
        size_t n = s->pre_len - s->pre_pos;
        if (n > cap) n = cap;
        memcpy(dst, s->pre + s->pre_pos, n);
        s->pre_pos += n;
        return (ssize_t)n;
    }
    return net_conn_read(s->conn, dst, cap);
}

static int ps_read_exact(pre_stream_t *s, unsigned char *dst, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = ps_read(s, dst + off, n - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int ps_read_line(pre_stream_t *s, char *line, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap) {
        unsigned char c;
        if (ps_read_exact(s, &c, 1) != 0) return -1;
        line[i++] = (char)c;
        if (c == '\n') { line[i] = '\0'; return 0; }
    }
    return -1;
}

static int read_body_chunked(net_conn_t *conn, membuf_t *body,
                             const char *prefix, size_t prefix_len) {
    pre_stream_t s = { conn, prefix, prefix_len, 0 };
    char line[4096];
    for (;;) {
        if (ps_read_line(&s, line, sizeof(line)) != 0) return -1;
        unsigned long chunk_len = strtoul(line, NULL, 16);
        if (chunk_len == 0) {
            do {
                if (ps_read_line(&s, line, sizeof(line)) != 0) return -1;
            } while (strcmp(line, "\r\n") != 0 && strcmp(line, "\n") != 0);
            return 0;
        }
        if (mb_reserve(body, chunk_len) != 0) return -1;
        if (ps_read_exact(&s, (unsigned char *)body->data + body->len, chunk_len) != 0)
            return -1;
        body->len += chunk_len;
        body->data[body->len] = '\0';
        if (body->len > FETCH_MAX_BODY) return -1;
        if (ps_read_line(&s, line, sizeof(line)) != 0) return -1;
    }
}

static int fetch_file_url(const char *path, char **out_body, size_t *out_len,
                          const char **err) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) { *err = "open failed"; return -1; }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        *err = "not a regular file";
        return -1;
    }
    if ((size_t)st.st_size > FETCH_MAX_BODY) {
        fclose(f); *err = "file too large"; return -1;
    }
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (buf == NULL) { fclose(f); *err = "out of memory"; return -1; }
    size_t n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[n] = '\0';
    *out_body = buf;
    *out_len = n;
    return 0;
}

/* Resolve "Location" header value against base into a malloc'd absolute URL.
 * Handles three cases: already absolute, "//host/path", "/path", "rel/path". */
static char *resolve_redirect(const char *base, const char *loc, size_t loc_len) {
    if (loc_len == 0) return NULL;
    /* absolute? */
    if ((loc_len > 7 && strncasecmp(loc, "http://", 7) == 0) ||
        (loc_len > 8 && strncasecmp(loc, "https://", 8) == 0)) {
        char *out = (char *)malloc(loc_len + 1);
        if (out == NULL) return NULL;
        memcpy(out, loc, loc_len);
        out[loc_len] = '\0';
        return out;
    }
    /* parse base scheme + host */
    const char *scheme_end = strstr(base, "://");
    if (scheme_end == NULL) return NULL;
    const char *host_start = scheme_end + 3;
    const char *host_end = host_start;
    while (*host_end != '\0' && *host_end != '/' && *host_end != '?' && *host_end != '#')
        host_end++;
    size_t scheme_len = (size_t)(scheme_end - base);
    size_t host_len = (size_t)(host_end - host_start);
    /* protocol-relative // */
    if (loc_len >= 2 && loc[0] == '/' && loc[1] == '/') {
        size_t out_len = scheme_len + 1 + loc_len; /* "http:" + "//foo" */
        char *out = (char *)malloc(out_len + 2);
        if (out == NULL) return NULL;
        memcpy(out, base, scheme_len);
        out[scheme_len] = ':';
        memcpy(out + scheme_len + 1, loc, loc_len);
        out[scheme_len + 1 + loc_len] = '\0';
        return out;
    }
    /* root-relative /... */
    if (loc[0] == '/') {
        size_t prefix = scheme_len + 3 + host_len;
        char *out = (char *)malloc(prefix + loc_len + 1);
        if (out == NULL) return NULL;
        memcpy(out, base, prefix);
        memcpy(out + prefix, loc, loc_len);
        out[prefix + loc_len] = '\0';
        return out;
    }
    /* relative path -> drop trailing segment of base path then append */
    const char *path_start = host_end;
    const char *path_end = path_start;
    while (*path_end != '\0' && *path_end != '?' && *path_end != '#') path_end++;
    /* find last '/' in the path */
    const char *last_slash = path_start;
    for (const char *p = path_start; p < path_end; p++) {
        if (*p == '/') last_slash = p;
    }
    size_t base_prefix = (size_t)(last_slash - base) + 1;
    if (last_slash == path_start) {
        /* no path at all - synthesize a leading '/' */
        size_t prefix = scheme_len + 3 + host_len;
        char *out = (char *)malloc(prefix + 1 + loc_len + 1);
        if (out == NULL) return NULL;
        memcpy(out, base, prefix);
        out[prefix] = '/';
        memcpy(out + prefix + 1, loc, loc_len);
        out[prefix + 1 + loc_len] = '\0';
        return out;
    }
    char *out = (char *)malloc(base_prefix + loc_len + 1);
    if (out == NULL) return NULL;
    memcpy(out, base, base_prefix);
    memcpy(out + base_prefix, loc, loc_len);
    out[base_prefix + loc_len] = '\0';
    return out;
}

int browser_fetch_url(const char *url,
                      char **out_body,
                      size_t *out_len,
                      char **out_final,
                      const char **err) {
    if (out_body == NULL || out_len == NULL || url == NULL) {
        if (err != NULL) *err = "bad arg";
        return -1;
    }
    const char *local_err = NULL;
    if (err == NULL) err = &local_err;
    *err = NULL;

    if (strncasecmp(url, "file://", 7) == 0) {
        int rc = fetch_file_url(url + 7, out_body, out_len, err);
        if (rc == 0 && out_final != NULL) *out_final = strdup(url);
        return rc;
    }

    char *cur_url = strdup(url);
    if (cur_url == NULL) { *err = "out of memory"; return -1; }

    for (int hop = 0; hop <= FETCH_MAX_REDIRECTS; hop++) {
        net_url_t u;
        if (net_parse_http_url(cur_url, &u) != 0) {
            *err = "bad URL";
            free(cur_url);
            return -1;
        }

        net_conn_t conn = {0};
        conn.fd = -1;
        if (net_conn_open(&conn, u.host, u.port, FETCH_TIMEOUT_SEC,
                          u.use_tls, u.host, 0) != 0) {
            *err = "connect failed";
            net_free_url(&u);
            free(cur_url);
            return -1;
        }

        if (net_http_send_request(&conn, "GET", u.path, u.host, NULL, 0,
                                  NULL, FETCH_USER_AGENT) != 0) {
            *err = "send failed";
            net_conn_close(&conn);
            net_free_url(&u);
            free(cur_url);
            return -1;
        }

        membuf_t head = {0};
        size_t body_off = 0;
        if (read_headers(&conn, &head, &body_off) != 0) {
            *err = "header read failed";
            mb_free(&head);
            net_conn_close(&conn);
            net_free_url(&u);
            free(cur_url);
            return -1;
        }
        int status = 0;
        if (parse_status(head.data, body_off, &status) != 0) {
            *err = "bad status line";
            mb_free(&head);
            net_conn_close(&conn);
            net_free_url(&u);
            free(cur_url);
            return -1;
        }

        if (status >= 300 && status < 400) {
            const char *loc = header_value(head.data, body_off, "Location");
            if (loc == NULL) {
                *err = "redirect with no Location";
                mb_free(&head);
                net_conn_close(&conn);
                net_free_url(&u);
                free(cur_url);
                return -1;
            }
            size_t loc_len = value_len(loc);
            char *next = resolve_redirect(cur_url, loc, loc_len);
            mb_free(&head);
            net_conn_close(&conn);
            net_free_url(&u);
            if (next == NULL) {
                *err = "bad redirect URL";
                free(cur_url);
                return -1;
            }
            free(cur_url);
            cur_url = next;
            continue;
        }

        if (status < 200 || status >= 300) {
            static char buf[32];
            snprintf(buf, sizeof(buf), "HTTP %d", status);
            *err = buf;
            mb_free(&head);
            net_conn_close(&conn);
            net_free_url(&u);
            free(cur_url);
            return -1;
        }

        const char *te = header_value(head.data, body_off, "Transfer-Encoding");
        int chunked = te != NULL && strncasecmp(te, "chunked", 7) == 0;
        const char *cl = header_value(head.data, body_off, "Content-Length");
        long long content_len = cl != NULL ? atoll(cl) : -1;

        const char *prefix = head.data + body_off;
        size_t prefix_len = head.len - body_off;

        membuf_t body = {0};
        int rc;
        if (chunked) {
            rc = read_body_chunked(&conn, &body, prefix, prefix_len);
        } else if (content_len >= 0) {
            rc = read_body_content_length(&conn, &body, content_len,
                                          prefix, prefix_len);
        } else {
            rc = read_body_close_delim(&conn, &body, prefix, prefix_len);
        }

        mb_free(&head);
        net_conn_close(&conn);
        net_free_url(&u);

        if (rc != 0) {
            mb_free(&body);
            *err = "body read failed";
            free(cur_url);
            return -1;
        }

        *out_body = body.data;
        *out_len = body.len;
        if (out_final != NULL) *out_final = cur_url;
        else free(cur_url);
        return 0;
    }

    *err = "too many redirects";
    free(cur_url);
    return -1;
}
