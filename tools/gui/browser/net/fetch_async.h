#ifndef ONETOOL_TOOLS_GUI_BROWSER_NET_FETCH_ASYNC_H
#define ONETOOL_TOOLS_GUI_BROWSER_NET_FETCH_ASYNC_H

#include <pthread.h>
#include <stddef.h>

#include "../browser.h"

typedef enum {
    BR_NET_IDLE = 0,
    BR_NET_LOADING,
    BR_NET_DONE_OK,
    BR_NET_DONE_ERR,
} br_net_state_t;

/*
 * Asynchronous URL fetcher.
 *
 * br_net_start() spawns a worker thread that calls browser_fetch_url()
 * for the given URL. The main loop polls br_net_poll() each frame; when
 * it returns BR_NET_DONE_OK / BR_NET_DONE_ERR, the result fields are
 * filled in and the fetcher is ready for the next request.
 *
 * Only one fetch can be in-flight at a time. Calling start() while a
 * fetch is loading cancels the previous result (it's overwritten when
 * the new fetch lands).
 */
typedef struct {
    pthread_t       thread;
    pthread_mutex_t mtx;
    int             have_thread;

    /* Producer (worker) -> consumer (main loop). Guarded by mtx. */
    int             state;            /* br_net_state_t */
    char            request_url[BROWSER_URL_MAX];
    char           *body;             /* malloc'd, transferred to consumer */
    size_t          body_len;
    char           *final_url;        /* malloc'd; may be NULL */
    char            err[128];
} br_net_t;

void br_net_init(br_net_t *n);
void br_net_destroy(br_net_t *n);

/* Start a fetch. Returns 0 on success, -1 if the URL is empty / fetcher
 * thread couldn't be started. If a fetch was already loading, its result
 * is discarded. */
int  br_net_start(br_net_t *n, const char *url);

/* Non-blocking poll. Returns current state. If the state is
 * BR_NET_DONE_OK / BR_NET_DONE_ERR and 'consume' is non-zero, transfers
 * ownership of the body/final_url buffers to the caller (which must free
 * them) and resets the state to BR_NET_IDLE. */
int  br_net_poll(br_net_t *n, int consume,
                 char **out_body, size_t *out_len,
                 char **out_final, char *err_buf, size_t err_cap);

#endif
