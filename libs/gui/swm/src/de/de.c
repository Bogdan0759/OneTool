/*
 * Minimal desktop environment for the SWM compositor.
 *
 * Renders a taskbar (launcher + window list + clock) using the ranal GUI
 * library via an offscreen ranal_context. Each frame the DE updates its
 * widgets, asks ranal to redraw into an offscreen framebuffer and then
 * blits the result onto swm's compositor backbuffer.
 *
 * The swm process never calls ranal_init(); we drive ranal exclusively via
 * the offscreen-context API which keeps its own srapi context, command
 * buffer and framebuffer.
 */

#define _GNU_SOURCE
#include "de.h"

#include <ranal/ranal.h>
#include <srapi/srapi.h>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    DE_MENU_TERM = 0,
    DE_MENU_ABOUT,
    DE_MENU_TILE,
    DE_MENU_CASCADE,
    DE_MENU_MINIMIZE_ALL,
    DE_MENU_QUIT,
} de_menu_action_t;

static const char *k_menu_labels[DE_MENU_ENTRIES] = {
    "Term",
    "About system",
    "Tile windows",
    "Cascade",
    "Minimize all",
    "Quit SWM",
};

struct de {
    swm_state_t *swm;

    int32_t display_w;
    int32_t display_h;
    int32_t taskbar_top;

    /* Pre-computed hit rectangles in screen coordinates. */
    int32_t launcher_x, launcher_y, launcher_w, launcher_h;
    int32_t stats_x, stats_y, stats_w, stats_h;
    int32_t tick_x, tick_y, tick_w, tick_h;
    int32_t clock_x, clock_y, clock_w, clock_h;
    int32_t items_origin_x;
    int32_t item_w;
    int32_t item_y;
    int32_t item_h;

    de_item_t items[DE_MAX_ITEMS];
    int       item_count;

    int  launcher_hover;
    int  launcher_pressed;
    int  hover_item;
    int  pressed_item;

    int  menu_open;
    int  menu_hover;
    int  menu_x, menu_y, menu_w, menu_h;

    char clock_text[16];
    uint64_t last_clock_minute;
    char stats_text[40];
    struct timespec last_stats_time;
    int have_stats_time;
    uint64_t tick_counter;
    uint64_t prev_cpu_total;
    uint64_t prev_cpu_idle;
    int have_cpu_sample;

    /* Per-item tooltip fade [0..1]. 1.0 = fully visible, 0.0 = hidden. */
    float    item_tooltip_alpha[DE_MAX_ITEMS];
    struct timespec last_tick_time;
    int      have_last_tick;

    /* ranal offscreen rendering plumbing. */
    ranal_context_t *taskbar_ctx;
    ranal_widget_t  *taskbar_root;
    ranal_widget_t  *launcher_panel;
    ranal_widget_t  *launcher_label;
    ranal_widget_t  *clock_label;
    ranal_widget_t  *item_panels[DE_MAX_ITEMS];
    ranal_widget_t  *item_labels[DE_MAX_ITEMS];

    ranal_context_t *menu_ctx;
    ranal_widget_t  *menu_root;
    ranal_widget_t  *menu_entry_panels[DE_MENU_ENTRIES];
    ranal_widget_t  *menu_entry_labels[DE_MENU_ENTRIES];

    int  taskbar_dirty;
    int  menu_dirty;
};

/* Theme colors – picked to mesh with ranal_theme_dark / swm chrome. */
#define COL_BAR_BG       RANAL_COLOR(18, 20, 26)
#define COL_BAR_TOP      RANAL_COLOR(44, 58, 80)
#define COL_BAR_BORDER   RANAL_COLOR(70, 100, 150)

#define COL_BTN_IDLE     RANAL_COLOR(38, 44, 58)
#define COL_BTN_HOVER    RANAL_COLOR(58, 70, 92)
#define COL_BTN_PRESSED  RANAL_COLOR(90, 130, 200)
#define COL_BTN_ACTIVE   RANAL_COLOR(60, 110, 180)
#define COL_BTN_DIM      RANAL_COLOR(24, 26, 32)

#define COL_TEXT_BRIGHT  RANAL_COLOR(230, 232, 240)
#define COL_TEXT_DIM     RANAL_COLOR(140, 145, 158)
#define COL_TEXT_ACCENT  RANAL_COLOR(120, 200, 255)

#define COL_MENU_BG      RANAL_COLOR(30, 34, 44)
#define COL_MENU_HOVER   RANAL_COLOR(60, 110, 180)
#define COL_MENU_BORDER  RANAL_COLOR(70, 100, 150)

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double elapsed_seconds_since(struct timespec *prev, int *have_prev) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = 0.0;
    if (*have_prev) {
        dt = (double)(now.tv_sec - prev->tv_sec) +
             (double)(now.tv_nsec - prev->tv_nsec) * 1e-9;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.25) dt = 0.25;  /* clamp huge stalls so animation doesn't jump */
    }
    *prev = now;
    *have_prev = 1;
    return dt;
}

static double timespec_delta_seconds(const struct timespec *older,
                                     const struct timespec *newer) {
    return (double)(newer->tv_sec - older->tv_sec) +
           (double)(newer->tv_nsec - older->tv_nsec) * 1e-9;
}

/*
 * Alpha-blend a 0xAARRGGBB source into a 0xAARRGGBB destination pixel buffer.
 * The compositor uses pre-clear opaque pixels everywhere so the destination
 * alpha is treated as 1.0. `alpha` is the 0..255 source coverage.
 */
static uint32_t blend_pixel(uint32_t dst, uint32_t src, uint32_t alpha) {
    if (alpha == 0u) return dst;
    if (alpha >= 255u) return (0xFF000000u | (src & 0x00FFFFFFu));
    uint32_t inv = 255u - alpha;
    uint32_t sr = (src >> 16) & 0xFFu;
    uint32_t sg = (src >> 8)  & 0xFFu;
    uint32_t sb =  src        & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8)  & 0xFFu;
    uint32_t db =  dst        & 0xFFu;
    uint32_t r = (sr * alpha + dr * inv + 127u) / 255u;
    uint32_t g = (sg * alpha + dg * inv + 127u) / 255u;
    uint32_t b = (sb * alpha + db * inv + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void fb_blend_rect(struct srapi_framebuffer *fb,
                          int32_t x, int32_t y, int32_t w, int32_t h,
                          ranal_color_t color, uint32_t alpha) {
    if (fb == NULL || w <= 0 || h <= 0 || alpha == 0u) return;
    uint32_t *dst = srapi_framebuffer_pixels(fb);
    if (dst == NULL) return;
    int32_t fb_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t fb_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t row = 0; row < h; row++) {
        uint32_t *dr = dst + (size_t)(y + row) * pitch_px + x;
        for (int32_t col = 0; col < w; col++) {
            dr[col] = blend_pixel(dr[col], color, alpha);
        }
    }
}

static void fb_blend_glyph(struct srapi_framebuffer *fb,
                           int32_t x, int32_t y, char c,
                           ranal_color_t color, uint32_t alpha) {
    const uint8_t *g = ranal_font_glyph(c);
    if (g == NULL) return;
    uint32_t *dst = srapi_framebuffer_pixels(fb);
    if (dst == NULL) return;
    int32_t fb_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t fb_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
    for (int gy = 0; gy < RANAL_GLYPH_HEIGHT; gy++) {
        uint8_t row = g[gy];
        for (int gx = 0; gx < RANAL_GLYPH_WIDTH; gx++) {
            if ((row & (1u << (RANAL_GLYPH_WIDTH - 1 - gx))) == 0) continue;
            int32_t px = x + gx;
            int32_t py = y + gy;
            if (px < 0 || px >= fb_w || py < 0 || py >= fb_h) continue;
            uint32_t *p = dst + (size_t)py * pitch_px + px;
            *p = blend_pixel(*p, color, alpha);
        }
    }
}

static void fb_blend_text(struct srapi_framebuffer *fb,
                          int32_t x, int32_t y, const char *text,
                          ranal_color_t color, uint32_t alpha) {
    if (text == NULL) return;
    int32_t cur = x;
    for (const char *p = text; *p != '\0'; p++) {
        fb_blend_glyph(fb, cur, y, *p, color, alpha);
        cur += RANAL_FONT_ADVANCE_X;
    }
}

static void compute_layout(de_t *de) {
    de->taskbar_top = de->display_h - DE_TASKBAR_H;
    de->launcher_x = DE_TASK_PADDING;
    de->launcher_y = de->taskbar_top + DE_TASK_PADDING;
    de->launcher_w = DE_LAUNCHER_W;
    de->launcher_h = DE_TASKBAR_H - 2 * DE_TASK_PADDING;

    de->clock_w = DE_CLOCK_W;
    de->clock_h = DE_TASKBAR_H - 2 * DE_TASK_PADDING;
    de->clock_x = de->display_w - DE_CLOCK_W - DE_TASK_PADDING;
    de->clock_y = de->taskbar_top + DE_TASK_PADDING;

    de->tick_w = DE_TICK_W;
    de->tick_h = DE_TASKBAR_H - 2 * DE_TASK_PADDING;
    de->tick_x = de->clock_x - DE_TICK_W - DE_TASK_PADDING;
    de->tick_y = de->taskbar_top + DE_TASK_PADDING;

    de->stats_w = DE_STATS_W;
    de->stats_h = DE_TASKBAR_H - 2 * DE_TASK_PADDING;
    de->stats_x = de->tick_x - DE_STATS_W - DE_TASK_PADDING;
    de->stats_y = de->taskbar_top + DE_TASK_PADDING;

    de->items_origin_x = de->launcher_x + de->launcher_w + DE_TASK_PADDING * 2;
    int32_t items_avail = de->stats_x - de->items_origin_x - DE_TASK_PADDING;
    if (items_avail < 0) items_avail = 0;
    /* Compute item width that fits DE_MAX_ITEMS slots, clamped to min/max. */
    int32_t slots = DE_MAX_ITEMS;
    int32_t w_each = (items_avail - (slots - 1) * DE_ITEM_GAP) / slots;
    if (w_each > DE_ITEM_W_MAX) w_each = DE_ITEM_W_MAX;
    if (w_each < DE_ITEM_W_MIN) w_each = DE_ITEM_W_MIN;
    de->item_w = w_each;
    de->item_y = de->taskbar_top + DE_TASK_PADDING;
    de->item_h = DE_TASKBAR_H - 2 * DE_TASK_PADDING;

    de->menu_w = DE_MENU_W;
    de->menu_h = DE_MENU_ITEM_H * DE_MENU_ENTRIES + 8;
    de->menu_x = de->launcher_x;
    de->menu_y = de->taskbar_top - de->menu_h - 4;
    if (de->menu_y < 0) de->menu_y = 0;
}

static void format_clock(de_t *de) {
    time_t t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    uint64_t minute_id = (uint64_t)tm_now.tm_year * 60u * 24u * 366u
                       + (uint64_t)tm_now.tm_yday * 60u * 24u
                       + (uint64_t)tm_now.tm_hour * 60u
                       + (uint64_t)tm_now.tm_min;
    if (minute_id == de->last_clock_minute && de->clock_text[0] != '\0') return;
    de->last_clock_minute = minute_id;
    snprintf(de->clock_text, sizeof(de->clock_text), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    if (de->clock_label != NULL) {
        ranal_set_text(de->clock_label, de->clock_text);
        de->taskbar_dirty = 1;
    }
}

static int read_cpu_times(uint64_t *total_out, uint64_t *idle_out) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL) return 0;

    char line[256];
    int ok = 0;
    if (fgets(line, sizeof(line), f) != NULL) {
        unsigned long long user = 0, nice = 0, system = 0, idle = 0;
        unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
        int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &idle,
                       &iowait, &irq, &softirq, &steal);
        if (n >= 4) {
            uint64_t total = (uint64_t)(user + nice + system + idle);
            if (n > 4) total += (uint64_t)iowait;
            if (n > 5) total += (uint64_t)irq;
            if (n > 6) total += (uint64_t)softirq;
            if (n > 7) total += (uint64_t)steal;
            *total_out = total;
            *idle_out = (uint64_t)(idle + (n > 4 ? iowait : 0));
            ok = 1;
        }
    }
    fclose(f);
    return ok;
}

static int read_mem_percent(int *percent_out) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) return 0;

    char line[256];
    unsigned long long total = 0;
    unsigned long long available = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        sscanf(line, "MemTotal: %llu kB", &total);
        sscanf(line, "MemAvailable: %llu kB", &available);
        if (total > 0 && available > 0) break;
    }
    fclose(f);

    if (total == 0 || available > total) return 0;
    unsigned long long used = total - available;
    int pct = (int)((used * 100ull + total / 2ull) / total);
    *percent_out = clampi(pct, 0, 100);
    return 1;
}

static void format_system_stats(de_t *de) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (de->have_stats_time && de->stats_text[0] != '\0' &&
        timespec_delta_seconds(&de->last_stats_time, &now) < 0.5) {
        return;
    }
    de->last_stats_time = now;
    de->have_stats_time = 1;

    int cpu_tenths = -1;
    uint64_t total = 0;
    uint64_t idle = 0;
    if (read_cpu_times(&total, &idle)) {
        if (de->have_cpu_sample && total > de->prev_cpu_total) {
            uint64_t delta_total = total - de->prev_cpu_total;
            uint64_t delta_idle = (idle >= de->prev_cpu_idle) ? idle - de->prev_cpu_idle : 0u;
            uint64_t busy = (delta_total > delta_idle) ? delta_total - delta_idle : 0u;
            cpu_tenths = (int)((busy * 1000u + delta_total / 2u) / delta_total);
            cpu_tenths = clampi(cpu_tenths, 0, 1000);
        }
        de->prev_cpu_total = total;
        de->prev_cpu_idle = idle;
        de->have_cpu_sample = 1;
    }

    int mem_percent = -1;
    if (!read_mem_percent(&mem_percent)) mem_percent = -1;

    char buf[sizeof(de->stats_text)];
    if (cpu_tenths >= 0 && mem_percent >= 0) {
        snprintf(buf, sizeof(buf), "CPU %d.%d%% RAM %d%%",
                 cpu_tenths / 10, cpu_tenths % 10, mem_percent);
    } else if (cpu_tenths >= 0) {
        snprintf(buf, sizeof(buf), "CPU %d.%d%% RAM --",
                 cpu_tenths / 10, cpu_tenths % 10);
    } else if (mem_percent >= 0) {
        snprintf(buf, sizeof(buf), "CPU --.-%% RAM %d%%", mem_percent);
    } else {
        snprintf(buf, sizeof(buf), "CPU --.-%% RAM --");
    }

    if (strcmp(buf, de->stats_text) != 0) {
        snprintf(de->stats_text, sizeof(de->stats_text), "%s", buf);
        de->taskbar_dirty = 1;
    }
}

static ranal_widget_t *build_panel_button(ranal_widget_t *root,
                                          int32_t x, int32_t y,
                                          int32_t w, int32_t h,
                                          ranal_color_t bg) {
    ranal_widget_t *p = ranal_panel(root);
    if (p == NULL) return NULL;
    ranal_set_pos(p, x, y);
    ranal_set_size(p, w, h);
    ranal_set_background(p, bg);
    return p;
}

static void build_taskbar_widgets(de_t *de) {
    ranal_context_t *prev = ranal_get_current();
    ranal_set_current(de->taskbar_ctx);

    de->taskbar_root = ranal_context_root(de->taskbar_ctx);
    ranal_set_size(de->taskbar_root, de->display_w, DE_TASKBAR_H);
    ranal_set_layout(de->taskbar_root, RANAL_LAYOUT_ABSOLUTE);
    ranal_set_background(de->taskbar_root, COL_BAR_BG);

    /* Launcher button (panel + centered label). Positions are relative to root. */
    de->launcher_panel = build_panel_button(de->taskbar_root,
        de->launcher_x, DE_TASK_PADDING,
        de->launcher_w, de->launcher_h, COL_BTN_IDLE);
    if (de->launcher_panel != NULL) {
        ranal_widget_t *lbl = ranal_label(de->launcher_panel, "MENU");
        if (lbl != NULL) {
            int32_t tw = ranal_text_width("MENU");
            ranal_set_pos(lbl, (de->launcher_w - tw) / 2,
                               (de->launcher_h - ranal_text_height()) / 2);
            ranal_set_foreground(lbl, COL_TEXT_ACCENT);
            de->launcher_label = lbl;
        }
    }

    /* Pre-allocate window list slots. They start hidden and we toggle visibility
       and content based on the current swm surface list. */
    for (int i = 0; i < DE_MAX_ITEMS; i++) {
        int32_t ix = de->items_origin_x + i * (de->item_w + DE_ITEM_GAP);
        ranal_widget_t *p = build_panel_button(de->taskbar_root,
            ix, DE_TASK_PADDING, de->item_w, de->item_h, COL_BTN_IDLE);
        if (p == NULL) continue;
        ranal_set_visible(p, 0);
        de->item_panels[i] = p;
        ranal_widget_t *lbl = ranal_label(p, "");
        if (lbl != NULL) {
            ranal_set_pos(lbl, 6, (de->item_h - ranal_text_height()) / 2);
            ranal_set_foreground(lbl, COL_TEXT_BRIGHT);
            de->item_labels[i] = lbl;
        }
    }

    ranal_widget_t *sp = build_panel_button(de->taskbar_root,
        de->stats_x, DE_TASK_PADDING,
        de->stats_w, de->stats_h, COL_BAR_BG);
    (void)sp;

    ranal_widget_t *tp = build_panel_button(de->taskbar_root,
        de->tick_x, DE_TASK_PADDING,
        de->tick_w, de->tick_h, COL_BAR_BG);
    (void)tp;

    /* Clock (panel + centered label). */
    ranal_widget_t *cp = build_panel_button(de->taskbar_root,
        de->clock_x, DE_TASK_PADDING,
        de->clock_w, de->clock_h, COL_BAR_BG);
    if (cp != NULL) {
        ranal_widget_t *lbl = ranal_label(cp, "--:--");
        if (lbl != NULL) {
            int32_t tw = ranal_text_width("00:00");
            ranal_set_pos(lbl, (de->clock_w - tw) / 2,
                               (de->clock_h - ranal_text_height()) / 2);
            ranal_set_foreground(lbl, COL_TEXT_ACCENT);
            de->clock_label = lbl;
        }
    }

    ranal_invalidate();
    ranal_set_current(prev);
}

static void build_menu_widgets(de_t *de) {
    ranal_context_t *prev = ranal_get_current();
    ranal_set_current(de->menu_ctx);

    de->menu_root = ranal_context_root(de->menu_ctx);
    ranal_set_size(de->menu_root, de->menu_w, de->menu_h);
    ranal_set_layout(de->menu_root, RANAL_LAYOUT_ABSOLUTE);
    ranal_set_background(de->menu_root, COL_MENU_BG);

    for (int i = 0; i < DE_MENU_ENTRIES; i++) {
        int32_t y = 4 + i * DE_MENU_ITEM_H;
        ranal_widget_t *p = build_panel_button(de->menu_root, 4, y,
            de->menu_w - 8, DE_MENU_ITEM_H - 2, COL_MENU_BG);
        if (p == NULL) continue;
        de->menu_entry_panels[i] = p;
        ranal_widget_t *lbl = ranal_label(p, k_menu_labels[i]);
        if (lbl != NULL) {
            ranal_set_pos(lbl, 8, (DE_MENU_ITEM_H - 2 - ranal_text_height()) / 2);
            ranal_set_foreground(lbl, COL_TEXT_BRIGHT);
            de->menu_entry_labels[i] = lbl;
        }
    }
    ranal_invalidate();
    ranal_set_current(prev);
}

de_t *de_create(swm_state_t *swm) {
    if (swm == NULL) return NULL;
    de_t *de = calloc(1, sizeof(*de));
    if (de == NULL) return NULL;
    de->swm = swm;
    de->display_w = (int32_t)swm->display_w;
    de->display_h = (int32_t)swm->display_h;
    de->hover_item = -1;
    de->pressed_item = -1;
    compute_layout(de);

    de->taskbar_ctx = ranal_context_create_offscreen(de->display_w, DE_TASKBAR_H);
    if (de->taskbar_ctx == NULL) {
        free(de);
        return NULL;
    }
    build_taskbar_widgets(de);

    de->menu_ctx = ranal_context_create_offscreen(de->menu_w, de->menu_h);
    if (de->menu_ctx == NULL) {
        ranal_context_destroy(de->taskbar_ctx);
        free(de);
        return NULL;
    }
    build_menu_widgets(de);

    de->taskbar_dirty = 1;
    de->menu_dirty = 1;
    ranal_context_t *prev = ranal_get_current();
    ranal_set_current(de->taskbar_ctx);
    format_clock(de);
    format_system_stats(de);
    ranal_set_current(prev);
    return de;
}

void de_destroy(de_t *de) {
    if (de == NULL) return;
    if (de->menu_ctx != NULL) ranal_context_destroy(de->menu_ctx);
    if (de->taskbar_ctx != NULL) ranal_context_destroy(de->taskbar_ctx);
    free(de);
}

int32_t de_taskbar_top(const de_t *de) {
    return de != NULL ? de->taskbar_top : 0;
}

int32_t de_workspace_height(const de_t *de) {
    return de != NULL ? de->taskbar_top : 0;
}

static int rect_contains(int32_t mx, int32_t my, int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
}

static int de_surface_is_window(const swm_surface_t *s) {
    return s != NULL && s->in_use && s->committed && s->role == SPROT_SURFACE_ROLE_TOPLEVEL;
}

int de_point_in_panel(const de_t *de, int32_t mx, int32_t my) {
    if (de == NULL) return 0;
    if (my >= de->taskbar_top) return 1;
    if (de->menu_open && rect_contains(mx, my, de->menu_x, de->menu_y, de->menu_w, de->menu_h)) {
        return 1;
    }
    return 0;
}

static void rebuild_items_from_swm(de_t *de) {
    swm_state_t *swm = de->swm;
    de->item_count = 0;
    /* Stable order: walk surfaces in id order so the taskbar doesn't jitter as
       z-order shuffles around. */
    uint32_t seen_max_id = 0;
    for (int slot = 0; slot < DE_MAX_ITEMS; slot++) {
        uint32_t next_id = 0;
        swm_surface_t *picked = NULL;
        for (int i = 0; i < SWM_MAX_SURFACES; i++) {
            swm_surface_t *s = &swm->surfaces[i];
            if (!de_surface_is_window(s)) continue;
            if (s->id <= seen_max_id) continue;
            if (next_id == 0 || s->id < next_id) {
                next_id = s->id;
                picked = s;
            }
        }
        if (picked == NULL) break;
        seen_max_id = next_id;

        de_item_t *it = &de->items[de->item_count];
        it->surface_id = picked->id;
        it->minimized = picked->minimized;
        it->x = de->items_origin_x + de->item_count * (de->item_w + DE_ITEM_GAP);
        it->y = de->item_y;
        it->w = de->item_w;
        it->h = de->item_h;
        it->focused = 0;
        it->hover = (de->item_count == de->hover_item) ? 1 : 0;
        de->item_count++;
    }
    /* The topmost (highest z, non-minimized) surface is the "focused" one. */
    swm_surface_t *focused = NULL;
    int best_z = -1;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &swm->surfaces[i];
        if (!de_surface_is_window(s) || s->minimized) continue;
        if (s->z > best_z) { best_z = s->z; focused = s; }
    }
    if (focused != NULL) {
        for (int i = 0; i < de->item_count; i++) {
            if (de->items[i].surface_id == focused->id) {
                de->items[i].focused = 1;
                break;
            }
        }
    }
}

static void apply_items_to_widgets(de_t *de) {
    for (int i = 0; i < DE_MAX_ITEMS; i++) {
        ranal_widget_t *p = de->item_panels[i];
        ranal_widget_t *lbl = de->item_labels[i];
        if (p == NULL) continue;
        if (i >= de->item_count) {
            ranal_set_visible(p, 0);
            continue;
        }
        ranal_set_visible(p, 1);
        de_item_t *it = &de->items[i];

        ranal_color_t bg;
        ranal_color_t fg;
        int pressed_now = (de->pressed_item == i);
        if (pressed_now)            { bg = COL_BTN_PRESSED; fg = COL_TEXT_BRIGHT; }
        else if (it->focused)       { bg = COL_BTN_ACTIVE;  fg = COL_TEXT_BRIGHT; }
        else if (it->hover)         { bg = COL_BTN_HOVER;   fg = COL_TEXT_BRIGHT; }
        else if (it->minimized)     { bg = COL_BTN_DIM;     fg = COL_TEXT_DIM;    }
        else                        { bg = COL_BTN_IDLE;    fg = COL_TEXT_BRIGHT; }
        ranal_set_background(p, bg);

        /* Build a truncated label using the swm surface's title. */
        swm_surface_t *s = NULL;
        for (int k = 0; k < SWM_MAX_SURFACES; k++) {
            if (de->swm->surfaces[k].in_use && de->swm->surfaces[k].id == it->surface_id) {
                s = &de->swm->surfaces[k];
                break;
            }
        }
        const char *title = (s != NULL && s->title[0] != '\0') ? s->title : "(window)";
        char buf[64];
        int max_chars = (it->w - 12) / 6;
        if (max_chars < 1) max_chars = 1;
        if (max_chars > (int)sizeof(buf) - 1) max_chars = (int)sizeof(buf) - 1;
        const char *prefix = it->minimized ? "* " : "";
        size_t prefix_len = strlen(prefix);
        memcpy(buf, prefix, prefix_len);
        size_t copy = strlen(title);
        if (prefix_len + copy > (size_t)max_chars) copy = (size_t)max_chars - prefix_len;
        memcpy(buf + prefix_len, title, copy);
        buf[prefix_len + copy] = '\0';

        if (lbl != NULL) {
            ranal_set_text(lbl, buf);
            ranal_set_foreground(lbl, fg);
        }
    }
}

static void apply_launcher_to_widgets(de_t *de) {
    if (de->launcher_panel == NULL) return;
    ranal_color_t bg;
    if (de->menu_open || de->launcher_pressed) bg = COL_BTN_PRESSED;
    else if (de->launcher_hover)               bg = COL_BTN_HOVER;
    else                                       bg = COL_BTN_IDLE;
    ranal_set_background(de->launcher_panel, bg);
}

static void apply_menu_to_widgets(de_t *de) {
    for (int i = 0; i < DE_MENU_ENTRIES; i++) {
        ranal_widget_t *p = de->menu_entry_panels[i];
        if (p == NULL) continue;
        ranal_color_t bg = (i == de->menu_hover) ? COL_MENU_HOVER : COL_MENU_BG;
        ranal_set_background(p, bg);
    }
}

void de_tick(de_t *de, uint64_t frame_count) {
    (void)frame_count;
    if (de == NULL) return;
    de->tick_counter++;

    /*
     * All ranal_set_* calls update g_ranal->dirty, so we must switch the
     * current context to our offscreen one before mutating widgets — otherwise
     * the dirty flag lands on the wrong context and ranal_context_frame()
     * will short-circuit out of rendering after the first two frames.
     */
    ranal_context_t *prev = ranal_get_current();
    ranal_set_current(de->taskbar_ctx);
    format_clock(de);
    format_system_stats(de);
    rebuild_items_from_swm(de);
    apply_items_to_widgets(de);
    apply_launcher_to_widgets(de);
    ranal_invalidate();
    if (de->menu_open) {
        ranal_set_current(de->menu_ctx);
        apply_menu_to_widgets(de);
        ranal_invalidate();
    }
    ranal_set_current(prev);

    /*
     * Advance the per-item tooltip fade. Hovered item targets alpha=1, all
     * others target 0. We use an inverse-time-constant lerp so it eases in
     * naturally; DE_TOOLTIP_FADE_SEC sets the full-range duration.
     */
    double dt = elapsed_seconds_since(&de->last_tick_time, &de->have_last_tick);
    float step = (DE_TOOLTIP_FADE_SEC > 0.0f) ? (float)dt / DE_TOOLTIP_FADE_SEC : 1.0f;
    for (int i = 0; i < DE_MAX_ITEMS; i++) {
        float target = (i < de->item_count && i == de->hover_item) ? 1.0f : 0.0f;
        float cur = de->item_tooltip_alpha[i];
        if (cur < target) {
            cur += step;
            if (cur > target) cur = target;
        } else if (cur > target) {
            cur -= step;
            if (cur < target) cur = target;
        }
        de->item_tooltip_alpha[i] = clampf(cur, 0.0f, 1.0f);
    }
}

static int item_hit(const de_t *de, int32_t mx, int32_t my) {
    if (my < de->item_y || my >= de->item_y + de->item_h) return -1;
    for (int i = 0; i < de->item_count; i++) {
        const de_item_t *it = &de->items[i];
        if (rect_contains(mx, my, it->x, it->y, it->w, it->h)) return i;
    }
    return -1;
}

static int menu_hit(const de_t *de, int32_t mx, int32_t my) {
    if (!de->menu_open) return -1;
    if (mx < de->menu_x + 4 || mx >= de->menu_x + de->menu_w - 4) return -1;
    int32_t rel_y = my - de->menu_y - 4;
    if (rel_y < 0) return -1;
    int idx = rel_y / DE_MENU_ITEM_H;
    if (idx < 0 || idx >= DE_MENU_ENTRIES) return -1;
    return idx;
}

void de_on_mouse_motion(de_t *de, int32_t mx, int32_t my) {
    if (de == NULL) return;
    int prev_launcher = de->launcher_hover;
    int prev_item     = de->hover_item;
    int prev_menu     = de->menu_hover;

    de->launcher_hover = rect_contains(mx, my,
        de->launcher_x, de->launcher_y, de->launcher_w, de->launcher_h);
    de->hover_item = item_hit(de, mx, my);
    de->menu_hover = menu_hit(de, mx, my);

    if (de->launcher_hover != prev_launcher
        || de->hover_item != prev_item
        || de->menu_hover != prev_menu) {
        de->taskbar_dirty = 1;
        if (de->menu_open) de->menu_dirty = 1;
    }
}

static void de_action_tile(de_t *de) {
    swm_state_t *swm = de->swm;
    /* Collect non-minimized surfaces in id order. */
    int count = 0;
    swm_surface_t *list[SWM_MAX_SURFACES];
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &swm->surfaces[i];
        if (de_surface_is_window(s) && !s->minimized) list[count++] = s;
    }
    if (count == 0) return;
    int cols = 1, rows = 1;
    while (cols * rows < count) {
        if (cols <= rows) cols++;
        else rows++;
    }
    int32_t avail_w = (int32_t)swm->display_w;
    int32_t avail_h = de->taskbar_top;
    int32_t cell_w = avail_w / cols;
    int32_t cell_h = avail_h / rows;
    int32_t titlebar_chrome = SWM_TITLEBAR_H + 2 * SWM_BORDER;
    for (int i = 0; i < count; i++) {
        int r = i / cols, c = i % cols;
        swm_surface_t *s = list[i];
        s->maximized = 0;
        s->pos_x = c * cell_w + SWM_BORDER;
        s->pos_y = r * cell_h + titlebar_chrome;
        /* Don't resize; we just place — sizes stay client-owned. */
    }
}

static void de_action_cascade(de_t *de) {
    swm_state_t *swm = de->swm;
    int idx = 0;
    int32_t step = 28;
    int32_t base_x = 48, base_y = 56;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &swm->surfaces[i];
        if (!de_surface_is_window(s)) continue;
        s->maximized = 0;
        s->minimized = 0;
        s->pos_x = base_x + idx * step;
        s->pos_y = base_y + idx * step;
        if (s->pos_x + 100 > (int32_t)swm->display_w) s->pos_x = base_x;
        if (s->pos_y + 100 > de->taskbar_top)          s->pos_y = base_y;
        idx++;
    }
}

static void de_action_minimize_all(de_t *de) {
    swm_state_t *swm = de->swm;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &swm->surfaces[i];
        if (!de_surface_is_window(s)) continue;
        s->minimized = 1;
    }
}

static void de_action_quit(de_t *de) {
    de->swm->should_quit = 1;
}

static void de_launch_tool(de_t *de, const char *tool, const char *size_arg, const char *title) {
    (void)de;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* Grandchild trick: orphan launched tools so swm doesn't have to
           reap it. The intermediate child exits immediately. */
        pid_t inner = fork();
        if (inner < 0) _exit(127);
        if (inner == 0) {
            int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
            if (devnull >= 0) {
                dup2(devnull, 0);
                dup2(devnull, 1);
                dup2(devnull, 2);
                if (devnull > 2) close(devnull);
            }
            setsid();
            signal(SIGCHLD, SIG_DFL);
            char self_path[256];
            ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
            if (n <= 0) _exit(127);
            self_path[n] = '\0';

            char *args[8];
            int argc = 0;
            args[argc++] = self_path;
            args[argc++] = (char *)tool;
            args[argc++] = (char *)"--swm";
            if (size_arg != NULL) args[argc++] = (char *)size_arg;
            args[argc++] = (char *)"--title";
            args[argc++] = (char *)title;
            args[argc] = NULL;
            execv(self_path, args);
            _exit(127);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
}

static void de_action_term(de_t *de) {
    de_launch_tool(de, "term", NULL, "term");
}

static void de_action_about(de_t *de) {
    de_launch_tool(de, "about", "760x480", "About system");
}

static void run_menu_action(de_t *de, int idx) {
    switch (idx) {
        case DE_MENU_TERM:         de_action_term(de); break;
        case DE_MENU_ABOUT:        de_action_about(de); break;
        case DE_MENU_TILE:         de_action_tile(de); break;
        case DE_MENU_CASCADE:      de_action_cascade(de); break;
        case DE_MENU_MINIMIZE_ALL: de_action_minimize_all(de); break;
        case DE_MENU_QUIT:         de_action_quit(de); break;
        default: break;
    }
}

static void item_clicked(de_t *de, int idx) {
    if (idx < 0 || idx >= de->item_count) return;
    uint32_t sid = de->items[idx].surface_id;
    swm_state_t *swm = de->swm;
    swm_surface_t *s = NULL;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (swm->surfaces[i].in_use && swm->surfaces[i].id == sid) {
            s = &swm->surfaces[i];
            break;
        }
    }
    if (!de_surface_is_window(s)) return;
    /* Find topmost (focused) non-minimized surface to decide between
       raise-and-focus vs minimize. */
    swm_surface_t *focused = NULL;
    int best_z = -1;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *x = &swm->surfaces[i];
        if (!de_surface_is_window(x) || x->minimized) continue;
        if (x->z > best_z) { best_z = x->z; focused = x; }
    }
    if (s->minimized) {
        s->minimized = 0;
        s->z = ++swm->next_z;
    } else if (focused == s) {
        s->minimized = 1;
    } else {
        s->z = ++swm->next_z;
    }
}

int de_on_mouse_button(de_t *de, int32_t mx, int32_t my, uint32_t button, uint32_t state) {
    if (de == NULL) return 0;
    if (button != 1 /* left */) {
        /* Right/middle clicks inside the taskbar are consumed but ignored. */
        return de_point_in_panel(de, mx, my);
    }
    if (state == 1 /* pressed */) {
        if (de->menu_open) {
            int idx = menu_hit(de, mx, my);
            if (idx >= 0) {
                run_menu_action(de, idx);
                de->menu_open = 0;
                de->taskbar_dirty = 1;
                return 1;
            }
            /* Press outside the menu closes it (and may fall through to taskbar). */
            de->menu_open = 0;
            de->taskbar_dirty = 1;
        }
        if (rect_contains(mx, my,
                          de->launcher_x, de->launcher_y, de->launcher_w, de->launcher_h)) {
            de->launcher_pressed = 1;
            de->taskbar_dirty = 1;
            return 1;
        }
        int idx = item_hit(de, mx, my);
        if (idx >= 0) {
            de->pressed_item = idx;
            de->taskbar_dirty = 1;
            return 1;
        }
        if (my >= de->taskbar_top) return 1;
        return 0;
    } else {
        /* Released. */
        if (de->launcher_pressed) {
            de->launcher_pressed = 0;
            de->taskbar_dirty = 1;
            if (rect_contains(mx, my,
                              de->launcher_x, de->launcher_y, de->launcher_w, de->launcher_h)) {
                de->menu_open = !de->menu_open;
                de->menu_dirty = 1;
            }
            return 1;
        }
        if (de->pressed_item >= 0) {
            int idx = de->pressed_item;
            de->pressed_item = -1;
            de->taskbar_dirty = 1;
            int now_idx = item_hit(de, mx, my);
            if (now_idx == idx) item_clicked(de, idx);
            return 1;
        }
        if (my >= de->taskbar_top) return 1;
    }
    return 0;
}

void de_toggle_menu(de_t *de) {
    if (de == NULL) return;
    de->menu_open = !de->menu_open;
    de->menu_dirty = 1;
    de->taskbar_dirty = 1;
}

static void blit_surface_into_fb(ranal_surface_t *src,
                                 struct srapi_framebuffer *fb,
                                 int32_t dst_x, int32_t dst_y) {
    if (src == NULL || fb == NULL) return;
    uint32_t *src_px = ranal_surface_pixels(src);
    int32_t src_w = ranal_surface_width(src);
    int32_t src_h = ranal_surface_height(src);
    int32_t src_pitch_px = ranal_surface_pitch(src) / 4;
    if (src_px == NULL) return;

    uint32_t *dst_px = srapi_framebuffer_pixels(fb);
    if (dst_px == NULL) return;
    int32_t dst_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t dst_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t dst_pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);

    int32_t sx0 = 0, sy0 = 0;
    int32_t w = src_w, h = src_h;
    if (dst_x < 0) { sx0 = -dst_x; w -= sx0; dst_x = 0; }
    if (dst_y < 0) { sy0 = -dst_y; h -= sy0; dst_y = 0; }
    if (dst_x + w > dst_w) w = dst_w - dst_x;
    if (dst_y + h > dst_h) h = dst_h - dst_y;
    if (w <= 0 || h <= 0) return;
    for (int32_t row = 0; row < h; row++) {
        const uint32_t *sr = src_px + (sy0 + row) * src_pitch_px + sx0;
        uint32_t *dr = dst_px + (dst_y + row) * dst_pitch_px + dst_x;
        memcpy(dr, sr, (size_t)w * 4u);
    }
}

static void render_system_stats(de_t *de, struct srapi_framebuffer *fb) {
    format_system_stats(de);
    const char *text = de->stats_text[0] != '\0' ? de->stats_text : "CPU --.-% RAM --%";
    int32_t text_w = (int32_t)strlen(text) * RANAL_FONT_ADVANCE_X;
    int32_t tx = de->stats_x + (de->stats_w - text_w) / 2;
    if (tx < de->stats_x + 4) tx = de->stats_x + 4;
    int32_t ty = de->stats_y + (de->stats_h - RANAL_GLYPH_HEIGHT) / 2;

    fb_blend_rect(fb, de->stats_x, de->stats_y, de->stats_w, de->stats_h,
                  COL_BAR_BG, 255u);
    fb_blend_text(fb, tx, ty, text, COL_TEXT_BRIGHT, 255u);
}

static void render_tick_counter(de_t *de, struct srapi_framebuffer *fb) {
    char text[16];
    snprintf(text, sizeof(text), "T %06llu",
             (unsigned long long)(de->tick_counter % 1000000ull));
    int32_t text_w = (int32_t)strlen(text) * RANAL_FONT_ADVANCE_X;
    int32_t tx = de->tick_x + (de->tick_w - text_w) / 2;
    if (tx < de->tick_x + 4) tx = de->tick_x + 4;
    int32_t ty = de->tick_y + (de->tick_h - RANAL_GLYPH_HEIGHT) / 2;

    fb_blend_rect(fb, de->tick_x, de->tick_y, de->tick_w, de->tick_h,
                  COL_BAR_BG, 255u);
    fb_blend_text(fb, tx, ty, text, COL_TEXT_ACCENT, 255u);
}

static void render_item_tooltips(de_t *de, struct srapi_framebuffer *fb) {
    /*
     * For each task button that has a non-zero fade alpha, draw a small
     * floating bubble above the taskbar with the full (unclipped) window
     * title. Fade is per-item so a quick mouse pass leaves a brief trail.
     */
    for (int i = 0; i < de->item_count; i++) {
        float a = de->item_tooltip_alpha[i];
        if (a <= 0.01f) continue;
        const de_item_t *it = &de->items[i];
        swm_surface_t *s = NULL;
        for (int k = 0; k < SWM_MAX_SURFACES; k++) {
            if (de->swm->surfaces[k].in_use && de->swm->surfaces[k].id == it->surface_id) {
                s = &de->swm->surfaces[k];
                break;
            }
        }
        const char *title = (s != NULL && s->title[0] != '\0') ? s->title : "(window)";

        int32_t text_w = (int32_t)strlen(title) * RANAL_FONT_ADVANCE_X;
        int32_t tip_w = text_w + 2 * DE_TOOLTIP_PAD_X;
        int32_t tip_h = RANAL_GLYPH_HEIGHT + 2 * DE_TOOLTIP_PAD_Y;
        int32_t tip_x = it->x + (it->w - tip_w) / 2;
        if (tip_x < 4) tip_x = 4;
        if (tip_x + tip_w > de->display_w - 4) tip_x = de->display_w - 4 - tip_w;

        /* Animate a small upward rise as it fades in (4px → 0). */
        int32_t rise = (int32_t)((1.0f - a) * 4.0f);
        int32_t tip_y = it->y - tip_h - DE_TOOLTIP_GAP + rise;
        if (tip_y < 0) tip_y = 0;

        uint32_t bg_alpha     = (uint32_t)(a * 220.0f);
        uint32_t border_alpha = (uint32_t)(a * 200.0f);
        uint32_t arrow_alpha  = (uint32_t)(a * 220.0f);
        uint32_t text_alpha   = (uint32_t)(a * 255.0f);

        ranal_color_t bg     = RANAL_COLOR(24, 28, 38);
        ranal_color_t border = RANAL_COLOR(90, 130, 200);
        ranal_color_t text   = RANAL_COLOR(235, 238, 248);

        fb_blend_rect(fb, tip_x, tip_y, tip_w, tip_h, bg, bg_alpha);
        fb_blend_rect(fb, tip_x, tip_y, tip_w, 1, border, border_alpha);
        fb_blend_rect(fb, tip_x, tip_y + tip_h - 1, tip_w, 1, border, border_alpha);
        fb_blend_rect(fb, tip_x, tip_y, 1, tip_h, border, border_alpha);
        fb_blend_rect(fb, tip_x + tip_w - 1, tip_y, 1, tip_h, border, border_alpha);

        /* Tiny pointer arrow under the bubble centered over the button. */
        int32_t arrow_cx = it->x + it->w / 2;
        if (arrow_cx < tip_x + 4) arrow_cx = tip_x + 4;
        if (arrow_cx > tip_x + tip_w - 5) arrow_cx = tip_x + tip_w - 5;
        for (int row = 0; row < 4; row++) {
            int32_t aw = 7 - row * 2;
            fb_blend_rect(fb, arrow_cx - aw / 2, tip_y + tip_h + row, aw, 1, bg, arrow_alpha);
        }

        fb_blend_text(fb, tip_x + DE_TOOLTIP_PAD_X, tip_y + DE_TOOLTIP_PAD_Y,
                      title, text, text_alpha);
    }
}

void de_render(de_t *de, struct srapi_framebuffer *fb) {
    if (de == NULL || fb == NULL) return;

    /* Subtle 1-pixel top border separating the taskbar from the workspace. */
    uint32_t *dst_px = srapi_framebuffer_pixels(fb);
    if (dst_px != NULL) {
        int32_t dst_w = (int32_t)srapi_framebuffer_width(fb);
        int32_t dst_pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
        int32_t border_y = de->taskbar_top - 1;
        if (border_y >= 0 && border_y < (int32_t)srapi_framebuffer_height(fb)) {
            uint32_t *row = dst_px + (size_t)border_y * dst_pitch_px;
            int32_t lim = dst_w < de->display_w ? dst_w : de->display_w;
            for (int32_t x = 0; x < lim; x++) row[x] = COL_BAR_BORDER;
        }
    }

    /* Render the taskbar into its offscreen surface, then blit. de_tick()
       has already invalidated the offscreen contexts that need redrawing. */
    if (de->taskbar_ctx != NULL) {
        ranal_context_frame(de->taskbar_ctx);
        de->taskbar_dirty = 0;
        ranal_surface_t *src = ranal_context_surface(de->taskbar_ctx);
        blit_surface_into_fb(src, fb, 0, de->taskbar_top);
    }

    render_system_stats(de, fb);
    render_tick_counter(de, fb);

    /* Hovered task tooltips. Drawn on top of the bar, below the menu. */
    render_item_tooltips(de, fb);

    if (de->menu_open && de->menu_ctx != NULL) {
        ranal_context_frame(de->menu_ctx);
        de->menu_dirty = 0;
        ranal_surface_t *src = ranal_context_surface(de->menu_ctx);
        blit_surface_into_fb(src, fb, de->menu_x, de->menu_y);
    }

    (void)clampi;
}
