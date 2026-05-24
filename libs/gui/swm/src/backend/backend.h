#ifndef ONETOOL_LIBS_GUI_SWM_BACKEND_H
#define ONETOOL_LIBS_GUI_SWM_BACKEND_H

#include <srapi/srapi.h>

#include <stdint.h>

typedef struct swm_output swm_output_t;

srapi_result_t swm_output_create_default(swm_output_t **out);
void swm_output_destroy(swm_output_t *output);

uint32_t swm_output_width(const swm_output_t *output);
uint32_t swm_output_height(const swm_output_t *output);
const char *swm_output_device_path(const swm_output_t *output);

srapi_framebuffer_t *swm_output_backbuffer(swm_output_t *output);
srapi_result_t swm_output_present(swm_output_t *output);

srapi_result_t swm_output_record_start(swm_output_t *output, const char *path, uint32_t fps_millihz);
void swm_output_record_stop(swm_output_t *output);
int swm_output_is_recording(const swm_output_t *output);

#endif
