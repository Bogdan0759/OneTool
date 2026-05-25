/*
 * App glue: navigation, fetching, parsing, layout, paint dispatch.
 *
 * Fetching is asynchronous (pthread worker; see net/fetch_async.c) so
 * the UI stays responsive while a page is loading. The frame loop polls
 * the fetcher each iteration; when a fetch completes we re-parse, re-lay
 * out, and mark the page dirty.
 *
 * Dirty tracking is split between page_dirty (heavy: re-paints the
 * content surface) and chrome_dirty (light: re-paints just the URL bar
 * / status strip). URL-bar typing only sets chrome_dirty, so editing
 * the address doesn't trigger a full page repaint.
 */
#include "app.h"
#include "net/fetch.h"
#include "net/fetch_async.h"
#include "parse/html.h"
#include "render/layout.h"
#include "render/paint.h"
#include "ui/chrome.h"
#include "ui/input.h"

#include <ranal/ranal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module-private state — pinned by the input/key hook closure. */
static br_layout_t g_layout;
static int         g_layout_width = -1;
static void       *g_chrome_surface = NULL;
static void       *g_status_surface = NULL;

/* Built-in welcome doc, shown when no URL is given. */
static const char *kHomeHtml =
    "<html><head><title>OneTool Browser</title></head><body>"
    "<h1>OneTool Browser</h1>"
    "<p>A tiny text-mode browser built on <b>ranal</b> + <b>srapi</b>.</p>"
    "<h2>Controls</h2>"
    "<ul>"
    "<li><b>Ctrl+L</b> or <b>/</b> — focus the URL bar</li>"
    "<li><b>Enter</b> — submit URL / follow focused link</li>"
    "<li><b>Tab</b> — move link focus</li>"
    "<li><b>↑/↓ PgUp/PgDn Home/End Space</b> — scroll</li>"
    "<li><b>Alt+←</b> — back</li>"
    "<li><b>F5 / Ctrl+R</b> — reload</li>"
    "<li><b>Esc</b> — quit (or unfocus URL bar)</li>"
    "<li><b>Click</b> on a blue link to follow it; mouse wheel scrolls</li>"
    "</ul>"
    "<h2>Try</h2>"
    "<p>Type a URL like <code>example.com</code> and press Enter.</p>"
    "<hr>"
    "<p><i>Pass <code>--swm</code> to run this browser as a window inside swm "
    "instead of taking over DRM.</i></p>"
    "</body></html>";

static void load_home(browser_app_t *app) {
    br_doc_clear(app->doc);
    if (br_doc_parse_html(app->doc, kHomeHtml, strlen(kHomeHtml)) != 0) {
        snprintf(app->status, sizeof(app->status), "parse failed");
    } else {
        snprintf(app->status, sizeof(app->status),
                 "welcome — press Ctrl+L to enter a URL");
    }
    snprintf(app->url, sizeof(app->url), "about:home");
    app->scroll_y = 0;
    app->focused_link = -1;
    app->hover_link = -1;
    g_layout_width = -1;
    app->page_dirty = 1;
    app->chrome_dirty = 1;
}

int br_app_init(browser_app_t *app, const char *initial_url) {
    memset(app, 0, sizeof(*app));
    app->doc = br_doc_create();
    if (app->doc == NULL) return -1;
    app->focused_link = -1;
    app->hover_link = -1;
    app->page_dirty = 1;
    app->chrome_dirty = 1;
    snprintf(app->status, sizeof(app->status), "ready");
    br_layout_init(&g_layout);

    br_net_t *net = (br_net_t *)calloc(1, sizeof(*net));
    if (net == NULL) return -1;
    br_net_init(net);
    app->net = net;

    if (initial_url != NULL && initial_url[0] != '\0') {
        strncpy(app->url, initial_url, sizeof(app->url) - 1);
        app->pending_navigate = 1;
    } else {
        load_home(app);
    }
    return 0;
}

void br_app_shutdown(browser_app_t *app) {
    if (app->net != NULL) {
        br_net_destroy((br_net_t *)app->net);
        free(app->net);
        app->net = NULL;
    }
    br_doc_destroy(app->doc);
    app->doc = NULL;
    br_layout_clear(&g_layout);
    free(app->hits.rects);
    app->hits.rects = NULL;
    app->hits.count = app->hits.cap = 0;
    for (int i = 0; i < app->history_count; i++) free(app->history[i]);
    app->history_count = 0;
    if (app->page_surface != NULL) {
        ranal_surface_destroy((ranal_surface_t *)app->page_surface);
        app->page_surface = NULL;
    }
    if (g_chrome_surface != NULL) {
        ranal_surface_destroy((ranal_surface_t *)g_chrome_surface);
        g_chrome_surface = NULL;
    }
    if (g_status_surface != NULL) {
        ranal_surface_destroy((ranal_surface_t *)g_status_surface);
        g_status_surface = NULL;
    }
}

static void normalize_url(char *url, size_t cap) {
    if (url[0] == '\0') return;
    /* If no scheme and not a file path, prepend http://. */
    if (strstr(url, "://") != NULL) return;
    if (url[0] == '/') return; /* leave file paths to the user */
    char tmp[BROWSER_URL_MAX];
    snprintf(tmp, sizeof(tmp), "http://%s", url);
    strncpy(url, tmp, cap - 1);
    url[cap - 1] = '\0';
}

static void push_history(browser_app_t *app, const char *url) {
    if (app->history_count > 0 &&
        strcmp(app->history[app->history_count - 1], url) == 0) {
        return;
    }
    int max = (int)(sizeof(app->history) / sizeof(app->history[0]));
    if (app->history_count == max) {
        free(app->history[0]);
        memmove(app->history, app->history + 1,
                (size_t)(max - 1) * sizeof(app->history[0]));
        app->history_count = max - 1;
    }
    app->history[app->history_count++] = strdup(url);
}

/* Kick off a navigation. For about:home this is synchronous (cheap),
 * for HTTP(S) it spawns the async fetcher; the frame loop picks up the
 * result later. Returns 0 if accepted, -1 if rejected. */
int br_app_navigate(browser_app_t *app, const char *url) {
    if (url == NULL || url[0] == '\0') return -1;
    char target[BROWSER_URL_MAX];
    strncpy(target, url, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';

    if (strcmp(target, "about:home") == 0) {
        load_home(app);
        push_history(app, app->url);
        return 0;
    }

    normalize_url(target, sizeof(target));

    snprintf(app->status, sizeof(app->status), "loading %s…", target);
    app->chrome_dirty = 1;

    if (br_net_start((br_net_t *)app->net, target) != 0) {
        snprintf(app->status, sizeof(app->status), "error: fetch start failed");
        return -1;
    }
    /* Optimistically show the URL the user requested while loading. */
    strncpy(app->url, target, sizeof(app->url) - 1);
    app->url[sizeof(app->url) - 1] = '\0';
    return 0;
}

static void apply_fetch_result(browser_app_t *app,
                               char *body, size_t len, char *final_url,
                               const char *err, int ok) {
    if (!ok) {
        snprintf(app->status, sizeof(app->status), "error: %s",
                 err != NULL && err[0] != '\0' ? err : "unknown");
        app->chrome_dirty = 1;
        return;
    }
    if (br_doc_parse_html(app->doc, body, len) != 0) {
        snprintf(app->status, sizeof(app->status), "parse failed");
        app->chrome_dirty = 1;
        return;
    }

    const char *show_url = (final_url != NULL && final_url[0] != '\0')
                           ? final_url : app->url;
    strncpy(app->url, show_url, sizeof(app->url) - 1);
    app->url[sizeof(app->url) - 1] = '\0';
    strncpy(app->doc->base_url, app->url, sizeof(app->doc->base_url) - 1);
    app->doc->base_url[sizeof(app->doc->base_url) - 1] = '\0';

    push_history(app, app->url);

    snprintf(app->status, sizeof(app->status), "%zu B • %s",
             len, app->doc->title[0] != '\0' ? app->doc->title : "(no title)");
    app->scroll_y = 0;
    app->focused_link = -1;
    app->hover_link = -1;
    g_layout_width = -1;       /* force re-layout next frame */
    app->page_dirty = 1;
    app->chrome_dirty = 1;
}

static void resolve_link_target(browser_app_t *app, int link_index,
                                char *out, size_t cap) {
    out[0] = '\0';
    if (link_index < 0 || (size_t)link_index >= app->doc->link_count) return;
    const char *href = app->doc->links[link_index].href;
    if (href == NULL || href[0] == '\0') return;
    if (strstr(href, "://") != NULL) {
        strncpy(out, href, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    if (strncmp(href, "//", 2) == 0) {
        const char *base = app->doc->base_url[0] != '\0' ? app->doc->base_url : app->url;
        const char *scheme_end = strstr(base, "://");
        size_t scheme_len = scheme_end != NULL ? (size_t)(scheme_end - base) : 4;
        snprintf(out, cap, "%.*s:%s", (int)scheme_len, base, href);
        return;
    }
    const char *base = app->doc->base_url[0] != '\0' ? app->doc->base_url : app->url;
    if (href[0] == '/') {
        const char *scheme_end = strstr(base, "://");
        if (scheme_end != NULL) {
            const char *host_end = scheme_end + 3;
            while (*host_end != '\0' && *host_end != '/') host_end++;
            size_t prefix = (size_t)(host_end - base);
            snprintf(out, cap, "%.*s%s", (int)prefix, base, href);
            return;
        }
    }
    const char *last_slash = base;
    for (const char *q = base; *q != '\0'; q++) {
        if (*q == '/') last_slash = q;
    }
    size_t prefix = (size_t)(last_slash - base) + 1;
    snprintf(out, cap, "%.*s%s", (int)prefix, base, href);
}

void br_app_follow_link(browser_app_t *app, int link_index) {
    char target[BROWSER_URL_MAX];
    resolve_link_target(app, link_index, target, sizeof(target));
    if (target[0] == '\0') return;
    strncpy(app->url, target, sizeof(app->url) - 1);
    app->url[sizeof(app->url) - 1] = '\0';
    app->pending_navigate = 1;
    app->chrome_dirty = 1;
}

static int ensure_surfaces(browser_app_t *app) {
    int win_w = ranal_window_width();
    int win_h = ranal_window_height();
    if (win_w <= 0 || win_h <= 0) return -1;

    if (win_w != app->win_w || win_h != app->win_h) {
        app->win_w = win_w;
        app->win_h = win_h;

        int page_w = win_w;
        int page_h = win_h - BROWSER_CHROME_HEIGHT - BROWSER_STATUS_HEIGHT;
        if (page_h < 32) page_h = 32;

        if (app->page_surface != NULL) {
            ranal_surface_destroy((ranal_surface_t *)app->page_surface);
        }
        app->page_surface = ranal_surface_create(page_w, page_h);
        app->page_w = page_w;
        app->page_h = page_h;

        if (g_chrome_surface != NULL) {
            ranal_surface_destroy((ranal_surface_t *)g_chrome_surface);
        }
        g_chrome_surface = ranal_surface_create(win_w, BROWSER_CHROME_HEIGHT);

        if (g_status_surface != NULL) {
            ranal_surface_destroy((ranal_surface_t *)g_status_surface);
        }
        g_status_surface = ranal_surface_create(win_w, BROWSER_STATUS_HEIGHT);

        g_layout_width = -1;
        app->page_dirty = 1;
        app->chrome_dirty = 1;
    }
    return 0;
}

static void paint_status(browser_app_t *app) {
    if (g_status_surface == NULL) return;
    ranal_surface_t *s = (ranal_surface_t *)g_status_surface;
    uint32_t *px = ranal_surface_pixels(s);
    int sw = ranal_surface_width(s);
    int sh = ranal_surface_height(s);
    int pitch = ranal_surface_pitch(s) / 4;
    /* Background. */
    for (int r = 0; r < sh; r++) {
        uint32_t *row = px + (size_t)r * pitch;
        for (int c = 0; c < sw; c++) row[c] = RANAL_COLOR(22, 24, 30);
    }
    /* Text — first the status, then optionally the hovered link href. */
    int x = 8;
    int y = (sh - RANAL_GLYPH_HEIGHT) / 2;
    const char *t = app->status;
    /* If we're hovering a link, prefer showing its href in the status bar. */
    if (app->hover_link >= 0 && app->doc != NULL &&
        (size_t)app->hover_link < app->doc->link_count) {
        const char *href = app->doc->links[app->hover_link].href;
        if (href != NULL && href[0] != '\0') t = href;
    }
    int len = (int)strlen(t);
    int i = 0;
    while (i < len) {
        uint32_t cp;
        unsigned char c = (unsigned char)t[i];
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)t[i + 1] & 0x3F);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            cp = ((uint32_t)(c & 0x0F) << 12) |
                 (((unsigned char)t[i + 1] & 0x3F) << 6) |
                 ((unsigned char)t[i + 2] & 0x3F);
            n = 3;
        } else { cp = '?'; n = 1; }
        if (x + RANAL_FONT_ADVANCE_X > sw - 8) break;
        if (cp != ' ') {
            const uint8_t *g = ranal_font_glyph_u(cp);
            if (g != NULL) {
                for (int gy = 0; gy < RANAL_GLYPH_HEIGHT; gy++) {
                    uint8_t bits = g[gy];
                    for (int gx = 0; gx < RANAL_GLYPH_WIDTH; gx++) {
                        if ((bits & (1u << (RANAL_GLYPH_WIDTH - 1 - gx))) == 0) continue;
                        int yy = y + gy, xx = x + gx;
                        if (yy < 0 || yy >= sh || xx < 0 || xx >= sw) continue;
                        px[(size_t)yy * pitch + xx] = RANAL_COLOR(190, 195, 205);
                    }
                }
            }
        }
        x += RANAL_FONT_ADVANCE_X;
        i += n;
    }
}

int br_app_frame(browser_app_t *app) {
    /* Drain a finished fetch (if any) before processing the next request,
     * so back-to-back navigations don't lose the current result. */
    int net_state = br_net_poll((br_net_t *)app->net, 0, NULL, NULL, NULL,
                                NULL, 0);
    if (net_state == BR_NET_DONE_OK || net_state == BR_NET_DONE_ERR) {
        char *body = NULL;
        size_t len = 0;
        char *final_url = NULL;
        char err[128] = {0};
        int s = br_net_poll((br_net_t *)app->net, 1,
                            &body, &len, &final_url, err, sizeof(err));
        apply_fetch_result(app, body, len, final_url, err,
                           s == BR_NET_DONE_OK);
        free(body);
        free(final_url);
    }

    if (app->pending_navigate) {
        app->pending_navigate = 0;
        char target[BROWSER_URL_MAX];
        strncpy(target, app->url, sizeof(target) - 1);
        target[sizeof(target) - 1] = '\0';
        br_app_navigate(app, target);
    }

    if (ensure_surfaces(app) != 0) return 0;

    /* Re-layout if width changed. */
    if (g_layout_width != app->page_w) {
        br_layout_build(&g_layout, app->doc, app->page_w - 16);
        g_layout_width = app->page_w;
        app->content_h = g_layout.content_h;
        app->page_dirty = 1;
    }

    /* Clamp scroll. */
    int max_scroll = app->content_h - app->page_h;
    if (max_scroll < 0) max_scroll = 0;
    if (app->scroll_y > max_scroll) app->scroll_y = max_scroll;
    if (app->scroll_y < 0) app->scroll_y = 0;

    if (app->page_dirty) {
        br_paint_page(app->page_surface, &g_layout, app->scroll_y,
                      app->page_h,
                      app->hover_link >= 0 ? app->hover_link : app->focused_link,
                      &app->hits);
        app->page_dirty = 0;
    }
    if (app->chrome_dirty) {
        int loading = (net_state == BR_NET_LOADING);
        br_chrome_paint(g_chrome_surface, app, app->win_w, loading);
        paint_status(app);
        app->chrome_dirty = 0;
    }

    /* While loading, keep the chrome refreshed so the spinner ticks. */
    if (net_state == BR_NET_LOADING) {
        br_chrome_paint(g_chrome_surface, app, app->win_w, 1);
    }

    /* Hand frame to ranal: blit chrome + page onto its backbuffer. */
    ranal_widget_t *root = ranal_root();
    ranal_set_background(root, RANAL_COLOR(36, 40, 48));
    ranal_invalidate();
    if (ranal_render() != 0) return -1;
    ranal_blit_surface((ranal_surface_t *)g_chrome_surface, 0, 0);
    ranal_blit_surface((ranal_surface_t *)app->page_surface,
                       0, BROWSER_CHROME_HEIGHT);
    ranal_blit_surface((ranal_surface_t *)g_status_surface,
                       0, BROWSER_CHROME_HEIGHT + app->page_h);
    if (ranal_present() != 0) return -1;
    return 0;
}
