#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_n(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void net_free_url(net_url_t *url) {
    if (url == NULL) {
        return;
    }
    free(url->host);
    free(url->port);
    free(url->path);
    memset(url, 0, sizeof(*url));
}

int net_parse_http_url(const char *url, net_url_t *out) {
    const char *p = url;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    const char *port_sep = NULL;

    memset(out, 0, sizeof(*out));

    if (strncmp(p, "http://", 7) == 0) {
        out->use_tls = 0;
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        out->use_tls = 1;
        p += 8;
    } else {
        out->use_tls = 0;
    }

    host_start = p;
    path_start = strchr(host_start, '/');
    host_end = path_start ? path_start : host_start + strlen(host_start);
    path_start = path_start ? path_start : host_end;

    if (host_end == host_start) {
        fprintf(stderr, "net: invalid URL (empty host)\n");
        return 1;
    }

    for (const char *it = host_start; it < host_end; it++) {
        if (*it == ':') {
            port_sep = it;
            break;
        }
    }

    if (port_sep != NULL) {
        size_t host_len = (size_t)(port_sep - host_start);
        size_t port_len = (size_t)(host_end - (port_sep + 1));
        if (host_len == 0 || port_len == 0) {
            fprintf(stderr, "net: invalid URL (host/port)\n");
            return 1;
        }
        out->host = dup_n(host_start, host_len);
        out->port = dup_n(port_sep + 1, port_len);
    } else {
        size_t host_len = (size_t)(host_end - host_start);
        out->host = dup_n(host_start, host_len);
        out->port = strdup(out->use_tls ? "443" : "80");
    }

    if (out->host == NULL || out->port == NULL) {
        fprintf(stderr, "net: out of memory\n");
        net_free_url(out);
        return 1;
    }

    if (*path_start == '\0') {
        out->path = strdup("/");
    } else {
        out->path = strdup(path_start);
    }
    if (out->path == NULL) {
        fprintf(stderr, "net: out of memory\n");
        net_free_url(out);
        return 1;
    }

    return 0;
}
