/*
 * browser - OneTool GUI web browser, built on ranal + srapi.
 *
 * Two run modes:
 *   - standalone:  ranal_init() (direct DRM/fbdev rendering)
 *   - swm client:  ranal_init_swm() (windowed inside the swm compositor)
 *
 * The browser fetches an HTTP(S) URL with libs/net, runs the bytes through
 * a tag-stripping HTML tokenizer (parse/) that also reconstructs a thin
 * element tree (parent + id + class + inline style), lays the inline runs
 * out into a paged column at paint time (render/), and exposes a URL bar +
 * status line drawn by the chrome (ui/).
 *
 * Styling is computed by the CSS engine (parse/css/) using:
 *   - a built-in user-agent stylesheet (initial values + tag defaults),
 *   - any <style> blocks collected during HTML parsing,
 *   - external <link rel=stylesheet> sheets fetched after the page lands,
 *   - inline style="..." attributes per element.
 * The result is a br_computed_style_t attached to each element; runs read
 * from their element's computed style at paint time.
 */
#ifndef ONETOOL_TOOLS_GUI_BROWSER_H
#define ONETOOL_TOOLS_GUI_BROWSER_H

#include <stddef.h>
#include <stdint.h>

#define BROWSER_VERSION "0.1"

#define BROWSER_URL_MAX         2048
#define BROWSER_TITLE_MAX       256
#define BROWSER_STATUS_MAX      256
#define BROWSER_DEFAULT_W       900
#define BROWSER_DEFAULT_H       640
#define BROWSER_CHROME_HEIGHT   48
#define BROWSER_STATUS_HEIGHT   18

#define BROWSER_ELEMENT_TAG_MAX 24
#define BROWSER_ELEMENT_ID_MAX  64
#define BROWSER_ELEMENT_CLASS_MAX 32
#define BROWSER_ELEMENT_CLASSES 6
#define BROWSER_EXT_SHEETS_MAX  16

typedef enum {
    BR_RUN_TEXT = 0,
    BR_RUN_BREAK,        /* forced line break (<br>) */
    BR_RUN_PARAGRAPH,    /* paragraph end / block break */
    BR_RUN_RULE,         /* <hr> */
    BR_RUN_IMAGE,        /* inline/block image */
} br_run_kind_t;

typedef enum {
    BR_STYLE_NORMAL = 0,
    BR_STYLE_H1,
    BR_STYLE_H2,
    BR_STYLE_H3,
    BR_STYLE_BOLD,
    BR_STYLE_ITALIC,
    BR_STYLE_LINK,
    BR_STYLE_CODE,
    BR_STYLE_PRE,
    BR_STYLE_LIST_BULLET,
} br_style_t;

/* Computed style for a single element after the cascade runs. The fields
 * here are the subset the renderer actually uses — colour, weight, style,
 * decoration, scale, and a visibility flag. Anything CSS expresses that
 * the renderer doesn't model (margins, floats, etc.) is ignored. */
typedef struct {
    uint32_t color;          /* ARGB32 foreground */
    uint32_t bg_color;       /* ARGB32 background, 0 == transparent */
    int      bold;           /* font-weight ≥ 600 */
    int      italic;         /* font-style: italic | oblique */
    int      underline;      /* text-decoration includes underline */
    int      strike;         /* text-decoration includes line-through */
    int      font_scale;     /* 1 or 2 */
    int      hidden;         /* display:none / visibility:hidden */
    int      monospace;      /* font-family hints at monospace (code/tt/kbd default) */
    int      have_color;     /* used by cascade as "value was set" tracking */
    int      have_bg;
    int      have_scale;
} br_computed_style_t;

typedef struct {
    char tag[BROWSER_ELEMENT_TAG_MAX];
    char id[BROWSER_ELEMENT_ID_MAX];
    char classes[BROWSER_ELEMENT_CLASSES][BROWSER_ELEMENT_CLASS_MAX];
    int  class_count;
    char *inline_style;            /* raw style="..." text, owned, may be NULL */
    int  parent;                   /* index into doc->elements, -1 for root */
    br_computed_style_t computed;
} br_element_t;

typedef struct {
    br_run_kind_t kind;
    br_style_t    style;
    char         *text;          /* utf-8, owned; NULL for non-TEXT runs */
    int           link_index;    /* index into doc->links[], -1 if none */
    int           image_index;   /* index into doc->images[], -1 if none */
    int           element_index; /* index into doc->elements[], -1 if none */
} br_run_t;

typedef struct {
    char *href;                  /* owned */
    int   first_run;             /* first run that belongs to this link */
} br_link_t;

typedef struct {
    uint32_t *pixels;            /* ARGB32, malloc'd; NULL until decoded */
    int       width;
    int       height;
    char     *src;               /* raw src URL from HTML, owned */
    int       loaded;            /* 0=pending, 1=ok, -1=failed */
} br_image_t;

typedef struct {
    br_run_t      *runs;
    size_t         run_count;
    size_t         run_cap;
    br_link_t     *links;
    size_t         link_count;
    size_t         link_cap;
    br_image_t    *images;
    size_t         image_count;
    size_t         image_cap;
    br_element_t  *elements;
    size_t         element_count;
    size_t         element_cap;
    /* Accumulated <style> contents (concatenated, separated by '\n'). */
    char          *css_text;
    size_t         css_text_len;
    size_t         css_text_cap;
    /* Hrefs collected from <link rel=stylesheet>, owned. */
    char          *ext_sheets[BROWSER_EXT_SHEETS_MAX];
    int            ext_sheet_count;
    char           base_href[BROWSER_URL_MAX];
    /* Parsed stylesheet, owned. Created by app after HTML+ext-fetch finish. */
    void          *stylesheet;     /* br_stylesheet_t * */
    char           title[BROWSER_TITLE_MAX];
    char           base_url[BROWSER_URL_MAX];
} br_doc_t;

/* Bounding box recorded for a link during paint — used for hit testing. */
typedef struct {
    int   link_index;
    int   x, y, w, h;
} br_link_rect_t;

typedef struct {
    br_link_rect_t *rects;
    size_t          count;
    size_t          cap;
} br_link_hits_t;

/* Top-level app state. */
typedef struct browser_app {
    /* Window / surfaces */
    int    win_w;
    int    win_h;
    void  *page_surface;   /* ranal_surface_t * */
    int    page_w;
    int    page_h;

    /* Document */
    br_doc_t      *doc;
    br_link_hits_t hits;

    /* Navigation */
    char   url[BROWSER_URL_MAX];
    char   status[BROWSER_STATUS_MAX];
    char   queued_url[BROWSER_URL_MAX];
    char   pending_fragment[BROWSER_ELEMENT_ID_MAX];
    char   url_edit[BROWSER_URL_MAX];
    size_t url_edit_len;
    int    url_focused;
    size_t url_cursor;
    int    has_queued_nav;

    /* Page scrolling */
    int    scroll_y;
    int    content_h;

    /* Focused link (keyboard navigation) */
    int    focused_link;
    int    hover_link;     /* link under the mouse, -1 if none */
    int    mouse_x;
    int    mouse_y;

    /* History (simple back stack) */
    char  *history[64];
    int    history_count;

    /* Async fetcher state — opaque to consumers, set up by app_init. */
    void  *net;            /* br_net_t * */

    /* UI redraw flags. The chrome (URL bar / status) and the page are
     * tracked independently so URL-bar typing doesn't force a full page
     * repaint. */
    int    chrome_dirty;
    int    page_dirty;

    /* Navigation request: when set, app_frame() starts a background
     * fetch for app->url and clears the flag. */
    int    pending_navigate;
} browser_app_t;

#endif
