/*
 * Paint laid-out boxes into a ranal pixel surface.
 *
 * We don't use ranal_draw_* primitives because those are sent to the
 * widget tree and won't survive ranal_render's reset pass. Instead we
 * write straight into the surface buffer (same trick term.c uses) and
 * let main.c blit it onto the backbuffer each frame.
 */
#include "paint.h"

#include <ranal/ranal.h>
#include <string.h>
#include <stdlib.h>

#define COL_BG          RANAL_COLOR(245, 245, 245)
#define COL_TEXT        RANAL_COLOR( 24,  24,  28)
#define COL_HEADING     RANAL_COLOR( 12,  18,  32)
#define COL_HEADING2    RANAL_COLOR( 28,  32,  44)
#define COL_LINK        RANAL_COLOR( 30,  90, 200)
#define COL_LINK_FOCUS  RANAL_COLOR(200,  60,  20)
#define COL_CODE_FG     RANAL_COLOR( 30,  20,  80)
#define COL_CODE_BG     RANAL_COLOR(230, 230, 220)
#define COL_LIST_BULLET RANAL_COLOR( 80,  80,  80)
#define COL_RULE        RANAL_COLOR(180, 180, 180)
#define COL_LINK_BG     RANAL_COLOR(220, 232, 255)

static void surf_fill(uint32_t *px, int pitch_px, int sw, int sh,
                      int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= sw || y >= sh) return;
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *r = px + (size_t)(y + row) * pitch_px + x;
        for (int col = 0; col < w; col++) r[col] = color;
    }
}

static void draw_glyph_scaled(uint32_t *px, int pitch_px, int sw, int sh,
                              int x, int y, uint32_t cp,
                              uint32_t color, int scale) {
    const uint8_t *g = ranal_font_glyph_u(cp);
    if (g == NULL) return;
    for (int gy = 0; gy < RANAL_GLYPH_HEIGHT; gy++) {
        uint8_t bits = g[gy];
        for (int gx = 0; gx < RANAL_GLYPH_WIDTH; gx++) {
            if ((bits & (1u << (RANAL_GLYPH_WIDTH - 1 - gx))) == 0) continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + gy * scale + sy;
                if (py < 0 || py >= sh) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int xx = x + gx * scale + sx;
                    if (xx < 0 || xx >= sw) continue;
                    px[(size_t)py * pitch_px + xx] = color;
                }
            }
        }
    }
}

static int decode_utf8_one(const char *s, int len, uint32_t *cp) {
    if (len <= 0) { *cp = 0; return 0; }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((uint32_t)(c & 0x0F) << 12) |
              ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((uint32_t)(c & 0x07) << 18) |
              ((uint32_t)(s[1] & 0x3F) << 12) |
              ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static uint32_t style_color(br_style_t s, int focused_link, int is_focused) {
    switch (s) {
        case BR_STYLE_H1:
        case BR_STYLE_H2:        return COL_HEADING;
        case BR_STYLE_H3:        return COL_HEADING2;
        case BR_STYLE_LINK:      return (focused_link && is_focused) ? COL_LINK_FOCUS : COL_LINK;
        case BR_STYLE_CODE:
        case BR_STYLE_PRE:       return COL_CODE_FG;
        case BR_STYLE_LIST_BULLET: return COL_LIST_BULLET;
        default:                 return COL_TEXT;
    }
}

static int hits_grow(br_link_hits_t *h) {
    size_t want = h->cap == 0 ? 32 : h->cap * 2;
    br_link_rect_t *p = (br_link_rect_t *)realloc(h->rects, want * sizeof(*p));
    if (p == NULL) return -1;
    h->rects = p;
    h->cap = want;
    return 0;
}

static void hits_push(br_link_hits_t *h, int link, int x, int y, int w, int hh) {
    if (h->count == h->cap && hits_grow(h) != 0) return;
    h->rects[h->count].link_index = link;
    h->rects[h->count].x = x;
    h->rects[h->count].y = y;
    h->rects[h->count].w = w;
    h->rects[h->count].h = hh;
    h->count++;
}

void br_paint_page(void *surface_v,
                   const br_layout_t *layout,
                   int scroll_y,
                   int viewport_h,
                   int focused_link,
                   br_link_hits_t *hits) {
    if (surface_v == NULL || layout == NULL) return;
    ranal_surface_t *surface = (ranal_surface_t *)surface_v;
    uint32_t *px = ranal_surface_pixels(surface);
    if (px == NULL) return;
    int sw = ranal_surface_width(surface);
    int sh = ranal_surface_height(surface);
    int pitch_px = ranal_surface_pitch(surface) / 4;
    int vh = viewport_h > sh ? sh : viewport_h;

    /* Page background. */
    surf_fill(px, pitch_px, sw, sh, 0, 0, sw, vh, COL_BG);

    if (hits != NULL) hits->count = 0;

    for (size_t i = 0; i < layout->box_count; i++) {
        const br_box_t *b = &layout->boxes[i];
        int y0 = b->y - scroll_y;
        if (y0 + b->h < 0 || y0 >= vh) continue;

        int is_focused = (focused_link >= 0 && b->link_index == focused_link);

        if (b->style == BR_STYLE_NORMAL && b->text == NULL && b->w > 0 && b->h > 0) {
            /* horizontal rule */
            surf_fill(px, pitch_px, sw, sh, b->x, y0, b->w, b->h, COL_RULE);
            continue;
        }

        if (b->style == BR_STYLE_CODE || b->style == BR_STYLE_PRE) {
            surf_fill(px, pitch_px, sw, sh, b->x - 1, y0 - 1,
                      b->w + 2, b->h + 2, COL_CODE_BG);
        }
        if (b->style == BR_STYLE_LINK && is_focused) {
            surf_fill(px, pitch_px, sw, sh, b->x - 1, y0 - 1,
                      b->w + 2, b->h + 2, COL_LINK_BG);
        }

        uint32_t color = style_color(b->style, focused_link >= 0, is_focused);

        const char *t = b->text;
        int tlen = b->text_len;
        int x = b->x;
        int i_b = 0;
        while (i_b < tlen) {
            uint32_t cp;
            int n = decode_utf8_one(t + i_b, tlen - i_b, &cp);
            if (n <= 0) break;
            if (cp != ' ') {
                draw_glyph_scaled(px, pitch_px, sw, sh, x, y0, cp, color, b->scale);
                if (b->bold) {
                    draw_glyph_scaled(px, pitch_px, sw, sh, x + 1, y0, cp,
                                      color, b->scale);
                }
            }
            x += RANAL_FONT_ADVANCE_X * b->scale;
            i_b += n;
        }

        if (b->underline) {
            int uy = y0 + b->h;
            surf_fill(px, pitch_px, sw, sh, b->x, uy, b->w, 1, color);
        }

        if (hits != NULL && b->link_index >= 0) {
            hits_push(hits, b->link_index, b->x, y0, b->w, b->h);
        }
    }
}
