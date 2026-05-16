#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

srapi_result_t srapi_create_cmd_buffer(srapi_context_t *ctx, srapi_cmd_buffer_t **out) {
    srapi_cmd_buffer_t *cmd;

    (void)ctx;

    if (out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    cmd = calloc(1, sizeof(*cmd));
    if (cmd == NULL) {
        return SRAPI_ERROR_OOM;
    }
    *out = cmd;
    srapi_debugf("cmd_buffer create");
    return SRAPI_OK;
}

void srapi_destroy_cmd_buffer(srapi_cmd_buffer_t *cmd) {
    if (cmd == NULL) {
        return;
    }
    srapi_debugf("cmd_buffer destroy count=%zu capacity=%zu", cmd->count, cmd->capacity);
    free(cmd->items);
    free(cmd);
}

void srapi_cmd_reset(srapi_cmd_buffer_t *cmd) {
    if (cmd != NULL) {
        srapi_debugf("cmd_buffer reset old_count=%zu", cmd->count);
        cmd->count = 0;
    }
}

srapi_result_t srapi_cmd_push(srapi_cmd_buffer_t *cmd, const srapi_command_t *item) {
    srapi_command_t *new_items;
    size_t new_capacity;

    if (cmd == NULL || item == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (cmd->count == cmd->capacity) {
        new_capacity = cmd->capacity == 0 ? 16 : cmd->capacity * 2;
        if (new_capacity < cmd->capacity || new_capacity > SIZE_MAX / sizeof(*cmd->items)) {
            return SRAPI_ERROR_OVERFLOW;
        }
        new_items = realloc(cmd->items, new_capacity * sizeof(*cmd->items));
        if (new_items == NULL) {
            return SRAPI_ERROR_OOM;
        }
        cmd->items = new_items;
        cmd->capacity = new_capacity;
        srapi_debugf("cmd_buffer grow capacity=%zu", cmd->capacity);
    }
    cmd->items[cmd->count++] = *item;
    srapi_debugf("cmd_buffer push kind=%d count=%zu", item->kind, cmd->count);
    return SRAPI_OK;
}

srapi_result_t srapi_cmd_emit(srapi_cmd_buffer_t *cmd, const srapi_command_t *command) {
    if (command == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    switch (command->kind) {
        case SRAPI_COMMAND_CLEAR:
        case SRAPI_COMMAND_FILL_RECT:
        case SRAPI_COMMAND_DRAW_LINE:
        case SRAPI_COMMAND_FILL_TRIANGLE:
        case SRAPI_COMMAND_SHADE_RECT:
        case SRAPI_COMMAND_DRAW_VERTICES:
            return srapi_cmd_push(cmd, command);
        default:
            srapi_set_error("command: unknown kind %d", command->kind);
            return SRAPI_ERROR_BAD_ARG;
    }
}

srapi_result_t srapi_cmd_clear(srapi_cmd_buffer_t *cmd, srapi_color_t color) {
    srapi_command_t op;

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_CLEAR;
    op.color = color;
    return srapi_cmd_emit(cmd, &op);
}

srapi_result_t srapi_cmd_fill_rect(
    srapi_cmd_buffer_t *cmd,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_color_t color
) {
    srapi_command_t op;

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_FILL_RECT;
    op.color = color;
    op.x0 = x;
    op.y0 = y;
    op.width = width;
    op.height = height;
    return srapi_cmd_emit(cmd, &op);
}

srapi_result_t srapi_cmd_draw_line(
    srapi_cmd_buffer_t *cmd,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    srapi_color_t color
) {
    srapi_command_t op;

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_DRAW_LINE;
    op.color = color;
    op.x0 = x0;
    op.y0 = y0;
    op.x1 = x1;
    op.y1 = y1;
    return srapi_cmd_emit(cmd, &op);
}

srapi_result_t srapi_cmd_fill_triangle(
    srapi_cmd_buffer_t *cmd,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2,
    srapi_color_t color
) {
    srapi_command_t op;

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_FILL_TRIANGLE;
    op.color = color;
    op.x0 = x0;
    op.y0 = y0;
    op.x1 = x1;
    op.y1 = y1;
    op.x2 = x2;
    op.y2 = y2;
    return srapi_cmd_emit(cmd, &op);
}

srapi_result_t srapi_cmd_shade_rect(
    srapi_cmd_buffer_t *cmd,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_shader_t *shader
) {
    srapi_command_t op;

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_SHADE_RECT;
    op.x0 = x;
    op.y0 = y;
    op.width = width;
    op.height = height;
    op.shader = shader;
    return srapi_cmd_emit(cmd, &op);
}

static int primitive_group_size(srapi_primitive_topology_t topology) {
    switch (topology) {
        case SRAPI_PRIMITIVE_POINTS: return 1;
        case SRAPI_PRIMITIVE_LINES: return 2;
        case SRAPI_PRIMITIVE_TRIANGLES: return 3;
        default: return 0;
    }
}

srapi_result_t srapi_cmd_draw_vertices(
    srapi_cmd_buffer_t *cmd,
    srapi_primitive_topology_t topology,
    srapi_buffer_t *vertex_buffer,
    size_t vertex_offset,
    size_t vertex_count,
    srapi_buffer_t *index_buffer,
    size_t index_offset,
    size_t index_count
) {
    srapi_command_t op;
    int group_size = primitive_group_size(topology);
    size_t draw_count = index_buffer != NULL ? index_count : vertex_count;

    if (cmd == NULL || vertex_buffer == NULL || group_size == 0 || vertex_count == 0) {
        srapi_set_error("draw: bad vertex draw args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (index_buffer != NULL && index_count == 0) {
        srapi_set_error("draw: index buffer without indices");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (draw_count == 0 || draw_count % (size_t)group_size != 0) {
        srapi_set_error("draw: primitive count does not match topology");
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_DRAW_VERTICES;
    op.topology = topology;
    op.vertex_buffer = vertex_buffer;
    op.vertex_offset = vertex_offset;
    op.vertex_count = vertex_count;
    op.index_buffer = index_buffer;
    op.index_offset = index_offset;
    op.index_count = index_count;
    return srapi_cmd_emit(cmd, &op);
}

srapi_result_t srapi_cmd_draw_vertices_shader(
    srapi_cmd_buffer_t *cmd,
    srapi_primitive_topology_t topology,
    srapi_buffer_t *vertex_buffer,
    size_t vertex_offset,
    size_t vertex_count,
    srapi_buffer_t *index_buffer,
    size_t index_offset,
    size_t index_count,
    srapi_shader_t *vertex_shader
) {
    srapi_command_t op;
    int group_size = primitive_group_size(topology);
    size_t draw_count = index_buffer != NULL ? index_count : vertex_count;

    if (cmd == NULL || vertex_buffer == NULL || group_size == 0 || vertex_count == 0 || vertex_shader == NULL) {
        srapi_set_error("draw: bad vertex shader draw args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (index_buffer != NULL && index_count == 0) {
        srapi_set_error("draw: index buffer without indices");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (draw_count == 0 || draw_count % (size_t)group_size != 0) {
        srapi_set_error("draw: primitive count does not match topology");
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&op, 0, sizeof(op));
    op.kind = SRAPI_COMMAND_DRAW_VERTICES;
    op.shader = vertex_shader;
    op.vertex_buffer = vertex_buffer;
    op.vertex_offset = vertex_offset;
    op.vertex_count = vertex_count;
    op.index_buffer = index_buffer;
    op.index_offset = index_offset;
    op.index_count = index_count;
    op.topology = topology;
    return srapi_cmd_emit(cmd, &op);
}
