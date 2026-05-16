#include "internal.h"

#include <stdlib.h>

static void put_pixel(srapi_framebuffer_t *fb, int32_t x, int32_t y, srapi_color_t color) {
    if (x < 0 || y < 0 || x >= (int32_t)fb->width || y >= (int32_t)fb->height) {
        return;
    }
    fb->pixels[(uint32_t)y * (fb->pitch / sizeof(uint32_t)) + (uint32_t)x] = color;
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

    if (fb == NULL || fb->pixels == NULL || width == 0 || height == 0) {
        return;
    }

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + (int32_t)width;
    y1 = y + (int32_t)height;
    if (x1 > (int32_t)fb->width) x1 = (int32_t)fb->width;
    if (y1 > (int32_t)fb->height) y1 = (int32_t)fb->height;
    if (x0 >= x1 || y0 >= y1) {
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
    int32_t dx;
    int32_t sx;
    int32_t dy;
    int32_t sy;
    int32_t err;

    if (fb == NULL || fb->pixels == NULL) {
        return;
    }

    dx = abs(x1 - x0);
    sx = x0 < x1 ? 1 : -1;
    dy = -abs(y1 - y0);
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        int32_t e2;

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

static int32_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
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
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    int32_t area;

    if (fb == NULL || fb->pixels == NULL) {
        return;
    }

    min_x = x0 < x1 ? x0 : x1;
    if (x2 < min_x) min_x = x2;
    max_x = x0 > x1 ? x0 : x1;
    if (x2 > max_x) max_x = x2;
    min_y = y0 < y1 ? y0 : y1;
    if (y2 < min_y) min_y = y2;
    max_y = y0 > y1 ? y0 : y1;
    if (y2 > max_y) max_y = y2;

    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= (int32_t)fb->width) max_x = (int32_t)fb->width - 1;
    if (max_y >= (int32_t)fb->height) max_y = (int32_t)fb->height - 1;
    if (min_x > max_x || min_y > max_y) {
        return;
    }

    area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) {
        return;
    }

    for (int32_t y = min_y; y <= max_y; y++) {
        for (int32_t x = min_x; x <= max_x; x++) {
            int32_t w0 = edge(x1, y1, x2, y2, x, y);
            int32_t w1 = edge(x2, y2, x0, y0, x, y);
            int32_t w2 = edge(x0, y0, x1, y1, x, y);

            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                put_pixel(fb, x, y, color);
            }
        }
    }
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

    if (fb == NULL || fb->pixels == NULL || shader == NULL || width == 0 || height == 0) {
        return;
    }

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + (int32_t)width;
    y1 = y + (int32_t)height;
    if (x1 > (int32_t)fb->width) x1 = (int32_t)fb->width;
    if (y1 > (int32_t)fb->height) y1 = (int32_t)fb->height;
    if (x0 >= x1 || y0 >= y1) {
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
