#include "threadpool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#define SRAPI_VM_MAX_WORKERS 63

typedef struct {
    srapi_vm_tile_fn_t fn;
    void *user;
    uint32_t total;
    atomic_uint next;
} pool_job_t;

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_go = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cv_done = PTHREAD_COND_INITIALIZER;
static pool_job_t *current_job = NULL;
static int workers_done = 0;
static int n_workers = 0;
static uint64_t job_seq = 0;

static void *worker_main(void *arg) {
    uint64_t last_seq = 0;
    (void)arg;

    for (;;) {
        pool_job_t *job;
        uint64_t my_seq;

        pthread_mutex_lock(&mu);
        while (current_job == NULL || job_seq == last_seq) {
            pthread_cond_wait(&cv_go, &mu);
        }
        job = current_job;
        my_seq = job_seq;
        pthread_mutex_unlock(&mu);

        for (;;) {
            uint32_t i = atomic_fetch_add_explicit(&job->next, 1, memory_order_relaxed);
            if (i >= job->total) {
                break;
            }
            job->fn(job->user, i);
        }

        pthread_mutex_lock(&mu);
        workers_done++;
        if (workers_done == n_workers) {
            pthread_cond_signal(&cv_done);
        }
        last_seq = my_seq;
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}

static void init_pool(void) {
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    int desired;

    if (online <= 1) {
        n_workers = 0;
        return;
    }
    desired = (int)online - 1;
    if (desired > SRAPI_VM_MAX_WORKERS) {
        desired = SRAPI_VM_MAX_WORKERS;
    }

    for (int i = 0; i < desired; i++) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, worker_main, NULL) != 0) {
            break;
        }
        pthread_detach(tid);
        n_workers++;
    }
}

void srapi_vm_threadpool_dispatch(uint32_t tile_count, srapi_vm_tile_fn_t fn, void *user) {
    if (tile_count == 0 || fn == NULL) {
        return;
    }
    pthread_once(&init_once, init_pool);

    if (n_workers == 0 || tile_count == 1) {
        for (uint32_t i = 0; i < tile_count; i++) {
            fn(user, i);
        }
        return;
    }

    pool_job_t job;
    job.fn = fn;
    job.user = user;
    job.total = tile_count;
    atomic_init(&job.next, 0);

    pthread_mutex_lock(&mu);
    current_job = &job;
    workers_done = 0;
    job_seq++;
    pthread_cond_broadcast(&cv_go);
    pthread_mutex_unlock(&mu);

    for (;;) {
        uint32_t i = atomic_fetch_add_explicit(&job.next, 1, memory_order_relaxed);
        if (i >= job.total) {
            break;
        }
        fn(user, i);
    }

    pthread_mutex_lock(&mu);
    while (workers_done < n_workers) {
        pthread_cond_wait(&cv_done, &mu);
    }
    current_job = NULL;
    pthread_mutex_unlock(&mu);
}
