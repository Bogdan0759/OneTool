#ifndef ONETOOL_LIBS_SRAPI_GPU_I915_H
#define ONETOOL_LIBS_SRAPI_GPU_I915_H

#include "../../core/internal.h"

typedef struct {
    int available;
    char path[64];
    uint32_t chipset_id;
    int has_gem;
    int has_execbuf2;
    int has_blt;
    int has_exec_fence;
    int cs_timestamp_frequency;
} srapi_i915_probe_t;

srapi_result_t srapi_i915_query_fd(int fd, const char *path, srapi_i915_probe_t *out);
srapi_result_t srapi_i915_probe_path(const char *path, srapi_i915_probe_t *out);
srapi_result_t srapi_i915_probe_any(srapi_i915_probe_t *out);
srapi_result_t srapi_i915_create_buffer(
    srapi_device_t *device,
    const srapi_buffer_desc_t *desc,
    srapi_buffer_t **out
);
srapi_result_t srapi_i915_create_image(
    srapi_device_t *device,
    const srapi_image_desc_t *desc,
    srapi_image_t **out
);
srapi_result_t srapi_i915_set_tile_cache_enabled(srapi_device_t *device, uint32_t enabled);
uint32_t srapi_i915_tile_cache_enabled(const srapi_device_t *device);
srapi_result_t srapi_i915_submit_noop(srapi_device_t *device);
srapi_result_t srapi_i915_fill_buffer(
    srapi_device_t *device,
    srapi_buffer_t *dst,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t color
);
srapi_result_t srapi_i915_fill_rect_image(
    srapi_device_t *device,
    srapi_image_t *image,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
);
srapi_result_t srapi_i915_fill_image(srapi_device_t *device, srapi_image_t *image, uint32_t color);
srapi_result_t srapi_i915_fill_framebuffer_rect(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
);
srapi_result_t srapi_i915_render_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_cmd_buffer_t *cmd
);
void srapi_i915_destroy_gem(int fd, uint32_t handle);

#endif
