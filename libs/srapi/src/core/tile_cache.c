#include "internal.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

static uint64_t tc_hash_u64(uint64_t h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static uint64_t tc_hash_bytes(uint64_t h, const void *data, size_t len) {
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h = tc_hash_u64(h, b[i]);
    }
    return h;
}

static uint64_t tc_shader_hash(const srapi_shader_t *s) {
    uint64_t h = 1469598103934665603ull;
    if (s == NULL) return 0;
    h = tc_hash_u64(h, s->word_count);
    h = tc_hash_u64(h, s->inst_count);
    h = tc_hash_u64(h, s->uniform_count);
    h = tc_hash_bytes(h, s->bytecode, s->word_count * sizeof(uint32_t));
    h = tc_hash_bytes(h, s->uniforms, s->uniform_count * sizeof(float));
    return h;
}

static int tc_clip(
    int32_t x, int32_t y, uint32_t w, uint32_t h,
    int32_t cx, int32_t cy, uint32_t cw, uint32_t ch,
    int32_t *ox, int32_t *oy, uint32_t *ow, uint32_t *oh
) {
    int64_t x0 = x < cx ? cx : x;
    int64_t y0 = y < cy ? cy : y;
    int64_t x1 = (int64_t)x + w;
    int64_t y1 = (int64_t)y + h;
    int64_t cx1 = (int64_t)cx + cw;
    int64_t cy1 = (int64_t)cy + ch;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return 0;
    *ox = (int32_t)x0; *oy = (int32_t)y0;
    *ow = (uint32_t)(x1 - x0); *oh = (uint32_t)(y1 - y0);
    return 1;
}

static uint64_t tc_command_hash(
    const srapi_command_t *op,
    int scissor_enabled,
    int32_t scissor_x, int32_t scissor_y,
    uint32_t scissor_width, uint32_t scissor_height,
    int32_t tile_x, int32_t tile_y,
    uint32_t tile_width, uint32_t tile_height
) {
    int32_t  clip_x = scissor_enabled ? scissor_x : tile_x;
    int32_t  clip_y = scissor_enabled ? scissor_y : tile_y;
    uint32_t clip_w = scissor_enabled ? scissor_width  : tile_width;
    uint32_t clip_h = scissor_enabled ? scissor_height : tile_height;

    int32_t  ex; int32_t  ey; uint32_t ew; uint32_t eh;
    if (!tc_clip(clip_x, clip_y, clip_w, clip_h,
                 tile_x, tile_y, tile_width, tile_height,
                 &ex, &ey, &ew, &eh)) {
        ex = tile_x; ey = tile_y; ew = tile_width; eh = tile_height;
    }

    uint64_t h = 1469598103934665603ull;
    int32_t ox; int32_t oy; uint32_t ow; uint32_t oh;

    switch (op->kind) {
        case SRAPI_COMMAND_CLEAR:
        case SRAPI_COMMAND_FILL_RECT:
        case SRAPI_COMMAND_SHADE_RECT: {
            int32_t rx = op->kind == SRAPI_COMMAND_CLEAR ? 0 : op->x0;
            int32_t ry = op->kind == SRAPI_COMMAND_CLEAR ? 0 : op->y0;
            uint32_t rw = op->kind == SRAPI_COMMAND_CLEAR ? 0x7fffffff : op->width;
            uint32_t rh = op->kind == SRAPI_COMMAND_CLEAR ? 0x7fffffff : op->height;
            if (!tc_clip(rx, ry, rw, rh, ex, ey, ew, eh, &ox, &oy, &ow, &oh))
                return 0;
            h = tc_hash_u64(h, (uint64_t)op->kind);
            h = tc_hash_u64(h, (uint64_t)(int64_t)ox);
            h = tc_hash_u64(h, (uint64_t)(int64_t)oy);
            h = tc_hash_u64(h, ow); h = tc_hash_u64(h, oh);
            h = tc_hash_u64(h, op->color);
            if (op->kind == SRAPI_COMMAND_SHADE_RECT)
                h = tc_hash_u64(h, tc_shader_hash(op->shader));
            return h;
        }
        case SRAPI_COMMAND_DRAW_LINE: {
            int32_t mnx = op->x0 < op->x1 ? op->x0 : op->x1;
            int32_t mxx = op->x0 > op->x1 ? op->x0 : op->x1;
            int32_t mny = op->y0 < op->y1 ? op->y0 : op->y1;
            int32_t mxy = op->y0 > op->y1 ? op->y0 : op->y1;
            if (!tc_clip(mnx, mny, (uint32_t)(mxx - mnx + 1), (uint32_t)(mxy - mny + 1),
                         ex, ey, ew, eh, &ox, &oy, &ow, &oh))
                return 0;
            h = tc_hash_u64(h, (uint64_t)op->kind);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->x0);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->y0);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->x1);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->y1);
            h = tc_hash_u64(h, op->color);
            return h;
        }
        case SRAPI_COMMAND_FILL_TRIANGLE: {
            int32_t mnx = op->x0 < op->x1 ? op->x0 : op->x1;
            int32_t mxx = op->x0 > op->x1 ? op->x0 : op->x1;
            int32_t mny = op->y0 < op->y1 ? op->y0 : op->y1;
            int32_t mxy = op->y0 > op->y1 ? op->y0 : op->y1;
            if (op->x2 < mnx) mnx = op->x2;
            if (op->x2 > mxx) mxx = op->x2;
            if (op->y2 < mny) mny = op->y2;
            if (op->y2 > mxy) mxy = op->y2;
            if (!tc_clip(mnx, mny, (uint32_t)(mxx - mnx + 1), (uint32_t)(mxy - mny + 1),
                         ex, ey, ew, eh, &ox, &oy, &ow, &oh))
                return 0;
            h = tc_hash_u64(h, (uint64_t)op->kind);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->x0);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->y0);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->x1);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->y1);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->x2);
            h = tc_hash_u64(h, (uint64_t)(int64_t)op->y2);
            h = tc_hash_u64(h, op->color);
            return h;
        }
        case SRAPI_COMMAND_SET_SCISSOR:
        case SRAPI_COMMAND_SET_VIEWPORT:
            return tc_hash_u64(tc_hash_u64(tc_hash_u64(tc_hash_u64(h,
                op->kind), (uint64_t)(int64_t)op->x0), (uint64_t)(int64_t)op->y0),
                op->width ^ ((uint64_t)op->height << 32));
        case SRAPI_COMMAND_SET_BLEND:
            return tc_hash_u64(tc_hash_u64(h, (uint64_t)op->kind), (uint64_t)op->blend_mode);
        default:
            return tc_hash_u64(h, (uint64_t)op->kind);
    }
}

static uint64_t tc_tile_hash(
    const srapi_cmd_buffer_t *cmd,
    int32_t tile_x, int32_t tile_y,
    uint32_t tile_w, uint32_t tile_h,
    uint32_t target_w, uint32_t target_h
) {
    int scissor_enabled = 0;
    int32_t scissor_x = 0, scissor_y = 0;
    uint32_t scissor_width = target_w, scissor_height = target_h;
    uint64_t h = 1469598103934665603ull;

    for (size_t i = 0; i < cmd->count; i++) {
        const srapi_command_t *op = &cmd->items[i];
        uint64_t ch = tc_command_hash(op,
            scissor_enabled, scissor_x, scissor_y, scissor_width, scissor_height,
            tile_x, tile_y, tile_w, tile_h);
        if (op->kind == SRAPI_COMMAND_SET_SCISSOR) {
            scissor_enabled = 1;
            scissor_x = op->x0; scissor_y = op->y0;
            scissor_width = op->width; scissor_height = op->height;
        }
        h = tc_hash_u64(h, ch);
    }
    return h;
}


static srapi_result_t cpu_render_tile(
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd,
    int32_t tile_x, int32_t tile_y,
    uint32_t tile_w, uint32_t tile_h
) {
    srapi_render_state_t state;
    memset(&state, 0, sizeof(state));
    state.scissor_enabled = 1;
    state.scissor_x = tile_x;
    state.scissor_y = tile_y;
    state.scissor_width  = tile_w;
    state.scissor_height = tile_h;
    srapi_render_set_state(&state);

    for (size_t i = 0; i < cmd->count; i++) {
        const srapi_command_t *op = &cmd->items[i];
        switch (op->kind) {
            case SRAPI_COMMAND_CLEAR:
                srapi_render_clear(target, op->color);
                break;
            case SRAPI_COMMAND_FILL_RECT:
                srapi_render_fill_rect(target, op->x0, op->y0, op->width, op->height, op->color);
                break;
            case SRAPI_COMMAND_DRAW_LINE:
                srapi_render_draw_line(target, op->x0, op->y0, op->x1, op->y1, op->color);
                break;
            case SRAPI_COMMAND_FILL_TRIANGLE:
                srapi_render_fill_triangle(target,
                    op->x0, op->y0, op->x1, op->y1, op->x2, op->y2, op->color);
                break;
            case SRAPI_COMMAND_SHADE_RECT:
                srapi_render_shade_rect(target, op->x0, op->y0, op->width, op->height, op->shader);
                break;
            case SRAPI_COMMAND_SET_SCISSOR: {
                int32_t sx = op->x0 > tile_x ? op->x0 : tile_x;
                int32_t sy = op->y0 > tile_y ? op->y0 : tile_y;
                int32_t sx1 = op->x0 + (int32_t)op->width;
                int32_t sy1 = op->y0 + (int32_t)op->height;
                int32_t tx1 = tile_x + (int32_t)tile_w;
                int32_t ty1 = tile_y + (int32_t)tile_h;
                if (sx1 > tx1) sx1 = tx1;
                if (sy1 > ty1) sy1 = ty1;
                state.scissor_enabled = 1;
                state.scissor_x = sx;
                state.scissor_y = sy;
                state.scissor_width  = (uint32_t)(sx1 > sx ? sx1 - sx : 0);
                state.scissor_height = (uint32_t)(sy1 > sy ? sy1 - sy : 0);
                srapi_render_set_state(&state);
                break;
            }
            case SRAPI_COMMAND_SET_VIEWPORT:
                state.viewport_enabled = 1;
                state.viewport_x = op->x0; state.viewport_y = op->y0;
                state.viewport_width = op->width; state.viewport_height = op->height;
                srapi_render_set_state(&state);
                break;
            case SRAPI_COMMAND_SET_BLEND:
                state.blend_mode = op->blend_mode;
                srapi_render_set_state(&state);
                break;
            default:
                break;
        }
    }
    return SRAPI_OK;
}


srapi_result_t srapi_tile_cache_submit_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
) {
    const uint32_t cols = device->tile_cols;
    const uint32_t rows = device->tile_rows;
    uint32_t tile_w = (target->width  + cols - 1) / cols;
    uint32_t tile_h = (target->height + rows - 1) / rows;

    if (tile_w == 0) tile_w = target->width;
    if (tile_h == 0) tile_h = target->height;

    for (uint32_t ty = 0; ty < rows; ty++) {
        for (uint32_t tx = 0; tx < cols; tx++) {
            uint32_t  idx    = ty * cols + tx;
            int32_t   tile_x = (int32_t)(tx * tile_w);
            int32_t   tile_y = (int32_t)(ty * tile_h);
            uint32_t  cur_w  = tile_w;
            uint32_t  cur_h  = tile_h;

            if (tile_x >= (int32_t)target->width || tile_y >= (int32_t)target->height)
                continue;
            if ((uint32_t)tile_x + cur_w > target->width)
                cur_w = target->width - (uint32_t)tile_x;
            if ((uint32_t)tile_y + cur_h > target->height)
                cur_h = target->height - (uint32_t)tile_y;
            if (cur_w == 0 || cur_h == 0)
                continue;

            uint64_t hash = tc_tile_hash(cmd, tile_x, tile_y, cur_w, cur_h,
                                         target->width, target->height);
            if (device->tile_hashes[idx] == hash)
                continue; 
            device->tile_hashes[idx] = hash;

            srapi_result_t r;
            if (device->backend == SRAPI_BACKEND_GPU && device->gpu_driver == 915) {
                r = srapi_i915_render_tile_region(device, target, cmd,
                                                  tile_x, tile_y, cur_w, cur_h);
            } else {
                r = cpu_render_tile(target, cmd, tile_x, tile_y, cur_w, cur_h);
            }
            if (r != SRAPI_OK)
                return r;
        }
    }

    srapi_debugf("tile_cache submit ok backend=%d commands=%zu %ux%u",
                 (int)device->backend, cmd->count, target->width, target->height);
    return SRAPI_OK;
}
