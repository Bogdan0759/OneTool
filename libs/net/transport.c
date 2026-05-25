#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* Long-lived SSL context shared by every TLS connection in the process.
 *
 * Creating a fresh SSL_CTX per connection adds ~10-30 ms (loads default
 * ciphers, init internal tables, etc.). More importantly, OpenSSL keeps
 * its session cache on the CTX — so a persistent CTX lets repeat
 * requests to the same host resume the TLS session and skip a full
 * handshake (~1 RTT saved per connection).
 *
 * Init is done under pthread_once so async fetcher threads racing on
 * the first request don't both build the CTX. */
static SSL_CTX     *g_tls_ctx = NULL;
static pthread_once_t g_tls_ctx_once = PTHREAD_ONCE_INIT;

static void tls_ctx_init(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                     | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    g_tls_ctx = SSL_CTX_new(TLS_client_method());
    if (g_tls_ctx == NULL) return;
    SSL_CTX_set_min_proto_version(g_tls_ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(g_tls_ctx,
                        SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    /* Enable client-side session caching so SSL_connect can resume an
     * earlier session ticket for the same host. */
    SSL_CTX_set_session_cache_mode(g_tls_ctx,
        SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
    /* No peer verification — same posture as before this change. */
    SSL_CTX_set_verify(g_tls_ctx, SSL_VERIFY_NONE, NULL);
}

static SSL_CTX *get_tls_ctx(void) {
    pthread_once(&g_tls_ctx_once, tls_ctx_init);
    return g_tls_ctx;
}

/* Per-host TLS session cache — keyed by the SNI host name. Lets
 * SSL_connect resume the TLS session of a previous connection to the
 * same host (saves ~1 RTT and a full asymmetric crypto operation).
 *
 * Tiny LRU-ish array; on overflow we just stomp the oldest slot. The
 * mutex is fine-grained because session save/load is a few pointer
 * ops, not network I/O. */
#define TLS_SESS_CACHE_SLOTS 16
typedef struct {
    char         host[128];
    SSL_SESSION *session;
    unsigned     gen;
} tls_sess_slot_t;

static tls_sess_slot_t g_sess_cache[TLS_SESS_CACHE_SLOTS];
static unsigned        g_sess_gen = 0;
static pthread_mutex_t g_sess_mtx = PTHREAD_MUTEX_INITIALIZER;

static SSL_SESSION *sess_cache_get(const char *host) {
    if (host == NULL || host[0] == '\0') return NULL;
    SSL_SESSION *out = NULL;
    pthread_mutex_lock(&g_sess_mtx);
    for (int i = 0; i < TLS_SESS_CACHE_SLOTS; i++) {
        if (g_sess_cache[i].session != NULL &&
            strncmp(g_sess_cache[i].host, host,
                    sizeof(g_sess_cache[i].host)) == 0) {
            out = g_sess_cache[i].session;
            /* Bump ref so the caller can SSL_set_session and then
             * SSL_SESSION_free without invalidating our cached entry. */
            SSL_SESSION_up_ref(out);
            g_sess_cache[i].gen = ++g_sess_gen;
            break;
        }
    }
    pthread_mutex_unlock(&g_sess_mtx);
    return out;
}

static void sess_cache_put(const char *host, SSL_SESSION *sess) {
    if (host == NULL || host[0] == '\0' || sess == NULL) return;
    pthread_mutex_lock(&g_sess_mtx);
    int free_idx = -1, oldest_idx = 0;
    unsigned oldest_gen = (unsigned)-1;
    for (int i = 0; i < TLS_SESS_CACHE_SLOTS; i++) {
        if (g_sess_cache[i].session != NULL &&
            strncmp(g_sess_cache[i].host, host,
                    sizeof(g_sess_cache[i].host)) == 0) {
            /* refresh */
            SSL_SESSION_free(g_sess_cache[i].session);
            g_sess_cache[i].session = sess;   /* takes ownership */
            g_sess_cache[i].gen = ++g_sess_gen;
            pthread_mutex_unlock(&g_sess_mtx);
            return;
        }
        if (g_sess_cache[i].session == NULL && free_idx < 0) free_idx = i;
        if (g_sess_cache[i].gen < oldest_gen) {
            oldest_gen = g_sess_cache[i].gen;
            oldest_idx = i;
        }
    }
    int slot = free_idx >= 0 ? free_idx : oldest_idx;
    if (g_sess_cache[slot].session != NULL) {
        SSL_SESSION_free(g_sess_cache[slot].session);
    }
    strncpy(g_sess_cache[slot].host, host, sizeof(g_sess_cache[slot].host) - 1);
    g_sess_cache[slot].host[sizeof(g_sess_cache[slot].host) - 1] = '\0';
    g_sess_cache[slot].session = sess;       /* takes ownership */
    g_sess_cache[slot].gen = ++g_sess_gen;
    pthread_mutex_unlock(&g_sess_mtx);
}

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

        /* Disable Nagle. Browser HTTP requests are small + interactive;
         * waiting for ACK before sending the next packet adds ~40 ms of
         * latency that the user perceives as the page being slow to
         * start loading. */
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

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

    SSL_CTX *ctx = get_tls_ctx();
    if (ctx == NULL) {
        if (noisy) {
            fprintf(stderr, "net: SSL_CTX init failed\n");
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
        close(fd);
        conn->fd = -1;
        return 1;
    }

    if (tls_server_name != NULL) {
        SSL_set_tlsext_host_name(ssl, tls_server_name);
        SSL_set1_host(ssl, tls_server_name);
    }
    /* If we have a session from a previous handshake to this host,
     * attach it so SSL_connect performs the fast resume path
     * (one round trip with TLS 1.2 ticket; zero RTT data is not used). */
    SSL_SESSION *prev = sess_cache_get(tls_server_name != NULL ? tls_server_name : host);
    if (prev != NULL) {
        SSL_set_session(ssl, prev);
        SSL_SESSION_free(prev);
    }
    SSL_set_fd(ssl, fd);

    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        if (noisy) {
            fprintf(stderr, "net: TLS handshake failed: %s\n", ERR_error_string(e, NULL));
        }
        SSL_free(ssl);
        close(fd);
        conn->fd = -1;
        return 1;
    }

    /* Save the negotiated session for the next request to this host. */
    SSL_SESSION *new_sess = SSL_get1_session(ssl);
    if (new_sess != NULL) {
        sess_cache_put(tls_server_name != NULL ? tls_server_name : host, new_sess);
    }

    conn->ssl_ctx = NULL;     /* shared, do not free per-conn */
    conn->ssl = ssl;
    return 0;
}

void net_conn_close(net_conn_t *conn) {
    if (conn->ssl != NULL) {
        SSL_shutdown((SSL *)conn->ssl);
        SSL_free((SSL *)conn->ssl);
        conn->ssl = NULL;
    }
    /* conn->ssl_ctx is the shared g_tls_ctx now; never free it here. */
    conn->ssl_ctx = NULL;
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
