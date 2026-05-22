#include "drm_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zstd.h>

#if defined(__x86_64__) || defined(__i386__)
#  include <emmintrin.h>
#  include <smmintrin.h>
#  define SRVID_HAVE_SSE4 1
#else
#  define SRVID_HAVE_SSE4 0
#endif

#define SRVID_MAGIC  "SRVD"
#define SRVID_VERSION  1u
#define SRVID_PIXFMT_BGRA8888  0u
#define SRVID_ZSTD_LEVEL  1
#define REC_SLOTS  3
#define REC_FWRITE_BUF  (1u << 20)
#define REC_SLOT_ALIGN  64u

struct srapi_drm_record {
    pthread_t thread;
    int thread_started;
    pthread_mutex_t mtx;
    pthread_cond_t cv_have;
    int stop;
    int failed;

    void *slots[REC_SLOTS];
    int count;
    int head;

    uint64_t total;
    uint64_t dropped;

    ZSTD_CCtx *cctx;
    void *cbuf;
    size_t cbuf_cap;
    FILE *fp;
    void *fbuf;
    uint32_t width;
    uint32_t height;
    size_t frame_size;

    uint64_t interval_ns;
    uint64_t next_capture_ns;
};

static uint64_t mono_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#if SRVID_HAVE_SSE4
__attribute__((target("sse4.1")))
static void wc_copy_sse(void *dst, const void *src, size_t n) {
    const __m128i *s = (const __m128i *)src;
    __m128i *d = (__m128i *)dst;
    size_t blocks = n / 64;

    while (blocks--) {
        __m128i a0 = _mm_stream_load_si128((__m128i *)(s + 0));
        __m128i a1 = _mm_stream_load_si128((__m128i *)(s + 1));
        __m128i a2 = _mm_stream_load_si128((__m128i *)(s + 2));
        __m128i a3 = _mm_stream_load_si128((__m128i *)(s + 3));
        _mm_store_si128(d + 0, a0);
        _mm_store_si128(d + 1, a1);
        _mm_store_si128(d + 2, a2);
        _mm_store_si128(d + 3, a3);
        s += 4;
        d += 4;
    }
    _mm_sfence();
    n &= 63;
    if (n) {
        memcpy(d, s, n);
    }
}
#endif

static void fast_wc_copy(void *dst, const void *src, size_t n) {
#if SRVID_HAVE_SSE4
    if ((((uintptr_t)dst | (uintptr_t)src) & 15u) == 0) {
        wc_copy_sse(dst, src, n);
        return;
    }
#endif
    memcpy(dst, src, n);
}

static int write_u32_le(FILE *fp, uint32_t value) {
    uint8_t b[4] = {
        (uint8_t)(value & 0xff),
        (uint8_t)((value >> 8) & 0xff),
        (uint8_t)((value >> 16) & 0xff),
        (uint8_t)((value >> 24) & 0xff),
    };
    return fwrite(b, 1, 4, fp) == 4;
}

static int write_header(FILE *fp, uint32_t width, uint32_t height, uint32_t fps_millihz) {
    uint8_t reserved[8] = {0};

    if (fwrite(SRVID_MAGIC, 1, 4, fp) != 4) {
        return 0;
    }
    if (!write_u32_le(fp, SRVID_VERSION) ||
        !write_u32_le(fp, width) ||
        !write_u32_le(fp, height) ||
        !write_u32_le(fp, fps_millihz) ||
        !write_u32_le(fp, SRVID_PIXFMT_BGRA8888)) {
        return 0;
    }
    if (fwrite(reserved, 1, 8, fp) != 8) {
        return 0;
    }
    return 1;
}

static void *record_worker(void *arg) {
    struct srapi_drm_record *r = arg;
    void *frame_ptr;
    size_t compressed;

    for (;;) {
        pthread_mutex_lock(&r->mtx);
        while (r->count == 0 && !r->stop) {
            pthread_cond_wait(&r->cv_have, &r->mtx);
        }
        if (r->count == 0 && r->stop) {
            pthread_mutex_unlock(&r->mtx);
            break;
        }
        frame_ptr = r->slots[r->head];
        pthread_mutex_unlock(&r->mtx);

        compressed = ZSTD_compressCCtx(
            r->cctx,
            r->cbuf, r->cbuf_cap,
            frame_ptr, r->frame_size,
            SRVID_ZSTD_LEVEL
        );
        if (ZSTD_isError(compressed) || compressed > 0xffffffffu) {
            srapi_debugf("drm record worker: compress failed");
            r->failed = 1;
        } else if (!write_u32_le(r->fp, (uint32_t)compressed) ||
                   fwrite(r->cbuf, 1, compressed, r->fp) != compressed) {
            srapi_debugf("drm record worker: write failed: %s", strerror(errno));
            r->failed = 1;
        }

        pthread_mutex_lock(&r->mtx);
        r->head = (r->head + 1) % REC_SLOTS;
        r->count--;
        if (r->failed) {
            r->stop = 1;
            pthread_mutex_unlock(&r->mtx);
            break;
        }
        pthread_mutex_unlock(&r->mtx);
    }
    return NULL;
}

static void record_free(struct srapi_drm_record *r) {
    if (r == NULL) {
        return;
    }
    if (r->thread_started) {
        pthread_mutex_lock(&r->mtx);
        r->stop = 1;
        pthread_cond_signal(&r->cv_have);
        pthread_mutex_unlock(&r->mtx);
        pthread_join(r->thread, NULL);
    }
    pthread_mutex_destroy(&r->mtx);
    pthread_cond_destroy(&r->cv_have);
    for (int i = 0; i < REC_SLOTS; i++) {
        free(r->slots[i]);
    }
    if (r->cctx != NULL) {
        ZSTD_freeCCtx(r->cctx);
    }
    free(r->cbuf);
    if (r->fp != NULL) {
        fclose(r->fp);
    }
    free(r->fbuf);
    free(r);
}

srapi_result_t srapi_drm_record_start(
    srapi_drm_display_t *display,
    const char *path,
    uint32_t fps_millihz
) {
    struct srapi_drm_record *r;
    uint32_t width;
    uint32_t height;
    size_t raw_size;
    size_t bound;
    int nb_workers;

    if (display == NULL || path == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (display->recorder != NULL) {
        srapi_set_error("drm record: already recording");
        return SRAPI_ERROR;
    }

    width = display->mode.hdisplay;
    height = display->mode.vdisplay;
    if (width == 0 || height == 0) {
        srapi_set_error("drm record: display has no mode set");
        return SRAPI_ERROR;
    }
    if (fps_millihz == 0) {
        fps_millihz = 60000;
    }

    raw_size = (size_t)width * (size_t)height * 4u;
    bound = ZSTD_compressBound(raw_size);
    if (ZSTD_isError(bound)) {
        srapi_set_error("drm record: zstd compressBound failed");
        return SRAPI_ERROR;
    }

    r = calloc(1, sizeof(*r));
    if (r == NULL) {
        srapi_set_error("drm record: out of memory");
        return SRAPI_ERROR_OOM;
    }
    r->width = width;
    r->height = height;
    r->frame_size = raw_size;
    r->interval_ns = 1000000000000ull / (uint64_t)fps_millihz;
    r->next_capture_ns = 0;
    pthread_mutex_init(&r->mtx, NULL);
    pthread_cond_init(&r->cv_have, NULL);

    for (int i = 0; i < REC_SLOTS; i++) {
        if (posix_memalign(&r->slots[i], REC_SLOT_ALIGN, raw_size) != 0) {
            r->slots[i] = NULL;
            srapi_set_error("drm record: aligned alloc failed (%zu)", raw_size);
            record_free(r);
            return SRAPI_ERROR_OOM;
        }
    }

    r->cbuf = malloc(bound);
    if (r->cbuf == NULL) {
        srapi_set_error("drm record: out of memory for compress buffer (%zu)", bound);
        record_free(r);
        return SRAPI_ERROR_OOM;
    }
    r->cbuf_cap = bound;

    r->cctx = ZSTD_createCCtx();
    if (r->cctx == NULL) {
        srapi_set_error("drm record: ZSTD_createCCtx failed");
        record_free(r);
        return SRAPI_ERROR;
    }
    ZSTD_CCtx_setParameter(r->cctx, ZSTD_c_compressionLevel, SRVID_ZSTD_LEVEL);
    nb_workers = 2;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(r->cctx, ZSTD_c_nbWorkers, nb_workers))) {
        srapi_debugf("drm record: zstd workers unavailable, single-thread compress");
    }

    r->fp = fopen(path, "wb");
    if (r->fp == NULL) {
        srapi_set_error("drm record: fopen %s failed: %s", path, strerror(errno));
        record_free(r);
        return SRAPI_ERROR;
    }
    r->fbuf = malloc(REC_FWRITE_BUF);
    if (r->fbuf != NULL) {
        setvbuf(r->fp, r->fbuf, _IOFBF, REC_FWRITE_BUF);
    }
    if (!write_header(r->fp, width, height, fps_millihz)) {
        srapi_set_error("drm record: write header failed: %s", strerror(errno));
        record_free(r);
        return SRAPI_ERROR;
    }

    if (pthread_create(&r->thread, NULL, record_worker, r) != 0) {
        srapi_set_error("drm record: pthread_create failed: %s", strerror(errno));
        record_free(r);
        return SRAPI_ERROR;
    }
    r->thread_started = 1;

    display->recorder = r;
    srapi_debugf("drm record start %s %ux%u fps_mhz=%u slots=%d zstd_lvl=%d workers=%d",
                 path, width, height, fps_millihz, REC_SLOTS, SRVID_ZSTD_LEVEL, nb_workers);
    return SRAPI_OK;
}

void srapi_drm_record_close(srapi_drm_display_t *display) {
    struct srapi_drm_record *r;

    if (display == NULL || display->recorder == NULL) {
        return;
    }
    r = display->recorder;
    display->recorder = NULL;
    srapi_debugf("drm record stop: total=%llu dropped=%llu",
                 (unsigned long long)r->total, (unsigned long long)r->dropped);
    record_free(r);
}

void srapi_drm_record_stop(srapi_drm_display_t *display) {
    srapi_drm_record_close(display);
}

int srapi_drm_is_recording(const srapi_drm_display_t *display) {
    return display != NULL && display->recorder != NULL;
}

void srapi_drm_record_capture(srapi_drm_display_t *display) {
    struct srapi_drm_record *r;
    srapi_drm_buffer_t *buf;
    int slot_idx;
    uint64_t now;

    if (display == NULL || display->paused) {
        return;
    }
    r = display->recorder;
    if (r == NULL) {
        return;
    }

    buf = &display->buffers[display->front];
    if (buf->map == NULL) {
        return;
    }
    if (buf->fb.width != r->width || buf->fb.height != r->height) {
        srapi_debugf("drm record: frame size mismatch, stopping");
        srapi_drm_record_close(display);
        return;
    }

    now = mono_ns();
    if (r->next_capture_ns == 0) {
        r->next_capture_ns = now + r->interval_ns;
    } else if (now < r->next_capture_ns) {
        return;
    } else {
        r->next_capture_ns += r->interval_ns;
        if (r->next_capture_ns < now) {
            r->next_capture_ns = now + r->interval_ns;
        }
    }

    pthread_mutex_lock(&r->mtx);
    if (r->failed) {
        pthread_mutex_unlock(&r->mtx);
        srapi_drm_record_close(display);
        return;
    }
    r->total++;
    if (r->count >= REC_SLOTS) {
        r->dropped++;
        pthread_mutex_unlock(&r->mtx);
        return;
    }
    slot_idx = (r->head + r->count) % REC_SLOTS;
    pthread_mutex_unlock(&r->mtx);

    fast_wc_copy(r->slots[slot_idx], buf->map, r->frame_size);

    pthread_mutex_lock(&r->mtx);
    r->count++;
    pthread_cond_signal(&r->cv_have);
    pthread_mutex_unlock(&r->mtx);
}
