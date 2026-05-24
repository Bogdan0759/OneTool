#include "render3d.h"
#include "vm.h"
#include <stdlib.h>
#include <string.h>

#include <drm/i915_drm.h>
#include <sys/ioctl.h>
#include <errno.h>

static int drm_gem_wait_handle(int fd, uint32_t handle, int64_t timeout_ns) {
    struct drm_i915_gem_wait w;
    memset(&w, 0, sizeof(w));
    w.bo_handle = handle;
    w.timeout_ns = timeout_ns;
    return ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, &w);
}

static int clip_rect(
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    int32_t clip_x,
    int32_t clip_y,
    uint32_t clip_width,
    uint32_t clip_height,
    uint32_t *out_x,
    uint32_t *out_y,
    uint32_t *out_width,
    uint32_t *out_height
) {
    int64_t x0 = x;
    int64_t y0 = y;
    int64_t x1 = (int64_t)x + width;
    int64_t y1 = (int64_t)y + height;
    int64_t cx0 = clip_x;
    int64_t cy0 = clip_y;
    int64_t cx1 = (int64_t)clip_x + clip_width;
    int64_t cy1 = (int64_t)clip_y + clip_height;

    if (width == 0 || height == 0 || clip_width == 0 || clip_height == 0) {
        return 0;
    }
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    *out_x = (uint32_t)x0;
    *out_y = (uint32_t)y0;
    *out_width = (uint32_t)(x1 - x0);
    *out_height = (uint32_t)(y1 - y0);
    return 1;
}

static int in_clip(
    int32_t x,
    int32_t y,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height,
    uint32_t target_width,
    uint32_t target_height
) {
    int64_t cx1 = scissor_enabled ? (int64_t)scissor_x + scissor_width : target_width;
    int64_t cy1 = scissor_enabled ? (int64_t)scissor_y + scissor_height : target_height;
    int64_t cx0 = scissor_enabled ? scissor_x : 0;
    int64_t cy0 = scissor_enabled ? scissor_y : 0;

    return x >= 0 && y >= 0 &&
           x < (int32_t)target_width && y < (int32_t)target_height &&
           x >= cx0 && y >= cy0 && x < cx1 && y < cy1;
}

static int32_t edge_i32(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void put_pixel_mapped(srapi_framebuffer_t *target, int32_t x, int32_t y, srapi_color_t color) {
    uint32_t *row;

    if (target == NULL || target->pixels == NULL ||
        x < 0 || y < 0 || x >= (int32_t)target->width || y >= (int32_t)target->height) {
        return;
    }

    row = (uint32_t *)((uint8_t *)target->pixels + (size_t)y * target->pitch);
    row[x] = color;
}

static void fill_span_mapped(srapi_framebuffer_t *target, int32_t x, int32_t y, int32_t width, srapi_color_t color) {
    uint32_t *row;

    if (target == NULL || target->pixels == NULL || width <= 0 ||
        y < 0 || y >= (int32_t)target->height || x >= (int32_t)target->width || x + width <= 0) {
        return;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (x + width > (int32_t)target->width) {
        width = (int32_t)target->width - x;
    }
    if (width <= 0) {
        return;
    }

    row = (uint32_t *)((uint8_t *)target->pixels + (size_t)y * target->pitch);
    for (int32_t i = 0; i < width; i++) {
        row[x + i] = color;
    }
}

static void image_as_framebuffer(const srapi_image_t *image, srapi_framebuffer_t *fb) {
    memset(fb, 0, sizeof(*fb));
    fb->width = image->width;
    fb->height = image->height;
    fb->pitch = image->pitch;
    fb->pixels = (uint32_t *)image->data;
    fb->backend = image->backend;
    fb->device = image->device;
    fb->gpu_fd = image->device != NULL ? image->device->fd : -1;
    fb->gpu_handle = image->gpu_handle;
    fb->gpu_size = image->gpu_size;
    fb->gpu_memory = image->gpu_memory;
}

srapi_result_t srapi_i915_render3d_line_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
) {
    srapi_framebuffer_t fb;

    if (device == NULL || target == NULL || op == NULL ||
        target->device != device || target->gpu_memory != 1 || target->data == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    image_as_framebuffer(target, &fb);
    return srapi_i915_render3d_line_framebuffer(
        device, &fb, op,
        scissor_enabled, scissor_x, scissor_y, scissor_width, scissor_height
    );
}

srapi_result_t srapi_i915_render3d_line_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
) {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    int32_t dx;
    int32_t sx;
    int32_t dy;
    int32_t sy;
    int32_t err;

    if (device == NULL || target == NULL || op == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    x0 = op->x0;
    y0 = op->y0;
    x1 = op->x1;
    y1 = op->y1;
    dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    sx = x0 < x1 ? 1 : -1;
    dy = y1 >= y0 ? -(y1 - y0) : -(y0 - y1);
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        int32_t e2;

        if (in_clip(x0, y0, scissor_enabled, scissor_x, scissor_y,
                    scissor_width, scissor_height, target->width, target->height)) {
            put_pixel_mapped(target, x0, y0, op->color);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }

        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }

    (void)device;
    srapi_debugf("i915 render3d line mapped %d,%d -> %d,%d", op->x0, op->y0, op->x1, op->y1);
    return SRAPI_OK;
}

srapi_result_t srapi_i915_render3d_triangle_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
) {
    srapi_framebuffer_t fb;

    if (device == NULL || target == NULL || op == NULL ||
        target->device != device || target->gpu_memory != 1 || target->data == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    image_as_framebuffer(target, &fb);
    return srapi_i915_render3d_triangle_framebuffer(
        device, &fb, op,
        scissor_enabled, scissor_x, scissor_y, scissor_width, scissor_height
    );
}

srapi_result_t srapi_i915_render3d_triangle_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
) {
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    int32_t area;

    if (device == NULL || target == NULL || op == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    min_x = op->x0 < op->x1 ? op->x0 : op->x1;
    if (op->x2 < min_x) min_x = op->x2;
    max_x = op->x0 > op->x1 ? op->x0 : op->x1;
    if (op->x2 > max_x) max_x = op->x2;
    min_y = op->y0 < op->y1 ? op->y0 : op->y1;
    if (op->y2 < min_y) min_y = op->y2;
    max_y = op->y0 > op->y1 ? op->y0 : op->y1;
    if (op->y2 > max_y) max_y = op->y2;

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int32_t)target->width) max_x = (int32_t)target->width - 1;
    if (max_y >= (int32_t)target->height) max_y = (int32_t)target->height - 1;
    if (scissor_enabled) {
        if (min_x < scissor_x) min_x = scissor_x;
        if (min_y < scissor_y) min_y = scissor_y;
        if (max_x >= scissor_x + (int32_t)scissor_width) max_x = scissor_x + (int32_t)scissor_width - 1;
        if (max_y >= scissor_y + (int32_t)scissor_height) max_y = scissor_y + (int32_t)scissor_height - 1;
    }
    if (min_x > max_x || min_y > max_y) {
        return SRAPI_OK;
    }

    area = edge_i32(op->x0, op->y0, op->x1, op->y1, op->x2, op->y2);
    if (area == 0) {
        return SRAPI_OK;
    }

    for (int32_t y = min_y; y <= max_y; y++) {
        int32_t run_x = -1;
        int32_t run_len = 0;

        for (int32_t x = min_x; x <= max_x; x++) {
            int32_t px = x * 2 + 1;
            int32_t py = y * 2 + 1;
            int32_t w0 = edge_i32(op->x1 * 2, op->y1 * 2, op->x2 * 2, op->y2 * 2, px, py);
            int32_t w1 = edge_i32(op->x2 * 2, op->y2 * 2, op->x0 * 2, op->y0 * 2, px, py);
            int32_t w2 = edge_i32(op->x0 * 2, op->y0 * 2, op->x1 * 2, op->y1 * 2, px, py);
            int inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);

            if (inside) {
                if (run_len == 0) {
                    run_x = x;
                }
                run_len++;
            } else if (run_len > 0) {
                fill_span_mapped(target, run_x, y, run_len, op->color);
                run_len = 0;
            }
        }

        if (run_len > 0) {
            fill_span_mapped(target, run_x, y, run_len, op->color);
        }
    }

    (void)device;
    srapi_debugf("i915 render3d triangle mapped");
    return SRAPI_OK;
}

srapi_result_t srapi_i915_render3d_shade_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    if (device == NULL || target == NULL || op == NULL || op->shader == NULL ||
        target->device != device || target->gpu_memory != 1) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (!clip_rect(op->x0, op->y0, op->width, op->height,
                   scissor_enabled ? scissor_x : 0,
                   scissor_enabled ? scissor_y : 0,
                   scissor_enabled ? scissor_width : target->width,
                   scissor_enabled ? scissor_height : target->height,
                   &x, &y, &width, &height)) {
        return SRAPI_OK;
    }

    srapi_debugf("i915 render3d shade_rect cpu-simd target=image %u,%u %ux%u",
                 x, y, width, height);

    if (target->gpu_handle != 0) {
        drm_gem_wait_handle(device->fd, target->gpu_handle, 1000000000LL);
    }
    return srapi_i915_vm_shade_image(device, target, op, x, y, width, height);
}

srapi_result_t srapi_i915_render3d_shade_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x,
    int32_t scissor_y,
    uint32_t scissor_width,
    uint32_t scissor_height
) {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;

    if (device == NULL || target == NULL || op == NULL || op->shader == NULL ||
        target->gpu_handle == 0 || target->gpu_size == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (!clip_rect(op->x0, op->y0, op->width, op->height,
                   scissor_enabled ? scissor_x : 0,
                   scissor_enabled ? scissor_y : 0,
                   scissor_enabled ? scissor_width : target->width,
                   scissor_enabled ? scissor_height : target->height,
                   &x, &y, &width, &height)) {
        return SRAPI_OK;
    }

    srapi_debugf("i915 render3d shade_rect cpu-simd target=framebuffer %u,%u %ux%u",
                 x, y, width, height);

    drm_gem_wait_handle(device->fd, target->gpu_handle, 1000000000LL);
    return srapi_i915_vm_shade_framebuffer(device, target, op, x, y, width, height);
}
