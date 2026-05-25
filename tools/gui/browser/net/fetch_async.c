/*
 * Async fetch worker.
 *
 * One worker thread at a time. The mutex protects all shared fields;
 * we never hold it while the actual HTTP fetch is running, so the main
 * thread can call br_net_poll() to peek at the state without blocking
 * on the slow socket I/O.
 *
 * Lifecycle: br_net_start() spawns a fresh joinable thread. The previous
 * thread, if any, is joined first (it must already be done because
 * state != LOADING) so we don't leak.
 */
#include "fetch_async.h"
#include "fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void br_net_init(br_net_t *n) {
    memset(n, 0, sizeof(*n));
    pthread_mutex_init(&n->mtx, NULL);
    n->state = BR_NET_IDLE;
}

void br_net_destroy(br_net_t *n) {
    /* If a fetch is still in-flight, wait for it to finish — we can't
     * cancel a libssl read mid-handshake safely. */
    if (n->have_thread) {
        pthread_join(n->thread, NULL);
        n->have_thread = 0;
    }
    free(n->body);
    free(n->final_url);
    n->body = NULL;
    n->final_url = NULL;
    pthread_mutex_destroy(&n->mtx);
}

static void *worker(void *arg) {
    br_net_t *n = (br_net_t *)arg;

    /* Snapshot the URL under the lock, then unlock for the slow part. */
    char url[BROWSER_URL_MAX];
    pthread_mutex_lock(&n->mtx);
    strncpy(url, n->request_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    pthread_mutex_unlock(&n->mtx);

    char *body = NULL;
    size_t body_len = 0;
    char *final_url = NULL;
    const char *err = NULL;
    int rc = browser_fetch_url(url, &body, &body_len, &final_url, &err);

    pthread_mutex_lock(&n->mtx);
    /* Drop any previously-buffered result. */
    free(n->body);
    free(n->final_url);
    n->body = NULL;
    n->final_url = NULL;
    n->body_len = 0;
    n->err[0] = '\0';
    if (rc == 0) {
        n->body = body;
        n->body_len = body_len;
        n->final_url = final_url;
        n->state = BR_NET_DONE_OK;
    } else {
        if (err != NULL) {
            strncpy(n->err, err, sizeof(n->err) - 1);
            n->err[sizeof(n->err) - 1] = '\0';
        } else {
            strncpy(n->err, "unknown error", sizeof(n->err) - 1);
        }
        free(body);
        free(final_url);
        n->state = BR_NET_DONE_ERR;
    }
    pthread_mutex_unlock(&n->mtx);
    return NULL;
}

int br_net_start(br_net_t *n, const char *url) {
    if (url == NULL || url[0] == '\0') return -1;
    /* Join the previous worker if any. */
    if (n->have_thread) {
        pthread_join(n->thread, NULL);
        n->have_thread = 0;
    }

    pthread_mutex_lock(&n->mtx);
    strncpy(n->request_url, url, sizeof(n->request_url) - 1);
    n->request_url[sizeof(n->request_url) - 1] = '\0';
    n->state = BR_NET_LOADING;
    /* Old result buffers were already either consumed via poll(consume=1)
     * or are stale. Drop them. */
    free(n->body);
    free(n->final_url);
    n->body = NULL;
    n->final_url = NULL;
    n->body_len = 0;
    n->err[0] = '\0';
    pthread_mutex_unlock(&n->mtx);

    if (pthread_create(&n->thread, NULL, worker, n) != 0) {
        pthread_mutex_lock(&n->mtx);
        n->state = BR_NET_DONE_ERR;
        strncpy(n->err, "thread spawn failed", sizeof(n->err) - 1);
        pthread_mutex_unlock(&n->mtx);
        return -1;
    }
    n->have_thread = 1;
    return 0;
}

int br_net_poll(br_net_t *n, int consume,
                char **out_body, size_t *out_len,
                char **out_final, char *err_buf, size_t err_cap) {
    pthread_mutex_lock(&n->mtx);
    int state = n->state;
    if (consume && (state == BR_NET_DONE_OK || state == BR_NET_DONE_ERR)) {
        if (out_body != NULL) { *out_body = n->body; n->body = NULL; }
        else free(n->body);
        if (out_len  != NULL) *out_len = n->body_len;
        if (out_final != NULL) { *out_final = n->final_url; n->final_url = NULL; }
        else { free(n->final_url); }
        if (err_buf != NULL && err_cap > 0) {
            strncpy(err_buf, n->err, err_cap - 1);
            err_buf[err_cap - 1] = '\0';
        }
        n->body_len = 0;
        n->err[0] = '\0';
        n->state = BR_NET_IDLE;
        pthread_mutex_unlock(&n->mtx);
        /* The worker is guaranteed to be done since state was DONE_*; join. */
        if (n->have_thread) {
            pthread_join(n->thread, NULL);
            n->have_thread = 0;
        }
        return state;
    }
    pthread_mutex_unlock(&n->mtx);
    return state;
}
