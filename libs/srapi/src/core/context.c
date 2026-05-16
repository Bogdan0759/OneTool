#include "internal.h"

#include <stdint.h>
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
        case SRAPI_BACKEND_FBDEV: return "fbdev";
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
    } else if (backend != SRAPI_BACKEND_CPU && backend != SRAPI_BACKEND_GPU && backend != SRAPI_BACKEND_FBDEV) {
        return SRAPI_ERROR_BAD_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return SRAPI_ERROR_OOM;
    }
    ctx->width = desc->width;
    ctx->height = desc->height;
    ctx->backend = backend;
    ctx->backend_checks = desc->backend_config != NULL
        ? desc->backend_config->checks
        : srapi_backend_enabled_checks(backend);
    *out = ctx;
    srapi_debugf("context create %ux%u backend=%s checks=0x%x",
                 ctx->width, ctx->height, srapi_backend_name(ctx->backend), ctx->backend_checks);
    if (ctx->backend == SRAPI_BACKEND_GPU) {
        srapi_debugf("context gpu mode: resources and display targets are DRM-backed");
    } else if (ctx->backend == SRAPI_BACKEND_FBDEV) {
        srapi_debugf("context fbdev mode: render directly into mapped linux framebuffer");
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

srapi_result_t srapi_context_get_backend_config(
    const srapi_context_t *ctx,
    srapi_backend_config_t *out
) {
    if (ctx == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    out->backend = ctx->backend;
    out->checks = ctx->backend_checks;
    return SRAPI_OK;
}

srapi_result_t srapi_context_set_backend_config(
    srapi_context_t *ctx,
    const srapi_backend_config_t *config
) {
    if (ctx == NULL || config == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (config->backend != SRAPI_BACKEND_AUTO && config->backend != ctx->backend) {
        srapi_set_error("context config: backend=%s does not match context backend=%s",
                        srapi_backend_name(config->backend), srapi_backend_name(ctx->backend));
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((config->checks & ~SRAPI_BACKEND_CHECK_ALL) != 0) {
        srapi_set_error("context config: unknown check flags 0x%x",
                        config->checks & ~SRAPI_BACKEND_CHECK_ALL);
        return SRAPI_ERROR_BAD_ARG;
    }

    ctx->backend_checks = config->checks;
    srapi_debugf("context config set backend=%s checks=0x%x",
                 srapi_backend_name(ctx->backend), ctx->backend_checks);
    return SRAPI_OK;
}

uint32_t srapi_context_enabled_checks(const srapi_context_t *ctx) {
    return ctx != NULL ? ctx->backend_checks : 0;
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
    if (backend == SRAPI_BACKEND_FBDEV) {
        return srapi_fbdev_probe(out);
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
    if (backend == SRAPI_BACKEND_FBDEV) {
        srapi_set_error("device: fbdev backend only supports display/framebuffer output");
        return SRAPI_ERROR_UNSUPPORTED;
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

static int primitive_group_size(srapi_primitive_topology_t topology) {
    switch (topology) {
        case SRAPI_PRIMITIVE_POINTS: return 1;
        case SRAPI_PRIMITIVE_LINES: return 2;
        case SRAPI_PRIMITIVE_TRIANGLES: return 3;
        default: return 0;
    }
}

static float color_channel_unit(srapi_color_t color, uint32_t shift) {
    return (float)((color >> shift) & 0xff) / 255.0f;
}

static srapi_result_t submit_draw_vertices(
    const srapi_context_t *ctx,
    srapi_framebuffer_t *target,
    const srapi_command_t *op
) {
    const srapi_vertex_t *vertices;
    const uint32_t *indices = NULL;
    srapi_vertex_t *transformed = NULL;
    float *positions = NULL;
    size_t total_vertices;
    size_t total_indices;
    size_t draw_count;
    int group_size;

    if (op->vertex_buffer == NULL || op->vertex_buffer->data == NULL || op->vertex_count == 0) {
        srapi_set_error("draw: missing vertex buffer");
        return SRAPI_ERROR_BAD_ARG;
    }
    if ((ctx == NULL || (ctx->backend_checks & SRAPI_BACKEND_CHECK_USAGE) != 0) &&
        (op->vertex_buffer->usage & SRAPI_BUFFER_VERTEX) == 0) {
        srapi_set_error("draw: buffer missing vertex usage");
        return SRAPI_ERROR_BAD_ARG;
    }

    total_vertices = op->vertex_buffer->size / sizeof(*vertices);
    if (op->vertex_offset > total_vertices || op->vertex_count > total_vertices - op->vertex_offset) {
        srapi_set_error("draw: vertex range outside buffer");
        return SRAPI_ERROR_BAD_ARG;
    }
    vertices = (const srapi_vertex_t *)op->vertex_buffer->data + op->vertex_offset;

    draw_count = op->vertex_count;
    if (op->index_buffer != NULL) {
        if (op->index_buffer->data == NULL || op->index_count == 0) {
            srapi_set_error("draw: missing index buffer");
            return SRAPI_ERROR_BAD_ARG;
        }
        if ((ctx == NULL || (ctx->backend_checks & SRAPI_BACKEND_CHECK_USAGE) != 0) &&
            (op->index_buffer->usage & SRAPI_BUFFER_INDEX) == 0) {
            srapi_set_error("draw: buffer missing index usage");
            return SRAPI_ERROR_BAD_ARG;
        }

        total_indices = op->index_buffer->size / sizeof(*indices);
        if (op->index_offset > total_indices || op->index_count > total_indices - op->index_offset) {
            srapi_set_error("draw: index range outside buffer");
            return SRAPI_ERROR_BAD_ARG;
        }

        indices = (const uint32_t *)op->index_buffer->data + op->index_offset;
        draw_count = op->index_count;
        for (size_t i = 0; i < op->index_count; i++) {
            if (indices[i] >= op->vertex_count) {
                srapi_set_error("draw: index %u outside vertex range", indices[i]);
                return SRAPI_ERROR_BAD_ARG;
            }
        }
    }

    group_size = primitive_group_size(op->topology);
    if (group_size == 0 || draw_count == 0 || draw_count % (size_t)group_size != 0) {
        srapi_set_error("draw: bad primitive topology/count");
        return SRAPI_ERROR_BAD_ARG;
    }

    if (op->shader != NULL) {
        if (op->vertex_count > SIZE_MAX / (sizeof(*positions) * 2)) {
            srapi_set_error("draw: transformed vertex allocation overflow");
            return SRAPI_ERROR_OVERFLOW;
        }
        transformed = calloc(op->vertex_count, sizeof(*transformed));
        positions = calloc(op->vertex_count * 2, sizeof(*positions));
        if (transformed == NULL || positions == NULL) {
            free(transformed);
            free(positions);
            return SRAPI_ERROR_OOM;
        }

        for (size_t i = 0; i < op->vertex_count; i++) {
            float inputs[7];
            float out_pos[2];
            srapi_color_t out_color;
            srapi_result_t r;

            inputs[SRAPI_VM_INPUT_VERTEX_X] = vertices[i].x;
            inputs[SRAPI_VM_INPUT_VERTEX_Y] = vertices[i].y;
            inputs[SRAPI_VM_INPUT_VERTEX_R] = color_channel_unit(vertices[i].color, 16);
            inputs[SRAPI_VM_INPUT_VERTEX_G] = color_channel_unit(vertices[i].color, 8);
            inputs[SRAPI_VM_INPUT_VERTEX_B] = color_channel_unit(vertices[i].color, 0);
            inputs[SRAPI_VM_INPUT_VERTEX_A] = color_channel_unit(vertices[i].color, 24);
            inputs[SRAPI_VM_INPUT_VERTEX_INDEX] = (float)i;

            r = srapi_vm_run_vertex(op->shader, inputs, out_pos, &out_color);
            if (r != SRAPI_OK) {
                free(transformed);
                free(positions);
                return r;
            }

            transformed[i] = vertices[i];
            transformed[i].x = out_pos[0];
            transformed[i].y = out_pos[1];
            transformed[i].color = out_color;
            positions[i * 2 + 0] = out_pos[0];
            positions[i * 2 + 1] = out_pos[1];
        }

        srapi_render_draw_vertices_transformed(
            target,
            op->topology,
            transformed,
            op->vertex_count,
            indices,
            op->index_count,
            positions
        );
        free(transformed);
        free(positions);
    } else {
        srapi_render_draw_vertices(target, op->topology, vertices, op->vertex_count, indices, op->index_count);
    }
    return SRAPI_OK;
}

srapi_result_t srapi_submit(
    srapi_context_t *ctx,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
) {
    srapi_render_state_t state;

    if (target == NULL || cmd == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    if (ctx != NULL &&
        (ctx->backend_checks & SRAPI_BACKEND_CHECK_BACKEND_MATCH) != 0 &&
        target->backend != ctx->backend) {
        srapi_set_error("submit: target backend=%s does not match context backend=%s",
                        srapi_backend_name(target->backend), srapi_backend_name(ctx->backend));
        return SRAPI_ERROR_BAD_ARG;
    }

    state = srapi_render_default_state(target);
    srapi_render_set_state(&state);

    srapi_debugf("submit %zu commands ctx_backend=%s target_backend=%s framebuffer=%ux%u pitch=%u",
                 cmd->count,
                 ctx != NULL ? srapi_backend_name(ctx->backend) : "none",
                 srapi_backend_name(target->backend),
                 target->width, target->height, target->pitch);
    if (ctx != NULL && target->backend == ctx->backend &&
        (ctx->backend == SRAPI_BACKEND_GPU || ctx->backend == SRAPI_BACKEND_FBDEV)) {
        srapi_debugf("submit %s display path: rendering into mapped target",
                     srapi_backend_name(ctx->backend));
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
            case SRAPI_COMMAND_DRAW_VERTICES:
                srapi_debugf("cmd[%zu] draw_vertices topology=%d vertices=%zu indices=%zu",
                             i, op->topology, op->vertex_count, op->index_count);
                {
                    srapi_result_t r = submit_draw_vertices(ctx, target, op);
                    if (r != SRAPI_OK) {
                        return r;
                    }
                }
                break;
            case SRAPI_COMMAND_SET_SCISSOR:
                state.scissor_enabled = 1;
                state.scissor_x = op->x0;
                state.scissor_y = op->y0;
                state.scissor_width = op->width;
                state.scissor_height = op->height;
                srapi_render_set_state(&state);
                srapi_debugf("cmd[%zu] set_scissor x=%d y=%d w=%u h=%u",
                             i, op->x0, op->y0, op->width, op->height);
                break;
            case SRAPI_COMMAND_SET_VIEWPORT:
                state.viewport_enabled = 1;
                state.viewport_x = op->x0;
                state.viewport_y = op->y0;
                state.viewport_width = op->width;
                state.viewport_height = op->height;
                srapi_render_set_state(&state);
                srapi_debugf("cmd[%zu] set_viewport x=%d y=%d w=%u h=%u",
                             i, op->x0, op->y0, op->width, op->height);
                break;
            case SRAPI_COMMAND_SET_BLEND:
                state.blend_mode = op->blend_mode;
                srapi_render_set_state(&state);
                srapi_debugf("cmd[%zu] set_blend mode=%d", i, op->blend_mode);
                break;
            default:
                return SRAPI_ERROR;
        }
    }

    return SRAPI_OK;
}
