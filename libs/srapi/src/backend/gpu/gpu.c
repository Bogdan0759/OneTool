#include "../drm/drm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int try_node(const char *path, srapi_device_info_t *out) {
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        srapi_debugf("gpu probe %s failed: %s", path, strerror(errno));
        return 0;
    }

    srapi_debugf("gpu probe %s ok", path);
    close(fd);
    if (out != NULL) {
        out->backend = SRAPI_BACKEND_GPU;
        out->available = 1;
        snprintf(out->path, sizeof(out->path), "%s", path);
        snprintf(out->message, sizeof(out->message), "gpu drm node available");
    }
    return 1;
}

srapi_result_t srapi_gpu_probe(srapi_device_info_t *out) {
    char path[64];

    if (out != NULL) {
        out->backend = SRAPI_BACKEND_GPU;
    }

    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (try_node(path, out)) {
            return SRAPI_OK;
        }
    }

    for (int i = 128; i < 136; i++) {
        snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
        if (try_node(path, out)) {
            return SRAPI_OK;
        }
    }

    if (out != NULL) {
        snprintf(out->message, sizeof(out->message), "no usable drm gpu node");
    }
    srapi_set_error("gpu: no usable /dev/dri/card0..7 or /dev/dri/renderD128..135 node");
    return SRAPI_ERROR_UNSUPPORTED;
}

srapi_result_t srapi_gpu_open_device(const srapi_device_desc_t *desc, srapi_device_t **out) {
    srapi_device_info_t info;
    srapi_device_t *device;
    const char *path;
    int fd;

    if (desc == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    if (desc->device_path != NULL && desc->device_path[0] != '\0') {
        path = desc->device_path;
    } else {
        if (srapi_gpu_probe(&info) != SRAPI_OK) {
            return SRAPI_ERROR_UNSUPPORTED;
        }
        path = info.path;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        srapi_set_error("gpu: open %s failed: %s", path, strerror(errno));
        return SRAPI_ERROR;
    }

    device = calloc(1, sizeof(*device));
    if (device == NULL) {
        close(fd);
        return SRAPI_ERROR_OOM;
    }

    device->backend = SRAPI_BACKEND_GPU;
    device->fd = fd;
    snprintf(device->path, sizeof(device->path), "%s", path);
    *out = device;
    srapi_debugf("gpu device open path=%s fd=%d", device->path, device->fd);
    return SRAPI_OK;
}

void srapi_gpu_close_device(srapi_device_t *device) {
    if (device == NULL) {
        return;
    }
    if (device->fd >= 0) {
        srapi_debugf("gpu device close path=%s fd=%d", device->path, device->fd);
        close(device->fd);
        device->fd = -1;
    }
    free(device);
}

srapi_result_t srapi_gpu_create_buffer(
    srapi_device_t *device,
    const srapi_buffer_desc_t *desc,
    srapi_buffer_t **out
) {
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    srapi_buffer_t *buffer;
    uint32_t width;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL || desc->size == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    width = (uint32_t)((desc->size + 3u) / 4u);
    if ((size_t)width * 4u < desc->size || width == 0) {
        return SRAPI_ERROR_OVERFLOW;
    }

    memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = 1;
    create.bpp = 32;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        srapi_set_error("gpu: CREATE_DUMB buffer size=%zu failed on %s: %s",
                        desc->size, device->path, strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("gpu: MAP_DUMB handle=%u failed: %s", create.handle, strerror(errno));
        srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR_OOM;
    }

    buffer->data = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, device->fd, (off_t)map.offset);
    if (buffer->data == MAP_FAILED) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("gpu: mmap dumb buffer failed: %s", strerror(errno));
        buffer->data = NULL;
        free(buffer);
        srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }

    buffer->device = device;
    buffer->backend = SRAPI_BACKEND_GPU;
    buffer->size = desc->size;
    buffer->usage = desc->usage;
    buffer->gpu_handle = create.handle;
    buffer->gpu_size = create.size;
    if (desc->initial_data != NULL) {
        memcpy(buffer->data, desc->initial_data, desc->size);
    }

    *out = buffer;
    srapi_debugf("gpu buffer create path=%s handle=%u size=%zu alloc=%llu pitch=%u usage=0x%x",
                 device->path, buffer->gpu_handle, buffer->size,
                 (unsigned long long)buffer->gpu_size, create.pitch, buffer->usage);
    return SRAPI_OK;
}

srapi_result_t srapi_gpu_create_image(
    srapi_device_t *device,
    const srapi_image_desc_t *desc,
    srapi_image_t **out
) {
    struct drm_mode_create_dumb create;
    struct drm_mode_map_dumb map;
    srapi_image_t *image;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL ||
        desc->width == 0 || desc->height == 0 || device->fd < 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    if (desc->tiling == SRAPI_IMAGE_OPTIMAL) {
        srapi_debugf("gpu image optimal requested %ux%u usage=0x%x", desc->width, desc->height, desc->usage);
        srapi_set_error("gpu: optimal image storage is not implemented in SRAPI backend yet");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    if (desc->tiling != SRAPI_IMAGE_LINEAR) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&create, 0, sizeof(create));
    create.width = desc->width;
    create.height = desc->height;
    create.bpp = 32;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        srapi_set_error("gpu: CREATE_DUMB image %ux%u failed on %s: %s",
                        desc->width, desc->height, device->path, strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("gpu: MAP_DUMB image handle=%u failed: %s", create.handle, strerror(errno));
        srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }

    image = calloc(1, sizeof(*image));
    if (image == NULL) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR_OOM;
    }

    image->data = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, device->fd, (off_t)map.offset);
    if (image->data == MAP_FAILED) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("gpu: mmap image failed: %s", strerror(errno));
        image->data = NULL;
        free(image);
        srapi_drm_ioctl(device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }

    image->device = device;
    image->backend = SRAPI_BACKEND_GPU;
    image->width = desc->width;
    image->height = desc->height;
    image->pitch = create.pitch;
    image->tiling = SRAPI_IMAGE_LINEAR;
    image->usage = desc->usage;
    image->gpu_handle = create.handle;
    image->gpu_size = create.size;
    if (desc->initial_pixels != NULL) {
        for (uint32_t y = 0; y < image->height; y++) {
            memcpy((uint8_t *)image->data + (size_t)y * image->pitch,
                   (const uint8_t *)desc->initial_pixels + (size_t)y * image->width * sizeof(uint32_t),
                   (size_t)image->width * sizeof(uint32_t));
        }
    }

    *out = image;
    srapi_debugf("gpu image create linear path=%s handle=%u %ux%u pitch=%u alloc=%llu usage=0x%x",
                 device->path, image->gpu_handle, image->width, image->height,
                 image->pitch, (unsigned long long)image->gpu_size, image->usage);
    return SRAPI_OK;
}

srapi_result_t srapi_gpu_create_queue(const srapi_queue_desc_t *desc, srapi_queue_t **out) {
    srapi_queue_t *queue;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || desc->device == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_debugf("gpu queue create requested family=%u device=%s",
                 desc->family_index, srapi_device_path(desc->device));
    queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        return SRAPI_ERROR_OOM;
    }

    queue->device = desc->device;
    queue->backend = SRAPI_BACKEND_GPU;
    queue->family_index = desc->family_index;
    *out = queue;
    return SRAPI_OK;
}
