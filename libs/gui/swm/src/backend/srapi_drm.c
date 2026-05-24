#include "backend.h"

#include <stdlib.h>

struct swm_output {
    srapi_drm_display_t *drm;
};

srapi_result_t swm_output_create_default(swm_output_t **out) {
    srapi_drm_recommendation_t rec;
    swm_output_t *output;

    if (out == NULL) return SRAPI_ERROR_BAD_ARG;
    *out = NULL;

    if (srapi_drm_recommend(&rec) != SRAPI_OK) {
        return SRAPI_ERROR;
    }

    output = calloc(1, sizeof(*output));
    if (output == NULL) {
        return SRAPI_ERROR_OOM;
    }
    if (srapi_drm_open_display(&(srapi_drm_display_desc_t){ .device_path = rec.path }, &output->drm) != SRAPI_OK) {
        free(output);
        return SRAPI_ERROR;
    }

    *out = output;
    return SRAPI_OK;
}

void swm_output_destroy(swm_output_t *output) {
    if (output == NULL) return;
    if (output->drm != NULL) srapi_drm_close(output->drm);
    free(output);
}

uint32_t swm_output_width(const swm_output_t *output) {
    return output != NULL && output->drm != NULL ? srapi_drm_width(output->drm) : 0;
}

uint32_t swm_output_height(const swm_output_t *output) {
    return output != NULL && output->drm != NULL ? srapi_drm_height(output->drm) : 0;
}

const char *swm_output_device_path(const swm_output_t *output) {
    return output != NULL && output->drm != NULL ? srapi_drm_device_path(output->drm) : NULL;
}

srapi_framebuffer_t *swm_output_backbuffer(swm_output_t *output) {
    return output != NULL && output->drm != NULL ? srapi_drm_backbuffer(output->drm) : NULL;
}

srapi_result_t swm_output_present(swm_output_t *output) {
    if (output == NULL || output->drm == NULL) return SRAPI_ERROR_BAD_ARG;
    return srapi_drm_present(output->drm);
}

srapi_result_t swm_output_record_start(swm_output_t *output, const char *path, uint32_t fps_millihz) {
    if (output == NULL || output->drm == NULL) return SRAPI_ERROR_BAD_ARG;
    return srapi_drm_record_start(output->drm, path, fps_millihz);
}

void swm_output_record_stop(swm_output_t *output) {
    if (output == NULL || output->drm == NULL) return;
    srapi_drm_record_stop(output->drm);
}

int swm_output_is_recording(const swm_output_t *output) {
    return output != NULL && output->drm != NULL && srapi_drm_is_recording(output->drm);
}
