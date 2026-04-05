#include "../down.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim_left(char *s) {
    while (*s != '\0' && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        s++;
    }
    return s;
}

static void trim_right(char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[n - 1] = '\0';
            n--;
            continue;
        }
        break;
    }
}

static int starts_with_ci(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s == '\0') {
            return 0;
        }
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

static int ua_matches(const char *ua) {
    if (strcmp(ua, "*") == 0) {
        return 1;
    }
    return strstr(ua, "onetool-down") != NULL;
}

static int fetch_robots_txt(const down_request_t *req, char **out_text, down_response_t *out_resp) {
    down_request_t rr = {0};
    down_conn_t conn;
    FILE *tmp;
    down_response_t resp = {0};
    long sz;
    char *buf;
    int rc = 1;

    rr.host = req->host;
    rr.port = req->port;
    rr.path = "/robots.txt";
    rr.method = "GET";
    rr.timeout_sec = req->timeout_sec;
    rr.use_tls = req->use_tls;
    rr.header_count = 0;
    rr.body = NULL;

    if (down_socket_connect(&rr, &conn) != 0) {
        return 1;
    }
    if (down_send_request(&conn, &rr) != 0) {
        down_socket_close(&conn);
        return 1;
    }

    tmp = tmpfile();
    if (tmp == NULL) {
        down_socket_close(&conn);
        return 1;
    }

    if (down_read_response(&conn, tmp, 0, &resp) != 0) {
        fclose(tmp);
        down_socket_close(&conn);
        return 1;
    }

    if (fseek(tmp, 0, SEEK_END) != 0) {
        fclose(tmp);
        down_socket_close(&conn);
        return 1;
    }
    sz = ftell(tmp);
    if (sz < 0) {
        fclose(tmp);
        down_socket_close(&conn);
        return 1;
    }
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        down_socket_close(&conn);
        return 1;
    }

    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(tmp);
        down_socket_close(&conn);
        return 1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, tmp) != (size_t)sz) {
        free(buf);
        fclose(tmp);
        down_socket_close(&conn);
        return 1;
    }
    buf[sz] = '\0';

    *out_text = buf;
    *out_resp = resp;
    rc = 0;

    fclose(tmp);
    down_socket_close(&conn);
    return rc;
}

static int robots_allows_path(const char *robots_txt, const char *path) {
    char *work = strdup(robots_txt);
    char *line;
    int current_group_match = 0;
    int saw_ua_in_group = 0;
    int best_match_len = -1;
    int best_is_allow = 1;

    if (work == NULL) {
        return 1;
    }

    line = strtok(work, "\n");
    while (line != NULL) {
        char *p;
        char *colon;

        if ((p = strchr(line, '#')) != NULL) {
            *p = '\0';
        }
        p = trim_left(line);
        trim_right(p);

        if (*p == '\0') {
            saw_ua_in_group = 0;
            current_group_match = 0;
            line = strtok(NULL, "\n");
            continue;
        }

        colon = strchr(p, ':');
        if (colon == NULL) {
            line = strtok(NULL, "\n");
            continue;
        }
        *colon = '\0';
        char *key = trim_left(p);
        char *val = trim_left(colon + 1);
        trim_right(key);
        trim_right(val);

        for (char *k = key; *k != '\0'; k++) {
            *k = (char)tolower((unsigned char)*k);
        }

        if (strcmp(key, "user-agent") == 0) {
            if (!saw_ua_in_group) {
                current_group_match = 0;
                saw_ua_in_group = 1;
            }

            for (char *u = val; *u != '\0'; u++) {
                *u = (char)tolower((unsigned char)*u);
            }
            if (ua_matches(val)) {
                current_group_match = 1;
            }

            line = strtok(NULL, "\n");
            continue;
        }

        if (!current_group_match) {
            line = strtok(NULL, "\n");
            continue;
        }

        if (strcmp(key, "allow") == 0 || strcmp(key, "disallow") == 0) {
            int rule_is_allow = (strcmp(key, "allow") == 0);
            size_t rule_len = strlen(val);

            if (rule_len == 0) {
                line = strtok(NULL, "\n");
                continue;
            }
            if (strncmp(path, val, rule_len) == 0) {
                if ((int)rule_len > best_match_len) {
                    best_match_len = (int)rule_len;
                    best_is_allow = rule_is_allow;
                }
            }
        }

        line = strtok(NULL, "\n");
    }

    free(work);
    if (best_match_len < 0) {
        return 1;
    }
    return best_is_allow;
}

int down_check_robots(const down_request_t *req) {
    char *robots = NULL;
    down_response_t resp = {0};
    int allowed;

    if (req->ignore_robots) {
        return 0;
    }

    if (fetch_robots_txt(req, &robots, &resp) != 0) {
        if (req->verbose) {
            fprintf(stderr, "down: robots.txt check skipped (fetch failed)\n");
        }
        return 0;
    }

    if (resp.status_code == 404) {
        free(robots);
        return 0;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        if (req->verbose) {
            fprintf(stderr, "down: robots.txt returned HTTP %d, skipping policy\n", resp.status_code);
        }
        free(robots);
        return 0;
    }

    allowed = robots_allows_path(robots, req->path);
    free(robots);

    if (!allowed) {
        fprintf(stderr, "down: blocked by robots.txt for path %s\n", req->path);
        fprintf(stderr, "down: use --ignore-robots to override\n");
        return 1;
    }

    return 0;
}
