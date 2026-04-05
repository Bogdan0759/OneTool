#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int wait_connect(int fd, int timeout_sec) {
    fd_set wfds;
    struct timeval tv;
    int rc;
    int so_err = 0;
    socklen_t so_len = sizeof(so_err);

    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    rc = select(fd + 1, NULL, &wfds, NULL, &tv);
    if (rc == 0) {
        errno = ETIMEDOUT;
        return 1;
    }
    if (rc < 0) {
        return 1;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) != 0) {
        return 1;
    }
    if (so_err != 0) {
        errno = so_err;
        return 1;
    }
    return 0;
}

static int tcp_connect(const char *host, const char *port, int timeout_sec, int noisy) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        if (noisy) {
            fprintf(stderr, "net: resolve failed for %s:%s: %s\n", host, port, gai_strerror(rc));
        }
        return -1;
    }

    for (it = res; it != NULL; it = it->ai_next) {
        int flags;

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        if (rc != 0 && wait_connect(fd, timeout_sec) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (fcntl(fd, F_SETFL, flags) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(res);

    if (fd < 0 && noisy) {
        fprintf(stderr, "net: connect to %s:%s failed: %s\n", host, port, strerror(errno));
    }
    return fd;
}

int net_conn_open(net_conn_t *conn, const char *host, const char *port, int timeout_sec, int use_tls, const char *tls_server_name, int noisy) {
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    int fd;

    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    conn->use_tls = use_tls;

    fd = tcp_connect(host, port, timeout_sec, noisy);
    if (fd < 0) {
        return 1;
    }
    conn->fd = fd;

    if (!use_tls) {
        return 0;
    }

    SSL_load_error_strings();
    OPENSSL_init_ssl(0, NULL);

    ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == NULL) {
        if (noisy) {
            fprintf(stderr, "net: SSL_CTX_new failed\n");
        }
        close(fd);
        conn->fd = -1;
        return 1;
    }

    ssl = SSL_new(ctx);
    if (ssl == NULL) {
        if (noisy) {
            fprintf(stderr, "net: SSL_new failed\n");
        }
        SSL_CTX_free(ctx);
        close(fd);
        conn->fd = -1;
        return 1;
    }

    if (tls_server_name != NULL) {
        SSL_set_tlsext_host_name(ssl, tls_server_name);
        SSL_set1_host(ssl, tls_server_name);
    }
    SSL_set_fd(ssl, fd);

    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        if (noisy) {
            fprintf(stderr, "net: TLS handshake failed: %s\n", ERR_error_string(e, NULL));
        }
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        conn->fd = -1;
        return 1;
    }

    conn->ssl_ctx = ctx;
    conn->ssl = ssl;
    return 0;
}

void net_conn_close(net_conn_t *conn) {
    if (conn->ssl != NULL) {
        SSL_shutdown((SSL *)conn->ssl);
        SSL_free((SSL *)conn->ssl);
        conn->ssl = NULL;
    }
    if (conn->ssl_ctx != NULL) {
        SSL_CTX_free((SSL_CTX *)conn->ssl_ctx);
        conn->ssl_ctx = NULL;
    }
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

ssize_t net_conn_read(net_conn_t *conn, void *buf, size_t len) {
    if (conn->use_tls) {
        int n;
        do {
            n = SSL_read((SSL *)conn->ssl, buf, (int)len);
            if (n > 0) {
                return (ssize_t)n;
            }
            if (n == 0) {
                return 0;
            }
            if (SSL_get_error((SSL *)conn->ssl, n) == SSL_ERROR_WANT_READ) {
                continue;
            }
            return -1;
        } while (1);
    }

    for (;;) {
        ssize_t n = recv(conn->fd, buf, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

ssize_t net_conn_write(net_conn_t *conn, const void *buf, size_t len) {
    if (conn->use_tls) {
        int n;
        do {
            n = SSL_write((SSL *)conn->ssl, buf, (int)len);
            if (n > 0) {
                return (ssize_t)n;
            }
            if (SSL_get_error((SSL *)conn->ssl, n) == SSL_ERROR_WANT_WRITE) {
                continue;
            }
            return -1;
        } while (1);
    }

    for (;;) {
        ssize_t n = send(conn->fd, buf, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}
