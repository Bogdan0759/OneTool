#include "../down.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *down_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n);
    return p;
}

static char *down_strndup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static int set_string(char **dst, const char *src) {
    char *copy = down_strdup(src);
    if (copy == NULL) {
        fprintf(stderr, "down: out of memory\n");
        return 1;
    }
    free(*dst);
    *dst = copy;
    return 0;
}

void down_request_init(down_request_t *req) {
    memset(req, 0, sizeof(*req));
    req->timeout_sec = 10;
    req->use_tls = 0;
}

void down_request_free(down_request_t *req) {
    free(req->url);
    free(req->method);
    free(req->host);
    free(req->port);
    free(req->path);
    free(req->body);
    free(req->output_path);

    for (int i = 0; i < req->header_count; i++) {
        free(req->headers[i]);
    }
    memset(req, 0, sizeof(*req));
}

void down_print_cli_help(const char *tool_name) {
    printf("usage: %s <url> [options]\n", tool_name);
    printf("options:\n");
    printf("  -o <path>          write body to file\n");
    printf("  -X <method>        HTTP method (default GET, or POST when -d is used)\n");
    printf("  -H <header>        add request header, can be used multiple times\n");
    printf("  -d <data>          request body\n");
    printf("  --timeout <sec>    connect timeout in seconds (default 10)\n");
    printf("  -v                 verbose output to stderr\n");
    printf("  --help             show this help\n");
}

int down_parse_url(down_request_t *req) {
    const char *raw = req->url;
    const char *p = raw;
    const char *host_start;
    const char *path_start;
    const char *host_end;
    const char *port_sep;
    size_t host_len;
    size_t path_len;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        req->use_tls = 1;
        p += 8;
    }

    host_start = p;
    path_start = strchr(host_start, '/');
    if (path_start == NULL) {
        host_end = host_start + strlen(host_start);
        path_start = host_end;
    } else {
        host_end = path_start;
    }

    if (host_end == host_start) {
        fprintf(stderr, "down: invalid URL (empty host)\n");
        return 1;
    }

    port_sep = NULL;
    for (const char *it = host_start; it < host_end; it++) {
        if (*it == ':') {
            port_sep = it;
            break;
        }
    }

    if (port_sep != NULL) {
        size_t port_len;
        host_len = (size_t)(port_sep - host_start);
        if (host_len == 0) {
            fprintf(stderr, "down: invalid URL (empty host)\n");
            return 1;
        }
        free(req->host);
        req->host = down_strndup(host_start, host_len);
        if (req->host == NULL) {
            fprintf(stderr, "down: out of memory\n");
            return 1;
        }

        port_len = (size_t)(host_end - (port_sep + 1));
        if (port_len == 0) {
            fprintf(stderr, "down: invalid URL (empty port)\n");
            return 1;
        }
        free(req->port);
        req->port = down_strndup(port_sep + 1, port_len);
        if (req->port == NULL) {
            fprintf(stderr, "down: out of memory\n");
            return 1;
        }
    } else {
        host_len = (size_t)(host_end - host_start);
        free(req->host);
        req->host = down_strndup(host_start, host_len);
        if (req->host == NULL) {
            fprintf(stderr, "down: out of memory\n");
            return 1;
        }

        if (set_string(&req->port, "80") != 0) {
            return 1;
        }
        if (req->use_tls) {
            if (set_string(&req->port, "443") != 0) {
                return 1;
            }
        }
    }

    if (*path_start == '\0') {
        if (set_string(&req->path, "/") != 0) {
            return 1;
        }
    } else {
        path_len = strlen(path_start);
        free(req->path);
        req->path = down_strndup(path_start, path_len);
        if (req->path == NULL) {
            fprintf(stderr, "down: out of memory\n");
            return 1;
        }
    }

    return 0;
}

int down_parse_cli(int argc, char *argv[], down_request_t *req) {
    const char *url = NULL;

    down_request_init(req);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            down_print_cli_help(argv[0]);
            return 2;
        }

        if (strcmp(argv[i], "-v") == 0) {
            req->verbose = 1;
            continue;
        }

        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "down: -o requires a value\n");
                return 1;
            }
            if (set_string(&req->output_path, argv[++i]) != 0) {
                return 1;
            }
            continue;
        }

        if (strcmp(argv[i], "-X") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "down: -X requires a value\n");
                return 1;
            }
            if (set_string(&req->method, argv[++i]) != 0) {
                return 1;
            }
            continue;
        }

        if (strcmp(argv[i], "-H") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "down: -H requires a value\n");
                return 1;
            }
            if (req->header_count >= DOWN_MAX_HEADERS) {
                fprintf(stderr, "down: too many headers\n");
                return 1;
            }
            req->headers[req->header_count] = down_strdup(argv[++i]);
            if (req->headers[req->header_count] == NULL) {
                fprintf(stderr, "down: out of memory\n");
                return 1;
            }
            req->header_count++;
            continue;
        }

        if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "down: -d requires a value\n");
                return 1;
            }
            if (set_string(&req->body, argv[++i]) != 0) {
                return 1;
            }
            continue;
        }

        if (strcmp(argv[i], "--timeout") == 0) {
            char *end = NULL;
            long t;
            if (i + 1 >= argc) {
                fprintf(stderr, "down: --timeout requires a value\n");
                return 1;
            }
            t = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || t <= 0 || t > 3600) {
                fprintf(stderr, "down: invalid timeout: %s\n", argv[i]);
                return 1;
            }
            req->timeout_sec = (int)t;
            continue;
        }

        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "down: unknown option: %s\n", argv[i]);
            return 1;
        }

        if (url != NULL) {
            fprintf(stderr, "down: only one URL is allowed\n");
            return 1;
        }
        url = argv[i];
    }

    if (url == NULL) {
        fprintf(stderr, "down: URL is required\n");
        return 1;
    }
    if (set_string(&req->url, url) != 0) {
        return 1;
    }

    if (req->method == NULL) {
        if (req->body != NULL) {
            if (set_string(&req->method, "POST") != 0) {
                return 1;
            }
        } else {
            if (set_string(&req->method, "GET") != 0) {
                return 1;
            }
        }
    } else {
        for (char *m = req->method; *m != '\0'; m++) {
            *m = (char)toupper((unsigned char)*m);
        }
    }

    return down_parse_url(req);
}
