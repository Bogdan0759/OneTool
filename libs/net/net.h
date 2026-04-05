#ifndef ONETOOL_LIBS_NET_H
#define ONETOOL_LIBS_NET_H

#include <stdio.h>
#include <sys/types.h>

typedef struct {
    char *host;
    char *port;
    char *path;
    int use_tls;
} net_url_t;

typedef struct {
    int fd;
    void *ssl_ctx;
    void *ssl;
    int use_tls;
} net_conn_t;

typedef struct {
    int status_code;
    int chunked;
    long long content_length;
    long long body_bytes;
} net_http_response_t;

int net_parse_http_url(const char *url, net_url_t *out);
void net_free_url(net_url_t *url);

int net_conn_open(net_conn_t *conn, const char *host, const char *port, int timeout_sec, int use_tls, const char *tls_server_name, int noisy);
void net_conn_close(net_conn_t *conn);
ssize_t net_conn_read(net_conn_t *conn, void *buf, size_t len);
ssize_t net_conn_write(net_conn_t *conn, const void *buf, size_t len);

int net_http_send_request(
    net_conn_t *conn,
    const char *method,
    const char *path,
    const char *host,
    const char *headers[],
    int header_count,
    const char *body,
    const char *user_agent
);
int net_http_read_response(net_conn_t *conn, FILE *out, int verbose, net_http_response_t *resp);

int net_robots_is_allowed(
    const char *host,
    const char *port,
    int use_tls,
    int timeout_sec,
    const char *path,
    const char *user_agent,
    int verbose,
    int *allowed
);

#endif
