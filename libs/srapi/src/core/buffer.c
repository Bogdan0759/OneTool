#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int range_ok(size_t total, size_t offset, size_t size) {
    return offset <= total && size <= total - offset;
}

srapi_result_t srapi_create_buffer(
    srapi_device_t *device,
    const srapi_buffer_desc_t *desc,
    srapi_buffer_t **out
) {
    srapi_buffer_t *buffer;

    if (out != NULL) {
        *out = NULL;
    }
    if (device == NULL || desc == NULL || out == NULL || desc->size == 0) {
        srapi_set_error("buffer: bad create args");
        return SRAPI_ERROR_BAD_ARG;
    }

    if (device->backend == SRAPI_BACKEND_GPU) {
        return srapi_gpu_create_buffer(device, desc, out);
    }
    if (device->backend != SRAPI_BACKEND_CPU) {
        srapi_set_error("buffer: unsupported backend=%s", srapi_backend_name(device->backend));
        return SRAPI_ERROR_BAD_ARG;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (buffer == NULL) {
        return SRAPI_ERROR_OOM;
    }

    buffer->data = calloc(1, desc->size);
    if (buffer->data == NULL) {
        free(buffer);
        return SRAPI_ERROR_OOM;
    }

    buffer->device = device;
    buffer->backend = device->backend;
    buffer->size = desc->size;
    buffer->usage = desc->usage;
    if (desc->initial_data != NULL) {
        memcpy(buffer->data, desc->initial_data, desc->size);
    }

    *out = buffer;
    srapi_debugf("buffer create backend=%s size=%zu usage=0x%x",
                 srapi_backend_name(buffer->backend), buffer->size, buffer->usage);
    return SRAPI_OK;
}

void srapi_destroy_buffer(srapi_buffer_t *buffer) {
    if (buffer == NULL) {
        return;
    }

    srapi_debugf("buffer destroy backend=%s size=%zu usage=0x%x mapped=%d",
                 srapi_backend_name(buffer->backend), buffer->size, buffer->usage, buffer->mapped);
    if (buffer->backend == SRAPI_BACKEND_GPU) {
        srapi_gpu_destroy_buffer(buffer);
    } else {
        free(buffer->data);
    }
    free(buffer);
}

size_t srapi_buffer_size(const srapi_buffer_t *buffer) {
    return buffer != NULL ? buffer->size : 0;
}

uint32_t srapi_buffer_usage(const srapi_buffer_t *buffer) {
    return buffer != NULL ? buffer->usage : 0;
}

srapi_backend_t srapi_buffer_backend(const srapi_buffer_t *buffer) {
    return buffer != NULL ? buffer->backend : SRAPI_BACKEND_AUTO;
}

srapi_result_t srapi_buffer_write(srapi_buffer_t *buffer, size_t offset, const void *data, size_t size) {
    if (buffer == NULL || data == NULL || !range_ok(buffer->size, offset, size)) {
        srapi_set_error("buffer: bad write args offset=%zu size=%zu", offset, size);
        return SRAPI_ERROR_BAD_ARG;
    }
    if (buffer->backend != SRAPI_BACKEND_CPU && buffer->backend != SRAPI_BACKEND_GPU) {
        srapi_set_error("buffer: write unsupported for backend=%s", srapi_backend_name(buffer->backend));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memcpy((uint8_t *)buffer->data + offset, data, size);
    if (buffer->backend == SRAPI_BACKEND_GPU &&
        buffer->device != NULL &&
        buffer->device->gpu_driver == 915 &&
        buffer->gpu_memory == 1) {
        srapi_i915_set_domain(buffer->device, buffer, SRAPI_I915_DOMAIN_CPU, SRAPI_I915_DOMAIN_CPU);
    }
    srapi_debugf("buffer write offset=%zu size=%zu", offset, size);
    return SRAPI_OK;
}

srapi_result_t srapi_buffer_read(const srapi_buffer_t *buffer, size_t offset, void *out, size_t size) {
    if (buffer == NULL || out == NULL || !range_ok(buffer->size, offset, size)) {
        srapi_set_error("buffer: bad read args offset=%zu size=%zu", offset, size);
        return SRAPI_ERROR_BAD_ARG;
    }
    if (buffer->backend != SRAPI_BACKEND_CPU && buffer->backend != SRAPI_BACKEND_GPU) {
        srapi_set_error("buffer: read unsupported for backend=%s", srapi_backend_name(buffer->backend));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    if (buffer->backend == SRAPI_BACKEND_GPU &&
        buffer->device != NULL &&
        buffer->device->gpu_driver == 915 &&
        buffer->gpu_memory == 1) {
        srapi_i915_set_domain(buffer->device, (srapi_buffer_t *)buffer, SRAPI_I915_DOMAIN_CPU, 0);
    }
    memcpy(out, (const uint8_t *)buffer->data + offset, size);
    srapi_debugf("buffer read offset=%zu size=%zu", offset, size);
    return SRAPI_OK;
}

srapi_result_t srapi_buffer_map(srapi_buffer_t *buffer, void **out) {
    if (buffer == NULL || out == NULL) {
        srapi_set_error("buffer: bad map args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (buffer->backend != SRAPI_BACKEND_CPU && buffer->backend != SRAPI_BACKEND_GPU) {
        srapi_set_error("buffer: map unsupported for backend=%s", srapi_backend_name(buffer->backend));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    buffer->mapped++;
    *out = buffer->data;
    srapi_debugf("buffer map mapped=%d", buffer->mapped);
    return SRAPI_OK;
}

void srapi_buffer_unmap(srapi_buffer_t *buffer) {
    if (buffer == NULL) {
        return;
    }
    if (buffer->mapped > 0) {
        buffer->mapped--;
    }
    srapi_debugf("buffer unmap mapped=%d", buffer->mapped);
}
