#include "vm.h"
#include "../../../core/internal.h"

srapi_result_t srapi_i915_vm_shade_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    (void)device;

    if (target == NULL || target->data == NULL || op == NULL || op->shader == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_debugf("i915 vm shade image %u,%u %ux%u (rect %d,%d %ux%u)",
                 x, y, width, height,
                 op->x0, op->y0, op->width, op->height);
    return srapi_vm_shade_rect(
        target->data, target->pitch, op->shader,
        op->x0, op->y0, op->width, op->height,
        x, y, width, height
    );
}

srapi_result_t srapi_i915_vm_shade_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    (void)device;

    if (target == NULL || target->pixels == NULL || op == NULL || op->shader == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_debugf("i915 vm shade framebuffer %u,%u %ux%u (rect %d,%d %ux%u)",
                 x, y, width, height,
                 op->x0, op->y0, op->width, op->height);
    return srapi_vm_shade_rect(
        target->pixels, target->pitch, op->shader,
        op->x0, op->y0, op->width, op->height,
        x, y, width, height
    );
}
