#define _GNU_SOURCE
#include "compositor.h"
#include "window.h"
#include "buffer/buffer.h"
#include "de/de.h"

#include <sprot/sprot.h>

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

static void draw_titlebar_chrome(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                                 const swm_state_t *swm, const swm_surface_t *s, int is_focused) {
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
    swm_titlebar_button_rects(swm, s, &bmin, &bmax, &bclose, &by);
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

void swm_mark_dirty_rect(swm_state_t *swm, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (x1 >= x2 || y1 >= y2) return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int32_t)swm->display_w) x2 = (int32_t)swm->display_w;
    if (y2 > (int32_t)swm->display_h) y2 = (int32_t)swm->display_h;
    if (x1 >= x2 || y1 >= y2) return;
    if (!swm->has_dirty_rect) {
        swm->dirty_x1 = x1;
        swm->dirty_y1 = y1;
        swm->dirty_x2 = x2;
        swm->dirty_y2 = y2;
        swm->has_dirty_rect = 1;
    } else {
        if (x1 < swm->dirty_x1) swm->dirty_x1 = x1;
        if (y1 < swm->dirty_y1) swm->dirty_y1 = y1;
        if (x2 > swm->dirty_x2) swm->dirty_x2 = x2;
        if (y2 > swm->dirty_y2) swm->dirty_y2 = y2;
    }
}

void swm_mark_dirty_surface_outer(swm_state_t *swm, const swm_surface_t *s) {
    if (s == NULL || !s->in_use) return;
    int32_t ox, oy, ow, oh;
    swm_surface_outer_rect(swm, s, &ox, &oy, &ow, &oh);
    swm_mark_dirty_rect(swm, ox, oy, ox + ow, oy + oh);
}

void swm_reset_dirty(swm_state_t *swm) {
    swm->has_dirty_rect = 0;
}

void swm_composite_surfaces(swm_state_t *swm, srapi_framebuffer_t *fb, uint32_t bg_color) {
    uint32_t *dst = srapi_framebuffer_pixels(fb);
    int32_t dst_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t dst_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t dst_pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
    if (dst == NULL) return;

    int cursor_moved = (swm->mouse_x == swm->prev_cursor_x && swm->mouse_y == swm->prev_cursor_y) ? 0 : 1;

    if (!swm->has_dirty_rect && !cursor_moved) {
        if (swm->de != NULL) de_render(swm->de, fb);
        return;
    }

    /* mark old and new cursor positions as dirty */
    if (cursor_moved) {
        int32_t cw = 12, ch = 16;
        swm_mark_dirty_rect(swm,
            swm->prev_cursor_x - 6, swm->prev_cursor_y - 10,
            swm->prev_cursor_x + cw, swm->prev_cursor_y + ch);
        swm_mark_dirty_rect(swm,
            swm->mouse_x - 6, swm->mouse_y - 10,
            swm->mouse_x + cw, swm->mouse_y + ch);
    }

    int32_t cx1 = swm->dirty_x1;
    int32_t cy1 = swm->dirty_y1;
    int32_t cx2 = swm->dirty_x2;
    int32_t cy2 = swm->dirty_y2;

    if (cx1 < 0) cx1 = 0;
    if (cy1 < 0) cy1 = 0;
    if (cx2 > dst_w) cx2 = dst_w;
    if (cy2 > dst_h) cy2 = dst_h;

    for (int32_t y = cy1; y < cy2; y++) {
        uint32_t *row = dst + (size_t)y * dst_pitch_px;
        for (int32_t x = cx1; x < cx2; x++) row[x] = bg_color;
    }

    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_collect_surfaces_z_asc(swm, list, SWM_MAX_SURFACES);
    swm_surface_t *focused = n > 0 ? list[n - 1] : NULL;

    for (int i = 0; i < n; i++) {
        swm_surface_t *s = list[i];
        if (s->role != SPROT_SURFACE_ROLE_POPUP) {
            draw_titlebar_chrome(dst, dst_w, dst_h, dst_pitch_px, swm, s, s == focused);
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

        if (ew == src_w && eh == src_h) {
            int32_t sx0 = 0, sy0 = 0;
            int32_t w = src_w, h = src_h;
            int32_t dx = ex, dy = ey;
            if (dx < 0) { sx0 = -dx; w -= sx0; dx = 0; }
            if (dy < 0) { sy0 = -dy; h -= sy0; dy = 0; }
            if (dx + w > dst_w) w = dst_w - dx;
            if (dy + h > dst_h) h = dst_h - dy;
            if (w <= 0 || h <= 0) continue;
            for (int32_t row = 0; row < h; row++) {
                const uint32_t *sr = src + (sy0 + row) * src_pitch_px + sx0;
                uint32_t *dr = dst + (dy + row) * dst_pitch_px + dx;
                memcpy(dr, sr, (size_t)w * 4u);
            }
        } else {
            int32_t dy0 = ey > 0 ? ey : 0;
            int32_t dy1 = ey + eh < dst_h ? ey + eh : dst_h;
            int32_t dx0 = ex > 0 ? ex : 0;
            int32_t dx1 = ex + ew < dst_w ? ex + ew : dst_w;
            if (ew <= 0 || eh <= 0) continue;
            for (int32_t dy = dy0; dy < dy1; dy++) {
                int32_t sy = (int32_t)(((int64_t)(dy - ey) * src_h) / eh);
                if (sy < 0) sy = 0; else if (sy >= src_h) sy = src_h - 1;
                const uint32_t *sr = src + sy * src_pitch_px;
                uint32_t *dr = dst + dy * dst_pitch_px;
                for (int32_t dx = dx0; dx < dx1; dx++) {
                    int32_t sx = (int32_t)(((int64_t)(dx - ex) * src_w) / ew);
                    if (sx < 0) sx = 0; else if (sx >= src_w) sx = src_w - 1;
                    dr[dx] = sr[sx];
                }
            }
        }
        if (began_read) swm_buffer_end_cpu_read(s->buffer);
    }
    if (swm->de != NULL) {
        de_render(swm->de, fb);
    }
    draw_cursor(dst, dst_w, dst_h, dst_pitch_px, swm->mouse_x, swm->mouse_y, swm->current_cursor);

    swm_reset_dirty(swm);
    swm->prev_cursor_x = swm->mouse_x;
    swm->prev_cursor_y = swm->mouse_y;
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
