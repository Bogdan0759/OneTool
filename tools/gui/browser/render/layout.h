#ifndef ONETOOL_TOOLS_GUI_BROWSER_RENDER_LAYOUT_H
#define ONETOOL_TOOLS_GUI_BROWSER_RENDER_LAYOUT_H

#include "../browser.h"

/* A single drawn glyph cluster (a word). */
typedef struct {
    int      x, y;        /* top-left, in document-local pixels */
    int      w, h;        /* size in pixels (after scale) */
    int      scale;       /* 1 or 2 */
    int      bold;
    int      underline;
    br_style_t style;
    const char *text;     /* borrowed pointer into a run's text buffer */
    int      text_len;    /* byte count */
    int      link_index;  /* -1 if not a link */
} br_box_t;

typedef struct {
    br_box_t *boxes;
    size_t    box_count;
    size_t    box_cap;
    int       content_h;  /* total document height in pixels */
} br_layout_t;

void br_layout_init(br_layout_t *l);
void br_layout_clear(br_layout_t *l);
int  br_layout_build(br_layout_t *l, const br_doc_t *doc, int content_width);

#endif
