#ifndef ONETOOL_LIBS_SRAPI_GPU_I915_RENDER3D_H
#define ONETOOL_LIBS_SRAPI_GPU_I915_RENDER3D_H

#include "../i915.h"

srapi_result_t srapi_i915_render3d_shade_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
);

srapi_result_t srapi_i915_render3d_shade_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
);

srapi_result_t srapi_i915_render3d_line_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
);

srapi_result_t srapi_i915_render3d_triangle_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
);

#endif
