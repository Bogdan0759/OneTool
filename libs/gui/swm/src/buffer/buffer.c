#define _GNU_SOURCE
#include "buffer.h"

#include <sprot/sprot.h>

#include <errno.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct swm_buffer {
    uint32_t kind;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t size;
    int fd;
    void *map;
};

static int sync_dmabuf(int fd, uint64_t flags) {
    struct dma_buf_sync sync = { .flags = flags };
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

swm_buffer_t *swm_buffer_create(uint32_t kind, int fd, uint32_t width, uint32_t height, uint32_t stride, size_t size) {
    swm_buffer_t *buffer;

    if ((kind != SPROT_BUFFER_SHM && kind != SPROT_BUFFER_DMABUF) ||
        fd < 0 || width == 0 || height == 0 || stride == 0 || size == 0) {
        return NULL;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) return NULL;

    buffer->map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (buffer->map == MAP_FAILED) {
        free(buffer);
        return NULL;
    }

    buffer->kind = kind;
    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    buffer->size = size;
    buffer->fd = fd;
    return buffer;
}

void swm_buffer_destroy(swm_buffer_t *buffer) {
    if (buffer == NULL) return;
    if (buffer->map != NULL && buffer->map != MAP_FAILED) {
        munmap(buffer->map, buffer->size);
    }
    if (buffer->fd >= 0) close(buffer->fd);
    free(buffer);
}

uint32_t swm_buffer_kind(const swm_buffer_t *buffer) {
    return buffer != NULL ? buffer->kind : 0;
}

uint32_t swm_buffer_width(const swm_buffer_t *buffer) {
    return buffer != NULL ? buffer->width : 0;
}

uint32_t swm_buffer_height(const swm_buffer_t *buffer) {
    return buffer != NULL ? buffer->height : 0;
}

uint32_t swm_buffer_stride(const swm_buffer_t *buffer) {
    return buffer != NULL ? buffer->stride : 0;
}

size_t swm_buffer_size(const swm_buffer_t *buffer) {
    return buffer != NULL ? buffer->size : 0;
}

const void *swm_buffer_pixels(const swm_buffer_t *buffer) {
    return buffer != NULL ? buffer->map : NULL;
}

int swm_buffer_begin_cpu_read(swm_buffer_t *buffer) {
    if (buffer == NULL) return -1;
    if (buffer->kind != SPROT_BUFFER_DMABUF) return 0;
    return sync_dmabuf(buffer->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}

void swm_buffer_end_cpu_read(swm_buffer_t *buffer) {
    if (buffer == NULL || buffer->kind != SPROT_BUFFER_DMABUF) return;
    sync_dmabuf(buffer->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}
