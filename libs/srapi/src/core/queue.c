#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int copy_region_ok(
    const srapi_buffer_t *buffer,
    const srapi_image_t *image,
    const srapi_buffer_image_copy_t *region
) {
    size_t row_bytes;
    size_t copy_bytes;
    size_t buffer_need;

    if (buffer == NULL || image == NULL || region == NULL ||
        region->width == 0 || region->height == 0) {
        srapi_set_error("queue copy: bad region args");
        return 0;
    }
    if (region->image_x > image->width || region->image_y > image->height ||
        region->width > image->width - region->image_x ||
        region->height > image->height - region->image_y) {
        srapi_set_error("queue copy: region outside image");
        return 0;
    }

    if ((size_t)region->width > SIZE_MAX / sizeof(uint32_t)) {
        srapi_set_error("queue copy: row size overflow");
        return 0;
    }
    row_bytes = (size_t)region->width * sizeof(uint32_t);
    if (region->height > SIZE_MAX / row_bytes) {
        srapi_set_error("queue copy: copy size overflow");
        return 0;
    }
    copy_bytes = row_bytes * (size_t)region->height;
    if (region->buffer_offset > SIZE_MAX - copy_bytes) {
        srapi_set_error("queue copy: buffer offset overflow");
        return 0;
    }
    buffer_need = region->buffer_offset + copy_bytes;
    if (buffer_need > buffer->size) {
        srapi_set_error("queue copy: region outside buffer");
        return 0;
    }
    return 1;
}

static srapi_result_t check_copy_args(
    srapi_queue_t *queue,
    srapi_buffer_t *buffer,
    srapi_image_t *image,
    const srapi_buffer_image_copy_t *region,
    uint32_t buffer_usage,
    uint32_t image_usage
) {
    if (queue == NULL || buffer == NULL || image == NULL || region == NULL) {
        srapi_set_error("queue copy: bad args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (buffer->device != queue->device || image->device != queue->device) {
        srapi_set_error("queue copy: resources belong to a different device");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (image->data == NULL) {
        srapi_set_error("queue copy: image has no mapped storage");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    if ((buffer->usage & buffer_usage) == 0) {
        srapi_set_error("queue copy: buffer missing usage 0x%x", buffer_usage);
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((image->usage & image_usage) == 0) {
        srapi_set_error("queue copy: image missing usage 0x%x", image_usage);
        return SRAPI_ERROR_BAD_ARG;
    }
    if (!copy_region_ok(buffer, image, region)) {
        return SRAPI_ERROR_BAD_ARG;
    }
    return SRAPI_OK;
}

srapi_result_t srapi_create_queue(const srapi_queue_desc_t *desc, srapi_queue_t **out) {
    srapi_queue_t *queue;

    if (desc == NULL || desc->device == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    if (desc->device->backend == SRAPI_BACKEND_GPU) {
        return srapi_gpu_create_queue(desc, out);
    }
    if (desc->device->backend != SRAPI_BACKEND_CPU) {
        return SRAPI_ERROR_BAD_ARG;
    }

    queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        return SRAPI_ERROR_OOM;
    }

    queue->device = desc->device;
    queue->backend = desc->device->backend;
    queue->family_index = desc->family_index;
    *out = queue;

    srapi_debugf("queue create backend=%s family=%u",
                 srapi_backend_name(queue->backend), queue->family_index);
    return SRAPI_OK;
}

void srapi_destroy_queue(srapi_queue_t *queue) {
    if (queue == NULL) {
        return;
    }

    srapi_debugf("queue destroy backend=%s family=%u",
                 srapi_backend_name(queue->backend), queue->family_index);
    free(queue);
}

srapi_device_t *srapi_queue_device(srapi_queue_t *queue) {
    return queue != NULL ? queue->device : NULL;
}

srapi_result_t srapi_queue_submit(
    srapi_queue_t *queue,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
) {
    if (queue == NULL || target == NULL || cmd == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (queue->backend != SRAPI_BACKEND_CPU && queue->backend != SRAPI_BACKEND_GPU) {
        srapi_set_error("queue: submit unsupported for backend=%s", srapi_backend_name(queue->backend));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    srapi_debugf("queue submit backend=%s family=%u commands=%zu",
                 srapi_backend_name(queue->backend), queue->family_index, cmd->count);
    if (target->backend != queue->backend) {
        srapi_set_error("queue: target backend=%s does not match queue backend=%s",
                        srapi_backend_name(target->backend), srapi_backend_name(queue->backend));
        return SRAPI_ERROR_BAD_ARG;
    }

    return srapi_submit(NULL, target, cmd);
}

srapi_result_t srapi_queue_copy_buffer_to_image(
    srapi_queue_t *queue,
    srapi_buffer_t *src,
    srapi_image_t *dst,
    const srapi_buffer_image_copy_t *region
) {
    srapi_result_t r = check_copy_args(
        queue, src, dst, region,
        SRAPI_BUFFER_TRANSFER_SRC,
        SRAPI_IMAGE_TRANSFER_DST
    );
    uint64_t row_bytes;

    if (r != SRAPI_OK) {
        return r;
    }
    if (src->data == NULL || dst->data == NULL) {
        srapi_set_error("queue copy: source or destination has no mapped storage");
        return SRAPI_ERROR_BAD_ARG;
    }

    row_bytes = (uint64_t)region->width * sizeof(uint32_t);
    for (uint32_t y = 0; y < region->height; y++) {
        memcpy((uint8_t *)dst->data + (size_t)(region->image_y + y) * dst->pitch + (size_t)region->image_x * sizeof(uint32_t),
               (const uint8_t *)src->data + region->buffer_offset + (size_t)y * row_bytes,
               (size_t)row_bytes);
    }

    srapi_debugf("queue copy buffer->image backend=%s offset=%zu image=%u,%u size=%ux%u",
                 srapi_backend_name(queue->backend), region->buffer_offset,
                 region->image_x, region->image_y, region->width, region->height);
    return SRAPI_OK;
}

srapi_result_t srapi_queue_copy_image_to_buffer(
    srapi_queue_t *queue,
    srapi_image_t *src,
    srapi_buffer_t *dst,
    const srapi_buffer_image_copy_t *region
) {
    srapi_result_t r = check_copy_args(
        queue, dst, src, region,
        SRAPI_BUFFER_TRANSFER_DST,
        SRAPI_IMAGE_TRANSFER_SRC
    );
    uint64_t row_bytes;

    if (r != SRAPI_OK) {
        return r;
    }
    if (src->data == NULL || dst->data == NULL) {
        srapi_set_error("queue copy: source or destination has no mapped storage");
        return SRAPI_ERROR_BAD_ARG;
    }

    row_bytes = (uint64_t)region->width * sizeof(uint32_t);
    for (uint32_t y = 0; y < region->height; y++) {
        memcpy((uint8_t *)dst->data + region->buffer_offset + (size_t)y * row_bytes,
               (const uint8_t *)src->data + (size_t)(region->image_y + y) * src->pitch + (size_t)region->image_x * sizeof(uint32_t),
               (size_t)row_bytes);
    }

    srapi_debugf("queue copy image->buffer backend=%s image=%u,%u size=%ux%u offset=%zu",
                 srapi_backend_name(queue->backend),
                 region->image_x, region->image_y, region->width, region->height,
                 region->buffer_offset);
    return SRAPI_OK;
}
