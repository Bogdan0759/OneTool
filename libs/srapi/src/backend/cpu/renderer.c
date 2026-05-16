#include "internal.h"

#include <stdint.h>
#include <stdlib.h>

static void put_pixel(srapi_framebuffer_t *fb, int32_t x, int32_t y, srapi_color_t color) {
    if (x < 0 || y < 0 || x >= (int32_t)fb->width || y >= (int32_t)fb->height) {
        return;
    }
    fb->pixels[(uint32_t)y * (fb->pitch / sizeof(uint32_t)) + (uint32_t)x] = color;
}

static int clip_rect(
    const srapi_framebuffer_t *fb,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    int32_t *out_x0,
    int32_t *out_y0,
    int32_t *out_x1,
    int32_t *out_y1
) {
    int64_t x0;
    int64_t y0;
    int64_t x1;
    int64_t y1;

    if (fb == NULL || fb->pixels == NULL || width == 0 || height == 0) {
        return 0;
    }

    x0 = x;
    y0 = y;
    x1 = (int64_t)x + width;
    y1 = (int64_t)y + height;

    if (x1 <= 0 || y1 <= 0 || x0 >= fb->width || y0 >= fb->height) {
        return 0;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->width) x1 = fb->width;
    if (y1 > fb->height) y1 = fb->height;
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }

    *out_x0 = (int32_t)x0;
    *out_y0 = (int32_t)y0;
    *out_x1 = (int32_t)x1;
    *out_y1 = (int32_t)y1;
    return 1;
}

void srapi_render_clear(srapi_framebuffer_t *fb, srapi_color_t color) {
    uint64_t count;

    if (fb == NULL || fb->pixels == NULL) {
        return;
    }

    count = (uint64_t)fb->width;
    for (uint32_t y = 0; y < fb->height; y++) {
        uint32_t *row = fb->pixels + y * (fb->pitch / sizeof(uint32_t));
        for (uint64_t x = 0; x < count; x++) {
            row[x] = color;
        }
    }
}

void srapi_render_fill_rect(
    srapi_framebuffer_t *fb,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_color_t color
) {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;

    if (!clip_rect(fb, x, y, width, height, &x0, &y0, &x1, &y1)) {
        return;
    }

    for (int32_t py = y0; py < y1; py++) {
        uint32_t *row = fb->pixels + (uint32_t)py * (fb->pitch / sizeof(uint32_t));
        for (int32_t px = x0; px < x1; px++) {
            row[px] = color;
        }
    }
}

void srapi_render_draw_line(
    srapi_framebuffer_t *fb,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    srapi_color_t color
) {
    int64_t dx;
    int32_t sx;
    int64_t dy;
    int32_t sy;
    int64_t err;

    if (fb == NULL || fb->pixels == NULL) {
        return;
    }

    dx = x1 >= x0 ? (int64_t)x1 - x0 : (int64_t)x0 - x1;
    sx = x0 < x1 ? 1 : -1;
    dy = y1 >= y0 ? -((int64_t)y1 - y0) : -((int64_t)y0 - y1);
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        int64_t e2;

        put_pixel(fb, x0, y0, color);
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
}

static float absf_local(float v) {
    return v < 0.0f ? -v : v;
}

static int32_t round_to_i32(float v) {
    return (int32_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

static int32_t floor_to_i32(float v) {
    int32_t i = (int32_t)v;
    return v < (float)i ? i - 1 : i;
}

static int32_t ceil_to_i32(float v) {
    int32_t i = (int32_t)v;
    return v > (float)i ? i + 1 : i;
}

static uint8_t clamp_u8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

static srapi_color_t mix_color2(srapi_color_t a, srapi_color_t b, float t) {
    float aa = (float)((a >> 24) & 0xff);
    float ar = (float)((a >> 16) & 0xff);
    float ag = (float)((a >> 8) & 0xff);
    float ab = (float)(a & 0xff);
    float ba = (float)((b >> 24) & 0xff);
    float br = (float)((b >> 16) & 0xff);
    float bg = (float)((b >> 8) & 0xff);
    float bb = (float)(b & 0xff);

    return ((srapi_color_t)clamp_u8(aa + (ba - aa) * t) << 24) |
           ((srapi_color_t)clamp_u8(ar + (br - ar) * t) << 16) |
           ((srapi_color_t)clamp_u8(ag + (bg - ag) * t) << 8) |
           (srapi_color_t)clamp_u8(ab + (bb - ab) * t);
}

static srapi_color_t mix_color3(
    srapi_color_t a,
    srapi_color_t b,
    srapi_color_t c,
    float wa,
    float wb,
    float wc
) {
    float aa = (float)((a >> 24) & 0xff);
    float ar = (float)((a >> 16) & 0xff);
    float ag = (float)((a >> 8) & 0xff);
    float ab = (float)(a & 0xff);
    float ba = (float)((b >> 24) & 0xff);
    float br = (float)((b >> 16) & 0xff);
    float bg = (float)((b >> 8) & 0xff);
    float bb = (float)(b & 0xff);
    float ca = (float)((c >> 24) & 0xff);
    float cr = (float)((c >> 16) & 0xff);
    float cg = (float)((c >> 8) & 0xff);
    float cb = (float)(c & 0xff);

    return ((srapi_color_t)clamp_u8(aa * wa + ba * wb + ca * wc) << 24) |
           ((srapi_color_t)clamp_u8(ar * wa + br * wb + cr * wc) << 16) |
           ((srapi_color_t)clamp_u8(ag * wa + bg * wb + cg * wc) << 8) |
           (srapi_color_t)clamp_u8(ab * wa + bb * wb + cb * wc);
}

static float edgef(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static void draw_vertex_line(
    srapi_framebuffer_t *fb,
    const srapi_vertex_t *a,
    const srapi_vertex_t *b
) {
    int32_t x0 = round_to_i32(a->x);
    int32_t y0 = round_to_i32(a->y);
    int32_t x1 = round_to_i32(b->x);
    int32_t y1 = round_to_i32(b->y);
    int32_t dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 >= y0 ? y1 - y0 : y0 - y1;
    int32_t steps = dx > dy ? dx : dy;
    int64_t err;
    int32_t sx;
    int32_t sy;

    if (steps == 0) {
        put_pixel(fb, x0, y0, a->color);
        return;
    }

    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = (int64_t)dx - dy;

    for (int32_t step = 0;; step++) {
        float t = (float)step / (float)steps;
        int64_t e2;

        put_pixel(fb, x0, y0, mix_color2(a->color, b->color, t));
        if (x0 == x1 && y0 == y1) {
            break;
        }

        e2 = 2 * err;
        if (e2 > -(int64_t)dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_vertex_triangle(
    srapi_framebuffer_t *fb,
    const srapi_vertex_t *v0,
    const srapi_vertex_t *v1,
    const srapi_vertex_t *v2
) {
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    float area;

    if (fb == NULL || fb->pixels == NULL) {
        return;
    }

    min_x = floor_to_i32(v0->x < v1->x ? v0->x : v1->x);
    if (floor_to_i32(v2->x) < min_x) min_x = floor_to_i32(v2->x);
    max_x = ceil_to_i32(v0->x > v1->x ? v0->x : v1->x);
    if (ceil_to_i32(v2->x) > max_x) max_x = ceil_to_i32(v2->x);
    min_y = floor_to_i32(v0->y < v1->y ? v0->y : v1->y);
    if (floor_to_i32(v2->y) < min_y) min_y = floor_to_i32(v2->y);
    max_y = ceil_to_i32(v0->y > v1->y ? v0->y : v1->y);
    if (ceil_to_i32(v2->y) > max_y) max_y = ceil_to_i32(v2->y);

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int32_t)fb->width) max_x = (int32_t)fb->width - 1;
    if (max_y >= (int32_t)fb->height) max_y = (int32_t)fb->height - 1;
    if (min_x > max_x || min_y > max_y) {
        return;
    }

    area = edgef(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
    if (absf_local(area) < 0.0001f) {
        return;
    }

    for (int32_t y = min_y; y <= max_y; y++) {
        for (int32_t x = min_x; x <= max_x; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = edgef(v1->x, v1->y, v2->x, v2->y, px, py);
            float w1 = edgef(v2->x, v2->y, v0->x, v0->y, px, py);
            float w2 = edgef(v0->x, v0->y, v1->x, v1->y, px, py);

            if ((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f)) {
                put_pixel(fb, x, y, mix_color3(
                    v0->color, v1->color, v2->color,
                    w0 / area, w1 / area, w2 / area
                ));
            }
        }
    }
}

void srapi_render_fill_triangle(
    srapi_framebuffer_t *fb,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2,
    srapi_color_t color
) {
    srapi_vertex_t vertices[3] = {
        { (float)x0, (float)y0, color },
        { (float)x1, (float)y1, color },
        { (float)x2, (float)y2, color },
    };

    draw_vertex_triangle(fb, &vertices[0], &vertices[1], &vertices[2]);
}

void srapi_render_draw_vertices(
    srapi_framebuffer_t *fb,
    srapi_primitive_topology_t topology,
    const srapi_vertex_t *vertices,
    size_t vertex_count,
    const uint32_t *indices,
    size_t index_count
) {
    size_t count = indices != NULL ? index_count : vertex_count;

    if (fb == NULL || fb->pixels == NULL || vertices == NULL || count == 0) {
        return;
    }

#define VERTEX_AT(i) (&vertices[indices != NULL ? indices[(i)] : (i)])
    switch (topology) {
        case SRAPI_PRIMITIVE_POINTS:
            for (size_t i = 0; i < count; i++) {
                const srapi_vertex_t *v = VERTEX_AT(i);
                put_pixel(fb, round_to_i32(v->x), round_to_i32(v->y), v->color);
            }
            break;
        case SRAPI_PRIMITIVE_LINES:
            for (size_t i = 0; i + 1 < count; i += 2) {
                draw_vertex_line(fb, VERTEX_AT(i), VERTEX_AT(i + 1));
            }
            break;
        case SRAPI_PRIMITIVE_TRIANGLES:
            for (size_t i = 0; i + 2 < count; i += 3) {
                draw_vertex_triangle(fb, VERTEX_AT(i), VERTEX_AT(i + 1), VERTEX_AT(i + 2));
            }
            break;
        default:
            break;
    }
#undef VERTEX_AT
}

void srapi_render_draw_vertices_transformed(
    srapi_framebuffer_t *fb,
    srapi_primitive_topology_t topology,
    const srapi_vertex_t *vertices,
    size_t vertex_count,
    const uint32_t *indices,
    size_t index_count,
    const float *positions
) {
    srapi_vertex_t *tmp;
    size_t count = indices != NULL ? index_count : vertex_count;

    if (fb == NULL || fb->pixels == NULL || vertices == NULL || positions == NULL || count == 0) {
        return;
    }

    tmp = calloc(vertex_count, sizeof(*tmp));
    if (tmp == NULL) {
        return;
    }

    for (size_t i = 0; i < vertex_count; i++) {
        tmp[i] = vertices[i];
        tmp[i].x = positions[i * 2 + 0];
        tmp[i].y = positions[i * 2 + 1];
    }

    srapi_render_draw_vertices(fb, topology, tmp, vertex_count, indices, index_count);
    free(tmp);
}

void srapi_render_shade_rect(
    srapi_framebuffer_t *fb,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_shader_t *shader
) {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;

    if (shader == NULL || !clip_rect(fb, x, y, width, height, &x0, &y0, &x1, &y1)) {
        return;
    }

    srapi_debugf("cpu shade_rect clipped x=%d y=%d w=%d h=%d shader_insts=%zu",
                 x0, y0, x1 - x0, y1 - y0, shader->inst_count);

    for (int32_t py = y0; py < y1; py++) {
        uint32_t *row = fb->pixels + (uint32_t)py * (fb->pitch / sizeof(uint32_t));
        for (int32_t px = x0; px < x1; px++) {
            srapi_color_t color;
            float inputs[6];

            inputs[SRAPI_VM_INPUT_X] = (float)px;
            inputs[SRAPI_VM_INPUT_Y] = (float)py;
            inputs[SRAPI_VM_INPUT_U] = width > 1 ? (float)(px - x) / (float)(width - 1) : 0.0f;
            inputs[SRAPI_VM_INPUT_V] = height > 1 ? (float)(py - y) / (float)(height - 1) : 0.0f;
            inputs[SRAPI_VM_INPUT_WIDTH] = (float)width;
            inputs[SRAPI_VM_INPUT_HEIGHT] = (float)height;

            if (srapi_vm_run_fragment(shader, inputs, &color) == SRAPI_OK) {
                row[px] = color;
            }
        }
    }
}
