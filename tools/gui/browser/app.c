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
#include "parse/css/css.h"
#include "parse/css/internal.h"
#include "render/layout.h"
#include "render/paint.h"
#include "render/image.h"
#include "ui/chrome.h"
#include "ui/input.h"
#include "jsengine/include/jsengine.h"

#include <ranal/ranal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module-private state — pinned by the input/key hook closure. */
static br_layout_t g_layout;
static int         g_layout_width = -1;
static void       *g_chrome_surface = NULL;
static void       *g_status_surface = NULL;

/* Built-in welcome doc, shown when no URL is given. Includes an
 * intentional CSS demo block so you can see the cascade working without
 * having to load a remote page. */
static const char *kHomeHtml =
    "<html><head><title>OneTool Browser</title>"
    "<style>"
    "  body { color: #1a1a2e; background-color: #fbf8f1; }"
    "  h1 { color: #c81e5a; background-color: #ffe6f0; }"
    "  h2 { color: #2255aa; }"
    "  .green { color: #2a7a2a; font-weight: bold; }"
    "  .strike { text-decoration: line-through; color: #888; }"
    "  #boxed { background-color: #d8efff; color: #003366; }"
    "  .hidden-thing { display: none; }"
    "  code { background-color: #fff3b0; color: #5a2a00; }"
    "  a { color: #007a3d; }"
    "  blockquote { color: #553388; background-color: #f0e8ff; }"
    "</style>"
    "</head><body>"
    "<h1>OneTool Browser</h1>"
    "<p>A tiny text-mode browser built on <b>ranal</b> + <b>srapi</b>.</p>"

    "<h2>CSS demo</h2>"
    "<p>The colours and backgrounds you see here come from the "
    "<code>&lt;style&gt;</code> block in this page — proof that the CSS "
    "engine is alive.</p>"
    "<p>This text contains a <span class=\"green\">green bold span</span>, "
    "a <span class=\"strike\">struck-out fragment</span>, and an "
    "<span style=\"color:#cc4400;background-color:#ffe9d6\">inline-styled "
    "highlight</span>.</p>"
    "<p id=\"boxed\">This whole paragraph has an id-based background "
    "and text colour — selector <code>#boxed</code>.</p>"
    "<p class=\"hidden-thing\">If you can read this, display:none isn't "
    "working.</p>"
    "<blockquote>Block-quoted text inherits its purple colour from "
    "<code>blockquote { color: ... }</code>.</blockquote>"

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

static void build_and_apply_css(browser_app_t *app);
static void load_page_images(browser_app_t *app);
static void run_page_scripts(browser_app_t *app, size_t body_len);
static void copy_cstr(char *dst, size_t cap, const char *src);
static void resolve_url_against_base(const char *base_url, const char *url_str,
                                     const char *src, char *out, size_t cap);
static int doc_looks_js_heavy(const br_doc_t *doc);
static int doc_set_text_by_id(br_doc_t *doc, const char *id, const char *text);

static js_eval_result_t host_console_log(void *user_data, int argc,
                                         const js_eval_result_t *argv,
                                         char *err_buf, size_t err_cap);
static js_eval_result_t host_document_set_title(void *user_data, int argc,
                                                const js_eval_result_t *argv,
                                                char *err_buf, size_t err_cap);
static js_eval_result_t host_document_get_title(void *user_data, int argc,
                                                const js_eval_result_t *argv,
                                                char *err_buf, size_t err_cap);
static js_eval_result_t host_document_set_text(void *user_data, int argc,
                                               const js_eval_result_t *argv,
                                               char *err_buf, size_t err_cap);
static js_eval_result_t host_location_href(void *user_data, int argc,
                                           const js_eval_result_t *argv,
                                           char *err_buf, size_t err_cap);
static js_eval_result_t host_navigator_user_agent(void *user_data, int argc,
                                                  const js_eval_result_t *argv,
                                                  char *err_buf, size_t err_cap);

static const js_host_api_t kBrowserHostApi[] = {
    { "console_log", -1, host_console_log },
    { "console.log", -1, host_console_log },
    { "window.console.log", -1, host_console_log },
    { "document_set_title", 1, host_document_set_title },
    { "document.setTitle", 1, host_document_set_title },
    { "window.document.setTitle", 1, host_document_set_title },
    { "document_get_title", 0, host_document_get_title },
    { "document.getTitle", 0, host_document_get_title },
    { "window.document.getTitle", 0, host_document_get_title },
    { "document_set_text", 2, host_document_set_text },
    { "document.setText", 2, host_document_set_text },
    { "window.document.setText", 2, host_document_set_text },
    { "location_href", 0, host_location_href },
    { "location.href", 0, host_location_href },
    { "window.location.href", 0, host_location_href },
    { "navigator.userAgent", 0, host_navigator_user_agent },
    { "window.navigator.userAgent", 0, host_navigator_user_agent },
    { NULL, 0, NULL }
};

static int element_is_descendant(const br_doc_t *doc, int child, int ancestor) {
    while (child >= 0 && (size_t)child < doc->element_count) {
        if (child == ancestor) return 1;
        child = doc->elements[child].parent;
    }
    return 0;
}

static void host_set_error(char *err_buf, size_t err_cap, const char *msg) {
    if (err_buf == NULL || err_cap == 0) return;
    snprintf(err_buf, err_cap, "%s", msg != NULL ? msg : "host error");
}

static js_eval_result_t js_out_undefined(void) {
    js_eval_result_t out;
    memset(&out, 0, sizeof(out));
    out.kind = JS_VALUE_UNDEFINED;
    return out;
}

static js_eval_result_t js_out_bool(int value) {
    js_eval_result_t out = js_out_undefined();
    out.kind = JS_VALUE_BOOL;
    out.boolean = value ? 1 : 0;
    return out;
}

static js_eval_result_t js_out_string(const char *value) {
    js_eval_result_t out = js_out_undefined();
    out.kind = JS_VALUE_STRING;
    out.string = value != NULL ? value : "";
    return out;
}

static js_eval_result_t host_console_log(void *user_data, int argc,
                                         const js_eval_result_t *argv,
                                         char *err_buf, size_t err_cap) {
    (void)user_data;
    (void)err_buf;
    (void)err_cap;
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        switch (argv[i].kind) {
            case JS_VALUE_BOOL: printf("%s", argv[i].boolean ? "true" : "false"); break;
            case JS_VALUE_NUMBER: printf("%g", argv[i].number); break;
            case JS_VALUE_STRING: printf("%s", argv[i].string != NULL ? argv[i].string : ""); break;
            case JS_VALUE_NULL: printf("null"); break;
            default: printf("undefined"); break;
        }
    }
    printf("\n");
    return js_out_undefined();
}

static js_eval_result_t host_document_set_title(void *user_data, int argc,
                                                const js_eval_result_t *argv,
                                                char *err_buf, size_t err_cap) {
    browser_app_t *app = (browser_app_t *)user_data;
    if (app == NULL || app->doc == NULL) return js_out_undefined();
    if (argc != 1 || argv[0].kind != JS_VALUE_STRING) {
        host_set_error(err_buf, err_cap, "document_set_title expects string");
        return js_out_undefined();
    }
    copy_cstr(app->doc->title, sizeof(app->doc->title), argv[0].string);
    app->chrome_dirty = 1;
    return js_out_undefined();
}

static js_eval_result_t host_document_get_title(void *user_data, int argc,
                                                const js_eval_result_t *argv,
                                                char *err_buf, size_t err_cap) {
    browser_app_t *app = (browser_app_t *)user_data;
    (void)argv;
    if (argc != 0) {
        host_set_error(err_buf, err_cap, "document_get_title expects no args");
        return js_out_undefined();
    }
    if (app == NULL || app->doc == NULL) return js_out_string("");
    return js_out_string(app->doc->title);
}

static js_eval_result_t host_document_set_text(void *user_data, int argc,
                                               const js_eval_result_t *argv,
                                               char *err_buf, size_t err_cap) {
    browser_app_t *app = (browser_app_t *)user_data;
    if (app == NULL || app->doc == NULL) return js_out_bool(0);
    if (argc != 2 || argv[0].kind != JS_VALUE_STRING || argv[1].kind != JS_VALUE_STRING) {
        host_set_error(err_buf, err_cap, "document_set_text expects (id, text)");
        return js_out_bool(0);
    }
    int changed = doc_set_text_by_id(app->doc, argv[0].string, argv[1].string);
    if (changed > 0) {
        g_layout_width = -1;
        app->page_dirty = 1;
        app->chrome_dirty = 1;
    }
    return js_out_bool(changed > 0);
}

static js_eval_result_t host_location_href(void *user_data, int argc,
                                           const js_eval_result_t *argv,
                                           char *err_buf, size_t err_cap) {
    browser_app_t *app = (browser_app_t *)user_data;
    (void)argv;
    if (argc != 0) {
        host_set_error(err_buf, err_cap, "location_href expects no args");
        return js_out_undefined();
    }
    if (app == NULL) return js_out_string("");
    return js_out_string(app->url);
}

static js_eval_result_t host_navigator_user_agent(void *user_data, int argc,
                                                  const js_eval_result_t *argv,
                                                  char *err_buf, size_t err_cap) {
    (void)user_data;
    (void)argv;
    if (argc != 0) {
        host_set_error(err_buf, err_cap, "navigator.userAgent expects no args");
        return js_out_undefined();
    }
    return js_out_string("OneToolBrowser/0.1");
}

static int scroll_to_fragment(browser_app_t *app, const char *fragment) {
    if (app == NULL || app->doc == NULL || fragment == NULL || fragment[0] == '\0') {
        return -1;
    }
    int target = -1;
    for (size_t i = 0; i < app->doc->element_count; i++) {
        if (strcmp(app->doc->elements[i].id, fragment) == 0) {
            target = (int)i;
            break;
        }
    }
    if (target < 0) return -1;
    for (size_t i = 0; i < g_layout.box_count; i++) {
        const br_box_t *b = &g_layout.boxes[i];
        if (b->element_index >= 0 &&
            element_is_descendant(app->doc, b->element_index, target)) {
            app->scroll_y = b->y > 8 ? b->y - 8 : 0;
            app->page_dirty = 1;
            app->chrome_dirty = 1;
            return 0;
        }
    }
    return -1;
}

static int doc_set_text_by_id(br_doc_t *doc, const char *id, const char *text) {
    if (doc == NULL || id == NULL || id[0] == '\0' || text == NULL) return 0;
    int target = -1;
    for (size_t i = 0; i < doc->element_count; i++) {
        if (strcmp(doc->elements[i].id, id) == 0) {
            target = (int)i;
            break;
        }
    }
    if (target < 0) return 0;

    int changed = 0;
    int wrote = 0;
    for (size_t i = 0; i < doc->run_count; i++) {
        br_run_t *run = &doc->runs[i];
        if (run->kind != BR_RUN_TEXT || run->element_index < 0) continue;
        if (!element_is_descendant(doc, run->element_index, target)) continue;
        free(run->text);
        if (!wrote) {
            run->text = strdup(text);
            wrote = 1;
        } else {
            run->text = strdup("");
        }
        if (run->text == NULL) return changed;
        changed = 1;
    }
    return changed;
}

static int url_has_scheme(const char *s) {
    if (s == NULL || s[0] == '\0') return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))) {
        return 0;
    }
    for (const char *p = s + 1; *p != '\0'; p++) {
        if (*p == ':') return 1;
        if (*p == '/' || *p == '?' || *p == '#') return 0;
        if (!( (*p >= 'a' && *p <= 'z') ||
               (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') ||
               *p == '+' || *p == '-' || *p == '.')) {
            return 0;
        }
    }
    return 0;
}

static int app_is_loading(const browser_app_t *app) {
    if (app == NULL || app->net == NULL) return 0;
    return br_net_poll((br_net_t *)app->net, 0, NULL, NULL, NULL, NULL, 0) ==
           BR_NET_LOADING;
}

static void copy_cstr(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (src == NULL) src = "";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void split_fragment(const char *url, char *base, size_t base_cap,
                           char *frag, size_t frag_cap) {
    const char *hash = url != NULL ? strchr(url, '#') : NULL;
    if (frag_cap > 0) frag[0] = '\0';
    if (hash == NULL) {
        copy_cstr(base, base_cap, url);
        return;
    }
    if (base_cap > 0) {
        size_t n = (size_t)(hash - url);
        if (n >= base_cap) n = base_cap - 1;
        memcpy(base, url, n);
        base[n] = '\0';
    }
    if (frag_cap > 0) copy_cstr(frag, frag_cap, hash + 1);
}

static void base_without_fragment(const char *src, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return;
    while (src[n] != '\0' && src[n] != '#' && n + 1 < cap) {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
}

static void base_without_query_fragment(const char *src, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return;
    while (src[n] != '\0' && src[n] != '?' && src[n] != '#' && n + 1 < cap) {
        out[n] = src[n];
        n++;
    }
    out[n] = '\0';
}

static int same_url_ignoring_fragment(const char *a, const char *b) {
    char aa[BROWSER_URL_MAX];
    char bb[BROWSER_URL_MAX];
    base_without_fragment(a != NULL ? a : "", aa, sizeof(aa));
    base_without_fragment(b != NULL ? b : "", bb, sizeof(bb));
    return strcmp(aa, bb) == 0;
}

static void normalize_url_path(char *path) {
    char *src = path;
    char *dst = path;

    while (*src != '\0') {
        if (src[0] == '/' && src[1] == '/') {
            src++;
            continue;
        }
        if (src[0] == '/' && src[1] == '.' &&
            (src[2] == '/' || src[2] == '\0')) {
            src += (src[2] == '/') ? 2 : 1;
            continue;
        }
        if (src[0] == '/' && src[1] == '.' && src[2] == '.' &&
            (src[3] == '/' || src[3] == '\0')) {
            src += (src[3] == '/') ? 3 : 2;
            if (dst > path) {
                dst--;
                while (dst > path && dst[-1] != '/') dst--;
            }
            *dst = '\0';
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    if (path[0] == '\0') strcpy(path, "/");
}

static void join_url(const char *base, const char *ref, char *out, size_t cap) {
    out[0] = '\0';
    if (cap == 0 || ref == NULL || ref[0] == '\0') return;

    if (url_has_scheme(ref)) {
        copy_cstr(out, cap, ref);
        return;
    }
    if (strncmp(ref, "//", 2) == 0) {
        const char *scheme_end = strstr(base, ":");
        if (scheme_end == NULL) return;
        snprintf(out, cap, "%.*s:%s", (int)(scheme_end - base), base, ref);
        return;
    }

    char clean_base[BROWSER_URL_MAX];
    base_without_query_fragment(base != NULL ? base : "", clean_base,
                                sizeof(clean_base));

    const char *scheme_end = strstr(clean_base, "://");
    if (scheme_end == NULL) {
        if (ref[0] == '#') copy_cstr(out, cap, clean_base);
        else copy_cstr(out, cap, ref);
        return;
    }

    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    size_t origin_len = path != NULL ? (size_t)(path - clean_base)
                                     : strlen(clean_base);
    char origin[BROWSER_URL_MAX];
    snprintf(origin, sizeof(origin), "%.*s", (int)origin_len, clean_base);

    if (ref[0] == '#') {
        snprintf(out, cap, "%s%s", clean_base, ref);
        return;
    }
    if (ref[0] == '?') {
        snprintf(out, cap, "%s%s", clean_base, ref);
        return;
    }

    char path_buf[BROWSER_URL_MAX];
    if (ref[0] == '/') {
        copy_cstr(path_buf, sizeof(path_buf), ref);
    } else {
        const char *dir = path != NULL ? strrchr(path, '/') : NULL;
        if (dir == NULL) {
            snprintf(path_buf, sizeof(path_buf), "/%s", ref);
        } else {
            size_t dir_len = (size_t)(dir - clean_base) + 1;
            if (dir_len >= sizeof(path_buf)) dir_len = sizeof(path_buf) - 1;
            snprintf(path_buf, sizeof(path_buf), "%.*s%s", (int)dir_len,
                     clean_base, ref);
            memmove(path_buf, path_buf + origin_len,
                    strlen(path_buf + origin_len) + 1);
        }
    }

    normalize_url_path(path_buf);
    snprintf(out, cap, "%s%s", origin, path_buf);
}

static void effective_base_url(const browser_app_t *app, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (app == NULL || app->doc == NULL) return;
    if (app->doc->base_href[0] != '\0') {
        const char *fallback = app->doc->base_url[0] != '\0'
                               ? app->doc->base_url : app->url;
        join_url(fallback, app->doc->base_href, out, cap);
        if (out[0] != '\0') return;
    }
    if (app->doc->base_url[0] != '\0') {
        copy_cstr(out, cap, app->doc->base_url);
        return;
    }
    copy_cstr(out, cap, app->url);
}

static void load_home(browser_app_t *app) {
    if (app->doc != NULL && app->doc->stylesheet != NULL) {
        br_css_stylesheet_destroy((br_stylesheet_t *)app->doc->stylesheet);
        app->doc->stylesheet = NULL;
    }
    br_doc_clear(app->doc);
    if (br_doc_parse_html(app->doc, kHomeHtml, strlen(kHomeHtml)) != 0) {
        snprintf(app->status, sizeof(app->status), "parse failed");
    } else {
        snprintf(app->status, sizeof(app->status),
                 "welcome — press Ctrl+L to enter a URL");
    }
    snprintf(app->url, sizeof(app->url), "about:home");
    build_and_apply_css(app);
    app->scroll_y = 0;
    app->focused_link = -1;
    app->hover_link = -1;
    app->has_queued_nav = 0;
    app->queued_url[0] = '\0';
    app->pending_fragment[0] = '\0';
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
    if (net == NULL) {
        br_doc_destroy(app->doc);
        app->doc = NULL;
        return -1;
    }
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
    if (app->doc != NULL && app->doc->stylesheet != NULL) {
        br_css_stylesheet_destroy((br_stylesheet_t *)app->doc->stylesheet);
        app->doc->stylesheet = NULL;
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
    if (url_has_scheme(url)) return;
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
    char fetch_target[BROWSER_URL_MAX];
    char fragment[BROWSER_ELEMENT_ID_MAX];
    strncpy(target, url, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    normalize_url(target, sizeof(target));
    split_fragment(target, fetch_target, sizeof(fetch_target),
                   fragment, sizeof(fragment));
    copy_cstr(app->pending_fragment, sizeof(app->pending_fragment), fragment);

    if (app_is_loading(app)) {
        copy_cstr(app->queued_url, sizeof(app->queued_url), target);
        app->has_queued_nav = 1;
        snprintf(app->status, sizeof(app->status), "queued %s", target);
        app->chrome_dirty = 1;
        return 0;
    }

    if (fragment[0] != '\0' && same_url_ignoring_fragment(fetch_target, app->url)) {
        copy_cstr(app->url, sizeof(app->url), target);
        if (scroll_to_fragment(app, fragment) == 0) {
            snprintf(app->status, sizeof(app->status), "#%s", fragment);
        } else {
            snprintf(app->status, sizeof(app->status), "anchor not found: #%s",
                     fragment);
        }
        app->pending_fragment[0] = '\0';
        app->chrome_dirty = 1;
        return 0;
    }

    if (strcmp(fetch_target, "about:home") == 0) {
        load_home(app);
        if (fragment[0] != '\0') {
            scroll_to_fragment(app, fragment);
            app->pending_fragment[0] = '\0';
        }
        copy_cstr(app->url, sizeof(app->url), target);
        push_history(app, app->url);
        return 0;
    }

    snprintf(app->status, sizeof(app->status), "loading %s…", target);
    app->chrome_dirty = 1;

    if (br_net_start((br_net_t *)app->net, fetch_target) != 0) {
        snprintf(app->status, sizeof(app->status), "error: fetch start failed");
        return -1;
    }
    /* Optimistically show the URL the user requested while loading. */
    strncpy(app->url, target, sizeof(app->url) - 1);
    app->url[sizeof(app->url) - 1] = '\0';
    return 0;
}

/* Build doc->stylesheet from:
 *   1. the built-in user-agent rules,
 *   2. external <link rel=stylesheet> sheets (fetched synchronously),
 *   3. accumulated <style> blocks.
 * Then run the cascade across doc->elements. */
static void build_and_apply_css(browser_app_t *app) {
    if (app->doc == NULL) return;
    br_doc_t *doc = app->doc;

    if (doc->stylesheet != NULL) {
        br_css_stylesheet_destroy((br_stylesheet_t *)doc->stylesheet);
        doc->stylesheet = NULL;
    }
    br_stylesheet_t *ss = br_css_stylesheet_create();
    if (ss == NULL) return;

    /* (1) user agent first so author rules can override. */
    br_css_apply_user_agent(ss);

    /* (2) external <link> sheets. Synchronous — keeps the cascade well
     * defined before layout. We cap how many we'll fetch per page so a
     * pathological case can't hang the UI. */
    int fetched = 0;
    for (int i = 0; i < doc->ext_sheet_count && fetched < 8; i++) {
        char resolved[BROWSER_URL_MAX];
        char base[BROWSER_URL_MAX];
        effective_base_url(app, base, sizeof(base));
        resolve_url_against_base(base, app->url, doc->ext_sheets[i],
                                 resolved, sizeof(resolved));
        if (resolved[0] == '\0') continue;
        snprintf(app->status, sizeof(app->status), "css %d/%d…",
                 fetched + 1, doc->ext_sheet_count);
        app->chrome_dirty = 1;
        char *body = NULL;
        size_t len = 0;
        const char *err = NULL;
        if (browser_fetch_url(resolved, &body, &len, NULL, &err) == 0) {
            br_css_parse_into(ss, body, len);
            free(body);
        }
        fetched++;
    }

    /* (3) inline <style> contents. */
    if (doc->css_text != NULL && doc->css_text_len > 0) {
        br_css_parse_into(ss, doc->css_text, doc->css_text_len);
    }

    br_css_apply_to_doc(doc, ss);
    doc->stylesheet = ss;
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
    char final_base[BROWSER_URL_MAX];
    split_fragment(show_url, final_base, sizeof(final_base), NULL, 0);
    if (app->pending_fragment[0] != '\0') {
        snprintf(app->url, sizeof(app->url), "%s#%s",
                 final_base, app->pending_fragment);
    } else {
        strncpy(app->url, show_url, sizeof(app->url) - 1);
        app->url[sizeof(app->url) - 1] = '\0';
    }
    strncpy(app->doc->base_url, final_base, sizeof(app->doc->base_url) - 1);
    app->doc->base_url[sizeof(app->doc->base_url) - 1] = '\0';

    push_history(app, app->url);

    /* Resolve CSS *before* images so layout can pick up any
     * display:none / visibility:hidden choices the author made. */
    build_and_apply_css(app);

    /* Fetch and decode images referenced in the page. */
    if (app->doc->image_count > 0) {
        load_page_images(app);
    }

    run_page_scripts(app, len);

    if (app->doc->script_entry_count > 0 && strstr(app->status, "JS ") != NULL) {
        /* leave JS result in status */
    } else if (doc_looks_js_heavy(app->doc)) {
        snprintf(app->status, sizeof(app->status),
                 "%zu B • %s • JS-heavy page",
                 len, app->doc->title[0] != '\0' ? app->doc->title : "(no title)");
    } else {
        snprintf(app->status, sizeof(app->status), "%zu B • %s",
                 len, app->doc->title[0] != '\0' ? app->doc->title : "(no title)");
    }
    app->scroll_y = 0;
    app->focused_link = -1;
    app->hover_link = -1;
    g_layout_width = -1;       /* force re-layout next frame */
    app->page_dirty = 1;
    app->chrome_dirty = 1;
}

static int doc_looks_js_heavy(const br_doc_t *doc) {
    if (doc == NULL) return 0;
    if (doc->module_script_count > 0) return 1;
    if (doc->script_count >= 6) return 1;
    if (doc->js_app_hints >= 2) return 1;
    if (doc->noscript_count > 0 && doc->script_count > 0) return 1;
    return 0;
}

static void resolve_link_target(browser_app_t *app, int link_index,
                                char *out, size_t cap) {
    out[0] = '\0';
    if (link_index < 0 || (size_t)link_index >= app->doc->link_count) return;
    const char *href = app->doc->links[link_index].href;
    if (href == NULL || href[0] == '\0') return;
    char base[BROWSER_URL_MAX];
    effective_base_url(app, base, sizeof(base));
    join_url(base, href, out, cap);
}

static void resolve_url_against_base(const char *base_url, const char *url_str,
                                     const char *src, char *out, size_t cap) {
    out[0] = '\0';
    if (src == NULL || src[0] == '\0') return;
    const char *base = base_url[0] != '\0' ? base_url : url_str;
    join_url(base, src, out, cap);
}

#define BROWSER_MAX_IMAGES_PER_PAGE 32

static void load_page_images(browser_app_t *app) {
    if (app->doc == NULL) return;
    br_doc_t *doc = app->doc;
    int loaded = 0;

    for (size_t i = 0; i < doc->image_count && loaded < BROWSER_MAX_IMAGES_PER_PAGE; i++) {
        br_image_t *img = &doc->images[i];
        if (img->loaded != 0) continue;

        char resolved[BROWSER_URL_MAX];
        char base[BROWSER_URL_MAX];
        effective_base_url(app, base, sizeof(base));
        resolve_url_against_base(base, app->url, img->src, resolved,
                                 sizeof(resolved));

        if (resolved[0] == '\0') {
            img->loaded = -1;
            continue;
        }

        snprintf(app->status, sizeof(app->status), "image %d/%d…",
                 loaded + 1, (int)doc->image_count);
        app->chrome_dirty = 1;

        char *body = NULL;
        size_t len = 0;
        const char *err = NULL;
        if (browser_fetch_url(resolved, &body, &len, NULL, &err) == 0) {
            uint32_t *pixels = NULL;
            int w = 0, h = 0;
            if (br_image_decode(body, len, &pixels, &w, &h) == 0) {
                img->pixels = pixels;
                img->width = w;
                img->height = h;
                img->loaded = 1;
            } else {
                img->loaded = -1;
            }
            free(body);
        } else {
            img->loaded = -1;
        }
        loaded++;
    }
}

static int run_script_source(browser_app_t *app, js_engine_t *engine,
                             const char *label, const char *code,
                             size_t *ok_count, size_t *fail_count,
                             char *last_err, size_t last_err_cap) {
    (void)app;
    if (engine == NULL || code == NULL || code[0] == '\0') return 0;
    js_eval_result_t result;
    char err[256];
    js_eval_status_t status = js_engine_eval(engine, code, &result,
                                             err, sizeof(err));
    if (status == JS_EVAL_OK) {
        (*ok_count)++;
        return 0;
    }
    (*fail_count)++;
    snprintf(last_err, last_err_cap, "%s: %s",
             label != NULL ? label : "script", err);
    return -1;
}

static void run_page_scripts(browser_app_t *app, size_t body_len) {
    if (app == NULL || app->doc == NULL || app->doc->script_entry_count == 0) return;

    js_engine_t *engine = js_engine_create();
    if (engine == NULL) {
        snprintf(app->status, sizeof(app->status), "%zu B • JS init failed", body_len);
        return;
    }
    if (js_engine_set_host_api(engine, kBrowserHostApi, app) != 0) {
        js_engine_destroy(engine);
        snprintf(app->status, sizeof(app->status), "%zu B • JS host init failed", body_len);
        return;
    }

    size_t ok_count = 0;
    size_t fail_count = 0;
    char last_err[256] = "";

    for (size_t i = 0; i < app->doc->script_entry_count; i++) {
        br_script_t *script = &app->doc->scripts[i];
        if (script->src != NULL && script->src[0] != '\0') {
            char resolved[BROWSER_URL_MAX];
            char base[BROWSER_URL_MAX];
            effective_base_url(app, base, sizeof(base));
            resolve_url_against_base(base, app->url, script->src,
                                     resolved, sizeof(resolved));
            if (resolved[0] == '\0') {
                fail_count++;
                snprintf(last_err, sizeof(last_err), "%s: bad src", script->src);
                continue;
            }
            char *body = NULL;
            size_t len = 0;
            const char *err = NULL;
            if (browser_fetch_url(resolved, &body, &len, NULL, &err) != 0) {
                fail_count++;
                snprintf(last_err, sizeof(last_err), "%s: %s",
                         script->src, err != NULL ? err : "fetch failed");
                continue;
            }
            run_script_source(app, engine, script->src, body,
                              &ok_count, &fail_count, last_err, sizeof(last_err));
            free(body);
        } else {
            run_script_source(app, engine, "inline script", script->code,
                              &ok_count, &fail_count, last_err, sizeof(last_err));
        }
    }

    js_engine_destroy(engine);

    if (fail_count > 0) {
        snprintf(app->status, sizeof(app->status),
                 "%zu B • JS %zu ok, %zu failed • %s",
                 body_len, ok_count, fail_count,
                 last_err[0] != '\0' ? last_err : "runtime error");
    } else if (ok_count > 0) {
        snprintf(app->status, sizeof(app->status),
                 "%zu B • JS %zu ok • %s",
                 body_len, ok_count,
                 app->doc->title[0] != '\0' ? app->doc->title : "(no title)");
    }
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
        if (app->has_queued_nav) {
            char queued[BROWSER_URL_MAX];
            copy_cstr(queued, sizeof(queued), app->queued_url);
            app->has_queued_nav = 0;
            app->queued_url[0] = '\0';
            free(body);
            free(final_url);
            br_app_navigate(app, queued);
            body = NULL;
            final_url = NULL;
        } else {
            apply_fetch_result(app, body, len, final_url, err,
                               s == BR_NET_DONE_OK);
        }
        free(body);
        free(final_url);
        net_state = br_net_poll((br_net_t *)app->net, 0, NULL, NULL, NULL,
                                NULL, 0);
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
        if (app->pending_fragment[0] != '\0') {
            scroll_to_fragment(app, app->pending_fragment);
            app->pending_fragment[0] = '\0';
        }
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
