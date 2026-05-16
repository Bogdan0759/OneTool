#include "internal.h"

#include <stdlib.h>

srapi_result_t srapi_create_image_view(
    const srapi_image_view_desc_t *desc,
    srapi_image_view_t **out
) {
    srapi_image_view_t *view;
    uint32_t width;
    uint32_t height;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL || desc->image == NULL) {
        srapi_set_error("image view: bad create args");
        return SRAPI_ERROR_BAD_ARG;
    }

    if (desc->x >= desc->image->width || desc->y >= desc->image->height) {
        srapi_set_error("image view: origin outside image");
        return SRAPI_ERROR_BAD_ARG;
    }
    width = desc->width != 0 ? desc->width : desc->image->width - desc->x;
    height = desc->height != 0 ? desc->height : desc->image->height - desc->y;
    if (width == 0 || height == 0 ||
        width > desc->image->width - desc->x ||
        height > desc->image->height - desc->y) {
        srapi_set_error("image view: region outside image");
        return SRAPI_ERROR_BAD_ARG;
    }

    view = calloc(1, sizeof(*view));
    if (view == NULL) {
        return SRAPI_ERROR_OOM;
    }

    view->image = desc->image;
    view->backend = desc->image->backend;
    view->x = desc->x;
    view->y = desc->y;
    view->width = width;
    view->height = height;

    *out = view;
    srapi_debugf("image view create backend=%s region=%u,%u %ux%u image=%ux%u",
                 srapi_backend_name(view->backend),
                 view->x, view->y, view->width, view->height,
                 view->image->width, view->image->height);
    return SRAPI_OK;
}

void srapi_destroy_image_view(srapi_image_view_t *view) {
    if (view == NULL) {
        return;
    }
    srapi_debugf("image view destroy backend=%s region=%u,%u %ux%u mapped=%d",
                 srapi_backend_name(view->backend), view->x, view->y,
                 view->width, view->height, view->mapped);
    free(view);
}

srapi_image_t *srapi_image_view_image(srapi_image_view_t *view) {
    return view != NULL ? view->image : NULL;
}

uint32_t srapi_image_view_x(const srapi_image_view_t *view) {
    return view != NULL ? view->x : 0;
}

uint32_t srapi_image_view_y(const srapi_image_view_t *view) {
    return view != NULL ? view->y : 0;
}

uint32_t srapi_image_view_width(const srapi_image_view_t *view) {
    return view != NULL ? view->width : 0;
}

uint32_t srapi_image_view_height(const srapi_image_view_t *view) {
    return view != NULL ? view->height : 0;
}

srapi_backend_t srapi_image_view_backend(const srapi_image_view_t *view) {
    return view != NULL ? view->backend : SRAPI_BACKEND_AUTO;
}

srapi_result_t srapi_image_view_map(srapi_image_view_t *view, void **out, uint32_t *pitch) {
    uint8_t *base;

    if (view == NULL || view->image == NULL || out == NULL) {
        srapi_set_error("image view: bad map args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (view->image->tiling != SRAPI_IMAGE_LINEAR) {
        srapi_set_error("image view: optimal tiling is not host-mappable; use queue transfers");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    if (view->image->data == NULL) {
        srapi_set_error("image view: image has no mapped storage");
        return SRAPI_ERROR_UNSUPPORTED;
    }

    view->mapped++;
    view->image->mapped++;
    base = (uint8_t *)view->image->data;
    *out = base + (size_t)view->y * view->image->pitch + (size_t)view->x * sizeof(uint32_t);
    if (pitch != NULL) {
        *pitch = view->image->pitch;
    }
    srapi_debugf("image view map backend=%s mapped=%d image_mapped=%d pitch=%u",
                 srapi_backend_name(view->backend), view->mapped,
                 view->image->mapped, view->image->pitch);
    return SRAPI_OK;
}

void srapi_image_view_unmap(srapi_image_view_t *view) {
    if (view == NULL || view->image == NULL) {
        return;
    }
    if (view->mapped > 0) {
        view->mapped--;
    }
    if (view->image->mapped > 0) {
        view->image->mapped--;
    }
    srapi_debugf("image view unmap mapped=%d image_mapped=%d",
                 view->mapped, view->image->mapped);
}
