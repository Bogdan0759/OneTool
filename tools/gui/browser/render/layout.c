/*
 * Layout: walk the inline-run stream and produce positioned word boxes.
 *
 * Word wrapping is greedy: we measure each word in pixels using the run's
 * scale + style (H1/H2 are drawn at 2x, everything else at 1x). When a
 * word doesn't fit on the current line we drop to a fresh line. Lines are
 * separated by RUN_BREAK; RUN_PARAGRAPH inserts a half-line gap.
 *
 * Outputs a flat array of br_box_t boxes, each pointing back into the
 * borrowed run text (so the layout becomes invalid if the doc is freed).
 */
#include "layout.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <ranal/ranal.h>

#define GLYPH_W  RANAL_GLYPH_WIDTH
#define GLYPH_H  RANAL_GLYPH_HEIGHT
#define ADVANCE  RANAL_FONT_ADVANCE_X
#define LINE_H   (RANAL_FONT_ADVANCE_Y + 4)

void br_layout_init(br_layout_t *l) {
    memset(l, 0, sizeof(*l));
}

void br_layout_clear(br_layout_t *l) {
    free(l->boxes);
    l->boxes = NULL;
    l->box_count = l->box_cap = 0;
    l->content_h = 0;
}

static int layout_grow(br_layout_t *l) {
    size_t want = l->box_cap == 0 ? 256 : l->box_cap * 2;
    br_box_t *p = (br_box_t *)realloc(l->boxes, want * sizeof(br_box_t));
    if (p == NULL) return -1;
    l->boxes = p;
    l->box_cap = want;
    return 0;
}

static int style_scale(br_style_t s) {
    if (s == BR_STYLE_H1) return 2;
    if (s == BR_STYLE_H2) return 2;
    return 1;
}

static int style_bold(br_style_t s) {
    return s == BR_STYLE_H1 || s == BR_STYLE_H2 || s == BR_STYLE_H3 ||
           s == BR_STYLE_BOLD;
}

static int style_underline(br_style_t s) {
    return s == BR_STYLE_LINK;
}

/* Count UTF-8 codepoints in a span, for advance computation. */
static int utf8_count(const char *s, int len) {
    int n = 0;
    for (int i = 0; i < len; ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) i++;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i++;
        n++;
    }
    return n;
}

/* Length of one utf-8 codepoint starting at byte 'i'. */
static int utf8_skip(const char *s, int i, int len) {
    if (i >= len) return 0;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int word_advance_px(const char *text, int start, int end, int scale) {
    int cps = utf8_count(text + start, end - start);
    return cps * ADVANCE * scale;
}

static int emit_box(br_layout_t *l, br_box_t b) {
    if (l->box_count == l->box_cap && layout_grow(l) != 0) return -1;
    l->boxes[l->box_count++] = b;
    return 0;
}

/* For PRE text runs we preserve whitespace and never break in the middle.
 * For everything else we split on whitespace and wrap. */
int br_layout_build(br_layout_t *l, const br_doc_t *doc, int content_width) {
    br_layout_clear(l);
    if (doc == NULL) return 0;
    if (content_width < 80) content_width = 80;

    int x = 0, y = 0;
    int line_h = LINE_H;       /* height of current line */
    int line_top = 0;          /* y of current line top */

    /* Track which run on this line had the biggest scale, to set line_h. */
    for (size_t i = 0; i < doc->run_count; i++) {
        const br_run_t *r = &doc->runs[i];

        if (r->kind == BR_RUN_BREAK) {
            y = line_top + line_h;
            x = 0;
            line_top = y;
            line_h = LINE_H;
            continue;
        }
        if (r->kind == BR_RUN_PARAGRAPH) {
            y = line_top + line_h;
            y += LINE_H / 2;
            x = 0;
            line_top = y;
            line_h = LINE_H;
            continue;
        }
        if (r->kind == BR_RUN_RULE) {
            /* horizontal rule occupies its own row */
            y = line_top + line_h;
            br_box_t b = {0};
            b.x = 0; b.y = y + LINE_H / 2 - 1;
            b.w = content_width; b.h = 2;
            b.scale = 1;
            b.style = BR_STYLE_NORMAL;
            b.text = NULL; b.text_len = 0;
            b.link_index = -1;
            emit_box(l, b);
            y += LINE_H;
            x = 0;
            line_top = y;
            line_h = LINE_H;
            continue;
        }

        if (r->text == NULL) continue;
        int scale = style_scale(r->style);
        int bold = style_bold(r->style);
        int under = style_underline(r->style);
        int run_line_h = GLYPH_H * scale + 4;
        if (run_line_h > line_h) line_h = run_line_h;

        const char *t = r->text;
        int tlen = (int)strlen(t);
        int j = 0;
        int is_pre = (r->style == BR_STYLE_PRE);

        if (is_pre) {
            /* For pre, split on '\n' but otherwise keep all bytes in one box. */
            while (j < tlen) {
                int seg_start = j;
                while (j < tlen && t[j] != '\n') j++;
                int seg_end = j;
                if (seg_end > seg_start) {
                    int w = word_advance_px(t, seg_start, seg_end, scale);
                    br_box_t b = {0};
                    b.x = x; b.y = line_top;
                    b.w = w; b.h = GLYPH_H * scale;
                    b.scale = scale;
                    b.bold = bold;
                    b.underline = under;
                    b.style = r->style;
                    b.text = t + seg_start;
                    b.text_len = seg_end - seg_start;
                    b.link_index = r->link_index;
                    if (emit_box(l, b) != 0) return -1;
                    x += w;
                }
                if (j < tlen && t[j] == '\n') {
                    j++;
                    y = line_top + line_h;
                    x = 0;
                    line_top = y;
                    line_h = run_line_h;
                }
            }
            continue;
        }

        while (j < tlen) {
            /* Eat leading spaces. They contribute to advance but no box. */
            while (j < tlen && t[j] == ' ') {
                if (x > 0) {
                    x += ADVANCE * scale;
                    if (x > content_width) {
                        y = line_top + line_h;
                        x = 0;
                        line_top = y;
                        line_h = run_line_h;
                    }
                }
                j++;
            }
            if (j >= tlen) break;

            int seg_start = j;
            while (j < tlen && t[j] != ' ') j += utf8_skip(t, j, tlen);
            int seg_end = j;
            int w = word_advance_px(t, seg_start, seg_end, scale);

            /* If the word would overflow and we're not at the line start,
               wrap to a new line. */
            if (x + w > content_width && x > 0) {
                y = line_top + line_h;
                x = 0;
                line_top = y;
                line_h = run_line_h;
            }

            /* Long words that still don't fit: split codepoint by codepoint. */
            if (w > content_width) {
                int cur_start = seg_start;
                int cur_w = 0;
                int k = seg_start;
                while (k < seg_end) {
                    int adv = utf8_skip(t, k, seg_end);
                    int dw = ADVANCE * scale;
                    if (cur_w + dw > content_width && cur_start < k) {
                        br_box_t b = {0};
                        b.x = x; b.y = line_top;
                        b.w = cur_w; b.h = GLYPH_H * scale;
                        b.scale = scale;
                        b.bold = bold;
                        b.underline = under;
                        b.style = r->style;
                        b.text = t + cur_start;
                        b.text_len = k - cur_start;
                        b.link_index = r->link_index;
                        if (emit_box(l, b) != 0) return -1;
                        y = line_top + line_h;
                        x = 0;
                        line_top = y;
                        line_h = run_line_h;
                        cur_start = k;
                        cur_w = 0;
                    }
                    cur_w += dw;
                    k += adv;
                }
                if (cur_start < seg_end) {
                    br_box_t b = {0};
                    b.x = x; b.y = line_top;
                    b.w = cur_w; b.h = GLYPH_H * scale;
                    b.scale = scale;
                    b.bold = bold;
                    b.underline = under;
                    b.style = r->style;
                    b.text = t + cur_start;
                    b.text_len = seg_end - cur_start;
                    b.link_index = r->link_index;
                    if (emit_box(l, b) != 0) return -1;
                    x += cur_w;
                }
                continue;
            }

            br_box_t b = {0};
            b.x = x; b.y = line_top;
            b.w = w; b.h = GLYPH_H * scale;
            b.scale = scale;
            b.bold = bold;
            b.underline = under;
            b.style = r->style;
            b.text = t + seg_start;
            b.text_len = seg_end - seg_start;
            b.link_index = r->link_index;
            if (emit_box(l, b) != 0) return -1;
            x += w;
        }
    }

    l->content_h = line_top + line_h;
    return 0;
}
