#include "render.h"

#include "../buffer/buffer.h"
#include "../de/de.h"
#include "../surface/surface.h"

#include <string.h>

static void fill_rect(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst_w) w = dst_w - x;
    if (y + h > dst_h) h = dst_h - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t row = 0; row < h; row++) {
        uint32_t *dr = dst + (y + row) * dst_pitch_px + x;
        for (int32_t col = 0; col < w; col++) dr[col] = color;
    }
}

static const uint8_t SWM_FONT_5x7[95][7];

static void draw_glyph(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                       int32_t x, int32_t y, char c, uint32_t color) {
    int idx = (c >= 32 && c <= 126) ? (c - 32) : ('?' - 32);
    const uint8_t *g = SWM_FONT_5x7[idx];
    for (int gy = 0; gy < 7; gy++) {
        uint8_t row = g[gy];
        for (int gx = 0; gx < 5; gx++) {
            if (row & (1 << (4 - gx))) {
                int32_t px = x + gx, py = y + gy;
                if (px >= 0 && px < dst_w && py >= 0 && py < dst_h) {
                    dst[py * dst_pitch_px + px] = color;
                }
            }
        }
    }
}

static int32_t draw_text(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                         int32_t x, int32_t y, const char *s, uint32_t color, int32_t max_w) {
    int32_t cur = x;
    for (; *s; s++) {
        if (cur + 5 > x + max_w) break;
        draw_glyph(dst, dst_w, dst_h, dst_pitch_px, cur, y, *s, color);
        cur += 6;
    }
    return cur;
}

static void draw_cursor(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                        int32_t cx, int32_t cy, uint32_t cursor_type) {
    static const uint8_t arrow[16][12] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,1,1,1,1,0,0},
        {1,2,2,1,2,2,1,0,0,0,0,0},
        {1,2,1,0,1,2,2,1,0,0,0,0},
        {1,1,0,0,1,2,2,1,0,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,0,1,1,0,0,0,0},
    };
    static const uint8_t ibeam[16][12] = {
        {0,0,0,1,1,1,1,1,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,0,0,1,0,0,0,0,0,0},
        {0,0,0,1,1,1,1,1,0,0,0,0},
    };
    static const uint8_t hand[16][12] = {
        {0,0,0,0,1,1,0,0,0,0,0,0},
        {0,0,0,1,2,2,1,0,0,0,0,0},
        {0,0,0,1,2,2,1,0,0,0,0,0},
        {0,0,0,1,2,2,1,0,0,0,0,0},
        {0,0,0,1,2,2,1,1,1,0,0,0},
        {0,0,1,1,2,2,2,2,2,1,0,0},
        {0,1,2,2,2,2,2,2,2,2,1,0},
        {0,1,2,2,2,2,2,2,2,2,1,0},
        {1,2,2,2,2,2,2,2,2,2,1,0},
        {1,2,2,2,2,2,2,2,2,2,1,0},
        {1,2,2,2,2,2,2,2,2,2,1,0},
        {0,1,2,2,2,2,2,2,2,1,0,0},
        {0,0,1,2,2,2,2,2,2,1,0,0},
        {0,0,0,1,2,2,2,2,1,0,0,0},
        {0,0,0,0,1,2,2,1,0,0,0,0},
        {0,0,0,0,0,1,1,0,0,0,0,0},
    };
    const uint8_t (*sprite)[12] = arrow;
    int off_x = 0, off_y = 0;
    if (cursor_type == SPROT_CURSOR_IBEAM) { sprite = ibeam; off_x = -5; off_y = -8; }
    else if (cursor_type == SPROT_CURSOR_HAND) { sprite = hand; off_x = -4; off_y = 0; }

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 12; x++) {
            uint8_t v = sprite[y][x];
            if (v == 0) continue;
            int32_t px = cx + x + off_x, py = cy + y + off_y;
            if (px < 0 || px >= dst_w || py < 0 || py >= dst_h) continue;
            dst[py * dst_pitch_px + px] = (v == 1) ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

static void draw_cursor_image(swm_state_t *swm, uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                              int32_t cx, int32_t cy) {
    swm_buffer_t *buffer = swm->cursor_buffer;
    if (buffer == NULL || !swm->cursor_visible) return;
    if (swm_buffer_begin_cpu_read(buffer) != 0) return;

    const uint32_t *src = swm_buffer_pixels(buffer);
    int32_t src_w = (int32_t)swm_buffer_width(buffer);
    int32_t src_h = (int32_t)swm_buffer_height(buffer);
    int32_t src_pitch_px = (int32_t)(swm_buffer_stride(buffer) / 4u);
    int32_t x0 = cx - swm->cursor_hotspot_x;
    int32_t y0 = cy - swm->cursor_hotspot_y;

    if (src != NULL) {
        for (int32_t y = 0; y < src_h; y++) {
            int32_t dy = y0 + y;
            if (dy < 0 || dy >= dst_h) continue;
            const uint32_t *sr = src + y * src_pitch_px;
            uint32_t *dr = dst + dy * dst_pitch_px;
            for (int32_t x = 0; x < src_w; x++) {
                int32_t dx = x0 + x;
                if (dx < 0 || dx >= dst_w) continue;
                uint32_t sp = sr[x];
                uint32_t a = sp >> 24;
                if (a == 0) continue;
                if (a == 255) {
                    dr[dx] = sp;
                    continue;
                }
                uint32_t dp = dr[dx];
                uint32_t rb = (sp & 0x00FF00FFu) + (((dp & 0x00FF00FFu) * (255u - a)) >> 8);
                uint32_t g = (sp & 0x0000FF00u) + (((dp & 0x0000FF00u) * (255u - a)) >> 8);
                dr[dx] = 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
            }
        }
    }
    swm_buffer_end_cpu_read(buffer);
}

static uint32_t blend_premul_pixel(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 0) return dst;
    if (a == 255) return src;
    uint32_t inv = 255u - a;
    uint32_t rb = (src & 0x00FF00FFu) + (((dst & 0x00FF00FFu) * inv) >> 8);
    uint32_t g = (src & 0x0000FF00u) + (((dst & 0x0000FF00u) * inv) >> 8);
    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

static void draw_titlebar_chrome(swm_state_t *swm, uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                                 swm_surface_t *s, int is_focused) {
    uint32_t bar_color    = is_focused ? 0xFF3A6CB0u : 0xFF333742u;
    uint32_t border_color = is_focused ? 0xFF5AA0F0u : 0xFF4A4F5Bu;
    uint32_t text_color   = is_focused ? 0xFFFFFFFFu : 0xFFB5BAC4u;
    int32_t outer_x, outer_y, outer_w, outer_h;
    swm_surface_outer_rect(swm, s, &outer_x, &outer_y, &outer_w, &outer_h);

    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x, outer_y, outer_w, SWM_BORDER, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x, outer_y + outer_h - SWM_BORDER, outer_w, SWM_BORDER, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x, outer_y, SWM_BORDER, outer_h, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x + outer_w - SWM_BORDER, outer_y, SWM_BORDER, outer_h, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px,
              outer_x + SWM_BORDER, outer_y + SWM_BORDER,
              outer_w - 2 * SWM_BORDER, SWM_TITLEBAR_H, bar_color);

    int32_t bmin, bmax, bclose, by;
    swm_surface_titlebar_button_rects(swm, s, &bmin, &bmax, &bclose, &by);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, bmin,   by, SWM_BTN_SIZE, SWM_BTN_SIZE, 0xFFD0B040u);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, bmax,   by, SWM_BTN_SIZE, SWM_BTN_SIZE, 0xFF40C060u);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, bclose, by, SWM_BTN_SIZE, SWM_BTN_SIZE, 0xFFE05050u);

    int32_t title_x = outer_x + SWM_BORDER + 6;
    int32_t title_y = outer_y + SWM_BORDER + (SWM_TITLEBAR_H - 7) / 2;
    int32_t title_max = bmin - title_x - 6;
    if (title_max > 0) {
        draw_text(dst, dst_w, dst_h, dst_pitch_px, title_x, title_y, s->title, text_color, title_max);
    }
}

void swm_render_composite(swm_state_t *swm, srapi_framebuffer_t *fb, uint32_t bg_color) {
    uint32_t *dst = srapi_framebuffer_pixels(fb);
    int32_t dst_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t dst_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t dst_pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
    if (dst == NULL) return;

    for (int32_t y = 0; y < dst_h; y++) {
        uint32_t *row = dst + (size_t)y * dst_pitch_px;
        for (int32_t x = 0; x < dst_w; x++) row[x] = bg_color;
    }

    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_surface_collect_z_asc(swm, list, SWM_MAX_SURFACES);
    swm_surface_t *focused = swm_surface_topmost_window(swm);

    for (int i = 0; i < n; i++) {
        swm_surface_t *s = list[i];
        if (!swm_surface_role_is_child(s->role)) {
            draw_titlebar_chrome(swm, dst, dst_w, dst_h, dst_pitch_px, s, s == focused);
        }

        int began_read = 0;
        if (s->buffer == NULL) continue;
        int32_t ex, ey, ew, eh;
        swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
        int32_t src_pitch_px = (int32_t)(swm_buffer_stride(s->buffer) / 4u);
        const uint32_t *src = (const uint32_t *)swm_buffer_pixels(s->buffer);
        int32_t src_w = (int32_t)s->width;
        int32_t src_h = (int32_t)s->height;
        if (src == NULL) continue;
        if (swm_buffer_begin_cpu_read(s->buffer) != 0) continue;
        began_read = 1;

        (void)ew; (void)eh;
        int32_t sx0 = 0, sy0 = 0;
        int32_t w = src_w, h = src_h;
        int32_t dx = ex, dy = ey;
        if (dx < 0) { sx0 = -dx; w -= sx0; dx = 0; }
        if (dy < 0) { sy0 = -dy; h -= sy0; dy = 0; }
        if (dx + w > dst_w) w = dst_w - dx;
        if (dy + h > dst_h) h = dst_h - dy;
        if (w > 0 && h > 0) {
            int blend_child = swm_surface_role_is_child(s->role) && s->buffer_has_alpha;
            for (int32_t row = 0; row < h; row++) {
                const uint32_t *sr = src + (sy0 + row) * src_pitch_px + sx0;
                uint32_t *dr = dst + (dy + row) * dst_pitch_px + dx;
                if (blend_child) {
                    for (int32_t col = 0; col < w; col++) {
                        dr[col] = blend_premul_pixel(dr[col], sr[col]);
                    }
                } else {
                    memcpy(dr, sr, (size_t)w * 4u);
                }
            }
        }
        if (began_read) swm_buffer_end_cpu_read(s->buffer);
    }
    if (swm->de != NULL) {
        de_render(swm->de, fb);
    }
    if (swm->cursor_buffer != NULL && swm->cursor_visible) {
        draw_cursor_image(swm, dst, dst_w, dst_h, dst_pitch_px, swm->mouse_x, swm->mouse_y);
    } else {
        draw_cursor(dst, dst_w, dst_h, dst_pitch_px, swm->mouse_x, swm->mouse_y, swm->current_cursor);
    }
}

#define GS(a,b,c,d,e,f,g) { a,b,c,d,e,f,g }
static const uint8_t SWM_FONT_5x7[95][7] = {
    GS(0x00,0x00,0x00,0x00,0x00,0x00,0x00), GS(0x04,0x04,0x04,0x04,0x04,0x00,0x04),
    GS(0x0A,0x0A,0x00,0x00,0x00,0x00,0x00), GS(0x0A,0x1F,0x0A,0x0A,0x0A,0x1F,0x0A),
    GS(0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04), GS(0x19,0x19,0x02,0x04,0x08,0x13,0x13),
    GS(0x0C,0x12,0x14,0x08,0x15,0x12,0x0D), GS(0x04,0x04,0x00,0x00,0x00,0x00,0x00),
    GS(0x02,0x04,0x08,0x08,0x08,0x04,0x02), GS(0x08,0x04,0x02,0x02,0x02,0x04,0x08),
    GS(0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00), GS(0x00,0x04,0x04,0x1F,0x04,0x04,0x00),
    GS(0x00,0x00,0x00,0x00,0x00,0x04,0x08), GS(0x00,0x00,0x00,0x1F,0x00,0x00,0x00),
    GS(0x00,0x00,0x00,0x00,0x00,0x00,0x04), GS(0x01,0x02,0x02,0x04,0x08,0x08,0x10),
    GS(0x0E,0x11,0x13,0x15,0x19,0x11,0x0E), GS(0x04,0x0C,0x04,0x04,0x04,0x04,0x0E),
    GS(0x0E,0x11,0x01,0x02,0x04,0x08,0x1F), GS(0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E),
    GS(0x02,0x06,0x0A,0x12,0x1F,0x02,0x02), GS(0x1F,0x10,0x1E,0x01,0x01,0x01,0x1E),
    GS(0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E), GS(0x1F,0x01,0x02,0x04,0x08,0x10,0x10),
    GS(0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E), GS(0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E),
    GS(0x00,0x04,0x00,0x00,0x00,0x04,0x00), GS(0x00,0x04,0x00,0x00,0x00,0x04,0x08),
    GS(0x01,0x02,0x04,0x08,0x04,0x02,0x01), GS(0x00,0x00,0x1F,0x00,0x1F,0x00,0x00),
    GS(0x10,0x08,0x04,0x02,0x04,0x08,0x10), GS(0x0E,0x11,0x01,0x02,0x04,0x00,0x04),
    GS(0x0E,0x11,0x17,0x15,0x17,0x10,0x0E), GS(0x0E,0x11,0x11,0x1F,0x11,0x11,0x11),
    GS(0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E), GS(0x0F,0x10,0x10,0x10,0x10,0x10,0x0F),
    GS(0x1E,0x11,0x11,0x11,0x11,0x11,0x1E), GS(0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F),
    GS(0x1F,0x10,0x10,0x1E,0x10,0x10,0x10), GS(0x0F,0x10,0x10,0x13,0x11,0x11,0x0F),
    GS(0x11,0x11,0x11,0x1F,0x11,0x11,0x11), GS(0x0E,0x04,0x04,0x04,0x04,0x04,0x0E),
    GS(0x01,0x01,0x01,0x01,0x01,0x11,0x0E), GS(0x11,0x12,0x14,0x18,0x14,0x12,0x11),
    GS(0x10,0x10,0x10,0x10,0x10,0x10,0x1F), GS(0x11,0x1B,0x15,0x15,0x11,0x11,0x11),
    GS(0x11,0x19,0x15,0x13,0x11,0x11,0x11), GS(0x0E,0x11,0x11,0x11,0x11,0x11,0x0E),
    GS(0x1E,0x11,0x11,0x1E,0x10,0x10,0x10), GS(0x0E,0x11,0x11,0x11,0x15,0x12,0x0D),
    GS(0x1E,0x11,0x11,0x1E,0x14,0x12,0x11), GS(0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E),
    GS(0x1F,0x04,0x04,0x04,0x04,0x04,0x04), GS(0x11,0x11,0x11,0x11,0x11,0x11,0x0E),
    GS(0x11,0x11,0x11,0x11,0x11,0x0A,0x04), GS(0x11,0x11,0x11,0x11,0x15,0x15,0x0A),
    GS(0x11,0x11,0x0A,0x04,0x0A,0x11,0x11), GS(0x11,0x11,0x0A,0x04,0x04,0x04,0x04),
    GS(0x1F,0x01,0x02,0x04,0x08,0x10,0x1F), GS(0x0E,0x08,0x08,0x08,0x08,0x08,0x0E),
    GS(0x10,0x08,0x08,0x04,0x02,0x02,0x01), GS(0x0E,0x02,0x02,0x02,0x02,0x02,0x0E),
    GS(0x04,0x0A,0x11,0x00,0x00,0x00,0x00), GS(0x00,0x00,0x00,0x00,0x00,0x00,0x1F),
    GS(0x08,0x04,0x00,0x00,0x00,0x00,0x00), GS(0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F),
    GS(0x10,0x10,0x1E,0x11,0x11,0x11,0x1E), GS(0x00,0x00,0x0F,0x10,0x10,0x10,0x0F),
    GS(0x01,0x01,0x0F,0x11,0x11,0x11,0x0F), GS(0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E),
    GS(0x06,0x09,0x08,0x1C,0x08,0x08,0x08), GS(0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E),
    GS(0x10,0x10,0x1E,0x11,0x11,0x11,0x11), GS(0x04,0x00,0x0C,0x04,0x04,0x04,0x0E),
    GS(0x02,0x00,0x06,0x02,0x02,0x12,0x0C), GS(0x10,0x10,0x12,0x14,0x18,0x14,0x12),
    GS(0x0C,0x04,0x04,0x04,0x04,0x04,0x0E), GS(0x00,0x00,0x1A,0x15,0x15,0x15,0x15),
    GS(0x00,0x00,0x1E,0x11,0x11,0x11,0x11), GS(0x00,0x00,0x0E,0x11,0x11,0x11,0x0E),
    GS(0x00,0x00,0x1E,0x11,0x1E,0x10,0x10), GS(0x00,0x00,0x0F,0x11,0x0F,0x01,0x01),
    GS(0x00,0x00,0x16,0x19,0x10,0x10,0x10), GS(0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E),
    GS(0x08,0x08,0x1C,0x08,0x08,0x09,0x06), GS(0x00,0x00,0x11,0x11,0x11,0x11,0x0F),
    GS(0x00,0x00,0x11,0x11,0x11,0x0A,0x04), GS(0x00,0x00,0x11,0x11,0x15,0x15,0x0A),
    GS(0x00,0x00,0x11,0x0A,0x04,0x0A,0x11), GS(0x00,0x00,0x11,0x11,0x0F,0x01,0x0E),
    GS(0x00,0x00,0x1F,0x02,0x04,0x08,0x1F), GS(0x02,0x04,0x04,0x08,0x04,0x04,0x02),
    GS(0x04,0x04,0x04,0x04,0x04,0x04,0x04), GS(0x08,0x04,0x04,0x02,0x04,0x04,0x08),
    GS(0x09,0x15,0x12,0x00,0x00,0x00,0x00),
};
