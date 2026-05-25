/*
 * Chrome: URL bar at the top, status line at the bottom. Drawn into a
 * dedicated chrome surface so we can refresh it cheaply without re-laying
 * out the page below.
 */
#include "chrome.h"

#include <ranal/ranal.h>
#include <string.h>

#define COL_CHROME_BG      RANAL_COLOR( 36,  40,  48)
#define COL_CHROME_BORDER  RANAL_COLOR( 18,  20,  26)
#define COL_URLBAR_BG      RANAL_COLOR(220, 222, 230)
#define COL_URLBAR_BG_HOT  RANAL_COLOR(255, 255, 255)
#define COL_URLBAR_TEXT    RANAL_COLOR( 22,  24,  30)
#define COL_URLBAR_CURSOR  RANAL_COLOR( 30,  90, 200)
#define COL_STATUS_BG      RANAL_COLOR( 22,  24,  30)
#define COL_STATUS_TEXT    RANAL_COLOR(190, 195, 205)
#define COL_HINT           RANAL_COLOR(150, 155, 170)

static void surf_fill(uint32_t *px, int pitch, int sw, int sh,
                      int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= sw || y >= sh) return;
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *r = px + (size_t)(y + row) * pitch + x;
        for (int col = 0; col < w; col++) r[col] = color;
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

static void draw_glyph(uint32_t *px, int pitch, int sw, int sh,
                       int x, int y, uint32_t cp, uint32_t color) {
    const uint8_t *g = ranal_font_glyph_u(cp);
    if (g == NULL) return;
    for (int gy = 0; gy < RANAL_GLYPH_HEIGHT; gy++) {
        uint8_t bits = g[gy];
        int py = y + gy;
        if (py < 0 || py >= sh) continue;
        for (int gx = 0; gx < RANAL_GLYPH_WIDTH; gx++) {
            if ((bits & (1u << (RANAL_GLYPH_WIDTH - 1 - gx))) == 0) continue;
            int xx = x + gx;
            if (xx < 0 || xx >= sw) continue;
            px[(size_t)py * pitch + xx] = color;
        }
    }
}

static int draw_text(uint32_t *px, int pitch, int sw, int sh,
                     int x, int y, const char *s, int max_w, uint32_t color) {
    int i = 0;
    int x_start = x;
    int tlen = (int)strlen(s);
    while (i < tlen) {
        uint32_t cp;
        int n = decode_utf8_one(s + i, tlen - i, &cp);
        if (n <= 0) break;
        if (x - x_start + RANAL_FONT_ADVANCE_X > max_w) break;
        if (cp != ' ') {
            draw_glyph(px, pitch, sw, sh, x, y, cp, color);
        }
        x += RANAL_FONT_ADVANCE_X;
        i += n;
    }
    return x - x_start;
}

void br_chrome_paint(void *surface_v, const browser_app_t *app,
                     int width, int loading) {
    ranal_surface_t *surface = (ranal_surface_t *)surface_v;
    if (surface == NULL) return;
    uint32_t *px = ranal_surface_pixels(surface);
    if (px == NULL) return;
    int sw = ranal_surface_width(surface);
    int sh = ranal_surface_height(surface);
    int pitch = ranal_surface_pitch(surface) / 4;

    /* Top chrome strip. */
    surf_fill(px, pitch, sw, sh, 0, 0, sw, BROWSER_CHROME_HEIGHT, COL_CHROME_BG);
    surf_fill(px, pitch, sw, sh, 0, BROWSER_CHROME_HEIGHT - 1, sw, 1,
              COL_CHROME_BORDER);

    int bar_x = 12;
    int bar_y = 14;
    int bar_h = BROWSER_CHROME_HEIGHT - 24;
    int bar_w = width - 24;
    uint32_t bar_bg = app->url_focused ? COL_URLBAR_BG_HOT : COL_URLBAR_BG;
    surf_fill(px, pitch, sw, sh, bar_x, bar_y, bar_w, bar_h, bar_bg);
    surf_fill(px, pitch, sw, sh, bar_x, bar_y, bar_w, 1, COL_CHROME_BORDER);
    surf_fill(px, pitch, sw, sh, bar_x, bar_y + bar_h - 1, bar_w, 1, COL_CHROME_BORDER);
    surf_fill(px, pitch, sw, sh, bar_x, bar_y, 1, bar_h, COL_CHROME_BORDER);
    surf_fill(px, pitch, sw, sh, bar_x + bar_w - 1, bar_y, 1, bar_h, COL_CHROME_BORDER);

    /* Loading indicator on the left of the URL bar. */
    const char *spinner_chars = "|/-\\";
    static int spin_phase = 0;
    char spin[2] = { spinner_chars[(spin_phase / 4) % 4], '\0' };
    if (loading) spin_phase++;
    const char *prefix = loading ? spin : "›";
    int text_x = bar_x + 6;
    int text_y = bar_y + (bar_h - RANAL_GLYPH_HEIGHT) / 2;
    text_x += draw_text(px, pitch, sw, sh, text_x, text_y, prefix, 12,
                        COL_HINT);
    text_x += 4;

    /* URL text (edit buffer if focused, otherwise current url). */
    const char *show = app->url_focused ? app->url_edit : app->url;
    int max_text_w = bar_w - (text_x - bar_x) - 8;
    draw_text(px, pitch, sw, sh, text_x, text_y, show, max_text_w,
              COL_URLBAR_TEXT);

    if (app->url_focused) {
        /* Approximate cursor x by advancing per codepoint up to cursor pos. */
        int cx = text_x;
        const char *t = app->url_edit;
        size_t cur = app->url_cursor;
        size_t i = 0;
        while (i < cur && t[i] != '\0') {
            uint32_t cp;
            int n = decode_utf8_one(t + i, (int)(cur - i), &cp);
            if (n <= 0) break;
            cx += RANAL_FONT_ADVANCE_X;
            i += (size_t)n;
        }
        surf_fill(px, pitch, sw, sh, cx, text_y - 1, 1,
                  RANAL_GLYPH_HEIGHT + 2, COL_URLBAR_CURSOR);
    }

    /* Status bar at the bottom is drawn separately when the page sits
       above the bar. For simplicity we share this surface only for the
       chrome strip; status is drawn by main into another small surface. */
    (void)app;
}
