#include "internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

srapi_color_t srapi_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

const char *srapi_backend_name(srapi_backend_t backend) {
    switch (backend) {
        case SRAPI_BACKEND_AUTO: return "auto";
        case SRAPI_BACKEND_CPU: return "cpu";
        case SRAPI_BACKEND_GPU: return "gpu";
        default: return "unknown";
    }
}

srapi_result_t srapi_create_context(const srapi_context_desc_t *desc, srapi_context_t **out) {
    srapi_context_t *ctx;
    srapi_backend_t backend;

    if (desc == NULL || out == NULL || desc->width == 0 || desc->height == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    backend = desc->backend;
    if (backend == SRAPI_BACKEND_AUTO) {
        backend = SRAPI_BACKEND_CPU;
    } else if (backend != SRAPI_BACKEND_CPU && backend != SRAPI_BACKEND_GPU) {
        return SRAPI_ERROR_BAD_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return SRAPI_ERROR_OOM;
    }
    ctx->width = desc->width;
    ctx->height = desc->height;
    ctx->backend = backend;
    *out = ctx;
    srapi_debugf("context create %ux%u backend=%s",
                 ctx->width, ctx->height, srapi_backend_name(ctx->backend));
    if (ctx->backend == SRAPI_BACKEND_GPU) {
        srapi_debugf("context gpu mode: DRM target buffers are GPU-backed; command execution still uses cpu interpreter");
    }
    return SRAPI_OK;
}

void srapi_destroy_context(srapi_context_t *ctx) {
    if (ctx != NULL) {
        srapi_debugf("context destroy %ux%u backend=%s",
                     ctx->width, ctx->height, srapi_backend_name(ctx->backend));
    }
    free(ctx);
}

srapi_backend_t srapi_context_backend(const srapi_context_t *ctx) {
    return ctx != NULL ? ctx->backend : SRAPI_BACKEND_AUTO;
}

srapi_result_t srapi_probe_device(srapi_backend_t backend, srapi_device_info_t *out) {
    if (out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->backend = backend;

    if (backend == SRAPI_BACKEND_AUTO) {
        backend = SRAPI_BACKEND_CPU;
        out->backend = backend;
    }

    if (backend == SRAPI_BACKEND_CPU) {
        out->available = 1;
        snprintf(out->path, sizeof(out->path), "cpu");
        snprintf(out->message, sizeof(out->message), "cpu backend available");
        srapi_debugf("device probe backend=cpu available=1 path=%s", out->path);
        return SRAPI_OK;
    }

    if (backend == SRAPI_BACKEND_GPU) {
        return srapi_gpu_probe(out);
    }

    srapi_set_error("device: unknown backend %d", backend);
    return SRAPI_ERROR_BAD_ARG;
}

srapi_result_t srapi_create_device(const srapi_device_desc_t *desc, srapi_device_t **out) {
    srapi_device_t *device;
    srapi_backend_t backend;
    srapi_device_info_t info;

    if (desc == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    backend = desc->backend == SRAPI_BACKEND_AUTO ? SRAPI_BACKEND_CPU : desc->backend;
    if (backend == SRAPI_BACKEND_GPU) {
        return srapi_gpu_open_device(desc, out);
    }
    if (srapi_probe_device(backend, &info) != SRAPI_OK) {
        return SRAPI_ERROR_UNSUPPORTED;
    }

    device = calloc(1, sizeof(*device));
    if (device == NULL) {
        return SRAPI_ERROR_OOM;
    }

    device->backend = backend;
    device->fd = -1;
    if (desc->device_path != NULL && desc->device_path[0] != '\0') {
        snprintf(device->path, sizeof(device->path), "%s", desc->device_path);
    } else {
        snprintf(device->path, sizeof(device->path), "%s", info.path);
    }

    *out = device;
    srapi_debugf("device create backend=%s path=%s",
                 srapi_backend_name(device->backend), device->path);
    return SRAPI_OK;
}

void srapi_destroy_device(srapi_device_t *device) {
    if (device != NULL) {
        srapi_debugf("device destroy backend=%s path=%s",
                     srapi_backend_name(device->backend), device->path);
        if (device->backend == SRAPI_BACKEND_GPU) {
            srapi_gpu_close_device(device);
            return;
        }
    }
    free(device);
}

srapi_backend_t srapi_device_backend(const srapi_device_t *device) {
    return device != NULL ? device->backend : SRAPI_BACKEND_AUTO;
}

const char *srapi_device_path(const srapi_device_t *device) {
    return device != NULL ? device->path : "";
}

srapi_result_t srapi_submit(
    srapi_context_t *ctx,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
) {
    (void)ctx;

    if (target == NULL || cmd == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_debugf("submit %zu commands ctx_backend=%s target_backend=%s framebuffer=%ux%u pitch=%u",
                 cmd->count,
                 ctx != NULL ? srapi_backend_name(ctx->backend) : "none",
                 srapi_backend_name(target->backend),
                 target->width, target->height, target->pitch);
    if (ctx != NULL && ctx->backend == SRAPI_BACKEND_GPU && target->backend == SRAPI_BACKEND_GPU) {
        srapi_debugf("submit gpu/drm path: writing mapped DRM backbuffer, present handled by drm backend");
    }

    for (size_t i = 0; i < cmd->count; i++) {
        const srapi_command_t *op = &cmd->items[i];

        switch (op->kind) {
            case SRAPI_COMMAND_CLEAR:
                srapi_debugf("cmd[%zu] clear color=0x%08x", i, op->color);
                srapi_render_clear(target, op->color);
                break;
            case SRAPI_COMMAND_FILL_RECT:
                srapi_debugf("cmd[%zu] fill_rect x=%d y=%d w=%u h=%u color=0x%08x",
                             i, op->x0, op->y0, op->width, op->height, op->color);
                srapi_render_fill_rect(target, op->x0, op->y0, op->width, op->height, op->color);
                break;
            case SRAPI_COMMAND_DRAW_LINE:
                srapi_debugf("cmd[%zu] draw_line %d,%d -> %d,%d color=0x%08x",
                             i, op->x0, op->y0, op->x1, op->y1, op->color);
                srapi_render_draw_line(target, op->x0, op->y0, op->x1, op->y1, op->color);
                break;
            case SRAPI_COMMAND_FILL_TRIANGLE:
                srapi_debugf("cmd[%zu] fill_triangle %d,%d %d,%d %d,%d color=0x%08x",
                             i, op->x0, op->y0, op->x1, op->y1, op->x2, op->y2, op->color);
                srapi_render_fill_triangle(target, op->x0, op->y0, op->x1, op->y1, op->x2, op->y2, op->color);
                break;
            case SRAPI_COMMAND_SHADE_RECT:
                srapi_debugf("cmd[%zu] shade_rect x=%d y=%d w=%u h=%u shader=%p",
                             i, op->x0, op->y0, op->width, op->height, (void *)op->shader);
                srapi_render_shade_rect(target, op->x0, op->y0, op->width, op->height, op->shader);
                break;
            default:
                return SRAPI_ERROR;
        }
    }

    return SRAPI_OK;
}
