#include "net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim_left(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}

static void trim_right(char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[n - 1] = '\0';
            n--;
        } else {
            break;
        }
    }
}

static int ua_matches(const char *ua, const char *target) {
    if (strcmp(ua, "*") == 0) return 1;
    return strstr(target, ua) != NULL;
}

static int fetch_robots(const char *host, const char *port, int use_tls, int timeout_sec, const char *user_agent, char **robots_txt, int *status_code) {
    net_conn_t conn;
    net_http_response_t resp;
    FILE *tmp = tmpfile();
    long sz;
    char *buf;
    const char *headers[1];

    if (tmp == NULL) {
        return 1;
    }
    if (net_conn_open(&conn, host, port, timeout_sec, use_tls, host, 0) != 0) {
        fclose(tmp);
        return 1;
    }

    headers[0] = "Accept: text/plain";
    if (net_http_send_request(&conn, "GET", "/robots.txt", host, headers, 1, NULL, user_agent) != 0) {
        net_conn_close(&conn);
        fclose(tmp);
        return 1;
    }
    if (net_http_read_response(&conn, tmp, 0, &resp) != 0) {
        net_conn_close(&conn);
        fclose(tmp);
        return 1;
    }
    net_conn_close(&conn);

    if (fseek(tmp, 0, SEEK_END) != 0) {
        fclose(tmp);
        return 1;
    }
    sz = ftell(tmp);
    if (sz < 0 || fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        return 1;
    }

    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(tmp);
        return 1;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, tmp) != (size_t)sz) {
        free(buf);
        fclose(tmp);
        return 1;
    }
    buf[sz] = '\0';
    fclose(tmp);

    *robots_txt = buf;
    *status_code = resp.status_code;
    return 0;
}

static int rules_allow(const char *txt, const char *path, const char *agent) {
    char *work = strdup(txt);
    char *line;
    int current_group_match = 0;
    int saw_ua = 0;
    int best_len = -1;
    int best_allow = 1;

    if (work == NULL) return 1;

    line = strtok(work, "\n");
    while (line != NULL) {
        char *p = line;
        char *hash = strchr(p, '#');
        char *colon;
        char *key;
        char *val;

        if (hash) *hash = '\0';
        p = trim_left(p);
        trim_right(p);
        if (*p == '\0') {
            saw_ua = 0;
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
        key = trim_left(p);
        val = trim_left(colon + 1);
        trim_right(key);
        trim_right(val);

        for (char *k = key; *k; k++) *k = (char)tolower((unsigned char)*k);
        for (char *v = val; *v; v++) *v = (char)tolower((unsigned char)*v);

        if (strcmp(key, "user-agent") == 0) {
            if (!saw_ua) {
                saw_ua = 1;
                current_group_match = 0;
            }
            if (ua_matches(val, agent)) {
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
            int allow = (strcmp(key, "allow") == 0);
            size_t rlen = strlen(val);
            if (rlen > 0 && strncmp(path, val, rlen) == 0) {
                if ((int)rlen > best_len) {
                    best_len = (int)rlen;
                    best_allow = allow;
                }
            }
        }

        line = strtok(NULL, "\n");
    }

    free(work);
    if (best_len < 0) return 1;
    return best_allow;
}

int net_robots_is_allowed(
    const char *host,
    const char *port,
    int use_tls,
    int timeout_sec,
    const char *path,
    const char *user_agent,
    int verbose,
    int *allowed
) {
    char *txt = NULL;
    int status = 0;
    char agent_lc[128];
    size_t n;

    *allowed = 1;
    if (fetch_robots(host, port, use_tls, timeout_sec, user_agent, &txt, &status) != 0) {
        if (verbose) fprintf(stderr, "net: robots check skipped (fetch failed)\n");
        return 0;
    }

    if (status == 404 || status < 200 || status >= 300) {
        free(txt);
        return 0;
    }

    n = strlen(user_agent);
    if (n >= sizeof(agent_lc)) n = sizeof(agent_lc) - 1;
    memcpy(agent_lc, user_agent, n);
    agent_lc[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        agent_lc[i] = (char)tolower((unsigned char)agent_lc[i]);
    }

    *allowed = rules_allow(txt, path, agent_lc);
    free(txt);
    return 0;
}
