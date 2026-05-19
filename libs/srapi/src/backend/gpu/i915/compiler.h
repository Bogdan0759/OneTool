#ifndef ONETOOL_LIBS_SRAPI_GPU_I915_COMPILER_H
#define ONETOOL_LIBS_SRAPI_GPU_I915_COMPILER_H

#include "../i915.h"

srapi_result_t srapi_i915_compile_shader(
    srapi_device_t *device,
    srapi_shader_t *shader,
    srapi_buffer_t **out_buffer
);

srapi_result_t srapi_i915_run_shader_gpu(
    srapi_device_t *device,
    srapi_buffer_t *compiled_shader,
    srapi_shader_t *shader,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t dst_handle,
    uint64_t dst_size,
    uint32_t dst_pitch
);

#endif
