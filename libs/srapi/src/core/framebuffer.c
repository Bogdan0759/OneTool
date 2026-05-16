#include "internal.h"

#include <stdint.h>
#include <stdlib.h>

srapi_result_t srapi_create_framebuffer(
    srapi_context_t *ctx,
    const srapi_framebuffer_desc_t *desc,
    srapi_framebuffer_t **out
) {
    srapi_framebuffer_t *fb;
    uint64_t pixel_count;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL || desc->width == 0 || desc->height == 0) {
        srapi_set_error("framebuffer: bad create args");
        return SRAPI_ERROR_BAD_ARG;
    }

    pixel_count = (uint64_t)desc->width * desc->height;
    if (desc->width > UINT32_MAX / sizeof(uint32_t) ||
        pixel_count > SIZE_MAX / sizeof(uint32_t)) {
        srapi_set_error("framebuffer: size overflow %ux%u", desc->width, desc->height);
        return SRAPI_ERROR_OVERFLOW;
    }

    fb = calloc(1, sizeof(*fb));
    if (fb == NULL) {
        return SRAPI_ERROR_OOM;
    }
    fb->pixels = calloc((size_t)pixel_count, sizeof(uint32_t));
    if (fb->pixels == NULL) {
        free(fb);
        return SRAPI_ERROR_OOM;
    }

    fb->width = desc->width;
    fb->height = desc->height;
    fb->pitch = desc->width * sizeof(uint32_t);
    fb->owns_pixels = 1;
    fb->backend = ctx != NULL ? ctx->backend : SRAPI_BACKEND_CPU;
    *out = fb;
    srapi_debugf("framebuffer create %ux%u pitch=%u backend=%s",
                 fb->width, fb->height, fb->pitch, srapi_backend_name(fb->backend));
    return SRAPI_OK;
}

void srapi_destroy_framebuffer(srapi_framebuffer_t *fb) {
    if (fb == NULL) {
        return;
    }
    srapi_debugf("framebuffer destroy %ux%u pitch=%u owns=%d backend=%s",
                 fb->width, fb->height, fb->pitch, fb->owns_pixels, srapi_backend_name(fb->backend));
    if (fb->owns_pixels) {
        free(fb->pixels);
    }
    free(fb);
}

uint32_t srapi_framebuffer_width(const srapi_framebuffer_t *fb) {
    return fb ? fb->width : 0;
}

uint32_t srapi_framebuffer_height(const srapi_framebuffer_t *fb) {
    return fb ? fb->height : 0;
}

uint32_t srapi_framebuffer_pitch(const srapi_framebuffer_t *fb) {
    return fb ? fb->pitch : 0;
}

uint32_t *srapi_framebuffer_pixels(srapi_framebuffer_t *fb) {
    return fb ? fb->pixels : NULL;
}
