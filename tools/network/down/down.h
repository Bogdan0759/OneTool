#ifndef ONETOOL_DOWN_H
#define ONETOOL_DOWN_H

#include <stdio.h>
#include <sys/types.h>

#define DOWN_MAX_HEADERS 64

typedef struct {
    char *url;
    char *method;
    char *host;
    char *port;
    char *path;
    char *body;
    char *output_path;
    char *headers[DOWN_MAX_HEADERS];
    int header_count;
    int timeout_sec;
    int verbose;
    int use_tls;
    int ignore_robots;
} down_request_t;

typedef struct {
    int status_code;
    int chunked;
    long long content_length;
} down_response_t;

typedef struct {
    int fd;
    void *ssl_ctx;
    void *ssl;
    int use_tls;
} down_conn_t;

void down_request_init(down_request_t *req);
void down_request_free(down_request_t *req);
int down_parse_cli(int argc, char *argv[], down_request_t *req);
int down_parse_url(down_request_t *req);
void down_print_cli_help(const char *tool_name);

int down_socket_connect(const down_request_t *req, down_conn_t *conn);
void down_socket_close(down_conn_t *conn);
ssize_t down_conn_read(down_conn_t *conn, void *buf, size_t len);
ssize_t down_conn_write(down_conn_t *conn, const void *buf, size_t len);
int down_send_all(down_conn_t *conn, const void *buf, size_t len);
int down_send_request(down_conn_t *conn, const down_request_t *req);
int down_read_response(down_conn_t *conn, FILE *out, int verbose, down_response_t *resp);
int down_check_robots(const down_request_t *req);
int down_run(const down_request_t *req, FILE *out);

#endif
