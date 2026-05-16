#include "../backend/drm/drm_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int image_size(uint32_t width, uint32_t height, uint32_t *pitch, size_t *size) {
    uint64_t p = (uint64_t)width * sizeof(uint32_t);
    uint64_t total = p * height;

    if (width == 0 || height == 0 || p > UINT32_MAX || total > SIZE_MAX) {
        return 0;
    }

    *pitch = (uint32_t)p;
    *size = (size_t)total;
    return 1;
}

srapi_result_t srapi_create_image(
    srapi_device_t *device,
    const srapi_image_desc_t *desc,
    srapi_image_t **out
) {
    srapi_image_t *image;
    uint32_t pitch;
    size_t size;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL ||
        desc->width == 0 || desc->height == 0) {
        srapi_set_error("image: bad create args");
        return SRAPI_ERROR_BAD_ARG;
    }

    if (desc->tiling != SRAPI_IMAGE_LINEAR && desc->tiling != SRAPI_IMAGE_OPTIMAL) {
        srapi_set_error("image: bad tiling=%u", desc->tiling);
        return SRAPI_ERROR_BAD_ARG;
    }

    if (device->backend == SRAPI_BACKEND_GPU) {
        return srapi_gpu_create_image(device, desc, out);
    }
    if (device->backend != SRAPI_BACKEND_CPU) {
        srapi_set_error("image: unsupported backend=%s", srapi_backend_name(device->backend));
        return SRAPI_ERROR_BAD_ARG;
    }
    if (!image_size(desc->width, desc->height, &pitch, &size)) {
        srapi_set_error("image: size overflow %ux%u", desc->width, desc->height);
        return SRAPI_ERROR_OVERFLOW;
    }

    image = calloc(1, sizeof(*image));
    if (image == NULL) {
        return SRAPI_ERROR_OOM;
    }

    image->data = calloc(1, size);
    if (image->data == NULL) {
        free(image);
        return SRAPI_ERROR_OOM;
    }

    image->device = device;
    image->backend = SRAPI_BACKEND_CPU;
    image->width = desc->width;
    image->height = desc->height;
    image->pitch = pitch;
    image->tiling = desc->tiling;
    image->usage = desc->usage;
    image->gpu_size = size;

    if (desc->initial_pixels != NULL) {
        memcpy(image->data, desc->initial_pixels, size);
    }

    *out = image;
    srapi_debugf("image create backend=cpu %ux%u pitch=%u tiling=%u usage=0x%x size=%zu",
                 image->width, image->height, image->pitch, image->tiling, image->usage, size);
    return SRAPI_OK;
}

void srapi_destroy_image(srapi_image_t *image) {
    if (image == NULL) {
        return;
    }

    srapi_debugf("image destroy backend=%s %ux%u pitch=%u tiling=%u usage=0x%x mapped=%d",
                 srapi_backend_name(image->backend), image->width, image->height,
                 image->pitch, image->tiling, image->usage, image->mapped);

    if (image->backend == SRAPI_BACKEND_GPU) {
        if (image->data != NULL && image->gpu_size > 0) {
            munmap(image->data, image->gpu_size);
        }
        if (image->device != NULL && image->device->fd >= 0 && image->gpu_handle != 0) {
            struct drm_mode_destroy_dumb destroy = { .handle = image->gpu_handle };
            srapi_debugf("gpu image destroy handle=%u alloc=%llu",
                         image->gpu_handle, (unsigned long long)image->gpu_size);
            srapi_drm_ioctl(image->device->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
    } else {
        free(image->data);
    }
    free(image);
}

uint32_t srapi_image_width(const srapi_image_t *image) {
    return image != NULL ? image->width : 0;
}

uint32_t srapi_image_height(const srapi_image_t *image) {
    return image != NULL ? image->height : 0;
}

uint32_t srapi_image_pitch(const srapi_image_t *image) {
    return image != NULL ? image->pitch : 0;
}

srapi_image_tiling_t srapi_image_tiling(const srapi_image_t *image) {
    return image != NULL ? image->tiling : 0;
}

uint32_t srapi_image_usage(const srapi_image_t *image) {
    return image != NULL ? image->usage : 0;
}

srapi_backend_t srapi_image_backend(const srapi_image_t *image) {
    return image != NULL ? image->backend : SRAPI_BACKEND_AUTO;
}

srapi_result_t srapi_image_map(srapi_image_t *image, void **out, uint32_t *pitch) {
    if (image == NULL || out == NULL) {
        srapi_set_error("image: bad map args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (image->tiling != SRAPI_IMAGE_LINEAR) {
        srapi_set_error("image: optimal tiling is not host-mappable; use queue transfers");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    if (image->data == NULL) {
        srapi_set_error("image: no mapped storage");
        return SRAPI_ERROR_UNSUPPORTED;
    }

    image->mapped++;
    *out = image->data;
    if (pitch != NULL) {
        *pitch = image->pitch;
    }
    srapi_debugf("image map backend=%s mapped=%d pitch=%u",
                 srapi_backend_name(image->backend), image->mapped, image->pitch);
    return SRAPI_OK;
}

void srapi_image_unmap(srapi_image_t *image) {
    if (image == NULL) {
        return;
    }
    if (image->mapped > 0) {
        image->mapped--;
    }
    srapi_debugf("image unmap mapped=%d", image->mapped);
}
