#ifndef ONETOOL_LIBS_SRAPI_GPU_I915_VM_H
#define ONETOOL_LIBS_SRAPI_GPU_I915_VM_H

#include "../i915.h"

srapi_result_t srapi_i915_vm_shade_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);

srapi_result_t srapi_i915_vm_shade_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
);

#endif
