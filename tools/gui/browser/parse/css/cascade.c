/*
 * Cascade + inheritance.
 *
 * For each element in document order:
 *  1. start from the initial style;
 *  2. inherit from parent for properties marked as inherited;
 *  3. collect every declaration that comes from a matching rule;
 *  4. sort declarations by (important, specificity, source-order);
 *  5. apply them in order so the highest-priority value wins;
 *  6. parse and apply inline style="..." last (always wins barring !important
 *     in author sheets).
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

/* ----- decl application ----- */

static int keyword_eq(const br_css_value_t *v, const char *kw) {
    return v->kind == BR_CSSV_KEYWORD && strcmp(v->keyword, kw) == 0;
}

/* Convert "bold"/"normal"/numeric weight to bold-or-not. */
static int weight_to_bold(const br_css_value_t *v) {
    if (v->kind == BR_CSSV_KEYWORD) {
        if (strcmp(v->keyword, "bold") == 0 ||
            strcmp(v->keyword, "bolder") == 0) return 1;
        if (strcmp(v->keyword, "normal") == 0 ||
            strcmp(v->keyword, "lighter") == 0) return 0;
    }
    if (v->kind == BR_CSSV_NUMBER) return v->num >= 600.0 ? 1 : 0;
    return 0;
}

static int italic_from_value(const br_css_value_t *v) {
    if (v->kind != BR_CSSV_KEYWORD) return 0;
    return strcmp(v->keyword, "italic") == 0 ||
           strcmp(v->keyword, "oblique") == 0;
}

/* font-size: support keywords + a small set of units. We map to a rough
 * "scale" (1 = base, 2 = large) since the renderer is bitmap-fonted. */
static int font_size_to_scale(const br_css_value_t *v) {
    if (v->kind == BR_CSSV_KEYWORD) {
        if (strcmp(v->keyword, "xx-large") == 0 ||
            strcmp(v->keyword, "x-large") == 0 ||
            strcmp(v->keyword, "large") == 0 ||
            strcmp(v->keyword, "larger") == 0) return 2;
        if (strcmp(v->keyword, "small") == 0 ||
            strcmp(v->keyword, "x-small") == 0 ||
            strcmp(v->keyword, "xx-small") == 0 ||
            strcmp(v->keyword, "smaller") == 0) return 1;
        return 1;
    }
    if (v->kind == BR_CSSV_LENGTH) {
        switch (v->unit) {
            case BR_CSS_UNIT_PX: return v->num >= 20.0 ? 2 : 1;
            case BR_CSS_UNIT_PT: return v->num >= 16.0 ? 2 : 1;
            case BR_CSS_UNIT_EM:
            case BR_CSS_UNIT_REM: return v->num >= 1.4 ? 2 : 1;
            case BR_CSS_UNIT_PCT: return v->num >= 140.0 ? 2 : 1;
            default: return 1;
        }
    }
    if (v->kind == BR_CSSV_NUMBER) {
        return v->num >= 20.0 ? 2 : 1;
    }
    return 1;
}

/* Parse a text-decoration string ("underline overline line-through none"). */
static void apply_text_decoration(br_computed_style_t *cs,
                                  const br_css_value_t *v) {
    if (v->kind != BR_CSSV_KEYWORD) return;
    if (strcmp(v->keyword, "none") == 0) {
        cs->underline = 0;
        cs->strike = 0;
        return;
    }
    /* Look for substrings. */
    if (strstr(v->keyword, "underline") != NULL) cs->underline = 1;
    if (strstr(v->keyword, "line-through") != NULL) cs->strike = 1;
}

/* font-family heuristic — monospace if keyword contains "mono"/"courier"/"console". */
static void apply_font_family(br_computed_style_t *cs,
                              const br_css_value_t *v) {
    const char *s = NULL;
    if (v->kind == BR_CSSV_KEYWORD) s = v->keyword;
    else if (v->kind == BR_CSSV_STRING) s = v->keyword;
    if (s == NULL) return;
    if (strstr(s, "mono") != NULL || strstr(s, "courier") != NULL ||
        strstr(s, "console") != NULL || strstr(s, "fixed") != NULL) {
        cs->monospace = 1;
    } else {
        cs->monospace = 0;
    }
}

/* Shorthand: background → background-color only (we ignore images). */
static void apply_background_shorthand(br_computed_style_t *cs,
                                       const br_css_value_t *v) {
    if (v->kind == BR_CSSV_COLOR) {
        cs->bg_color = v->color;
        cs->have_bg = 1;
    } else if (v->kind == BR_CSSV_KEYWORD) {
        if (strcmp(v->keyword, "transparent") == 0 ||
            strcmp(v->keyword, "none") == 0) {
            cs->bg_color = 0;
            cs->have_bg = 1;
        }
    }
}

/* Shorthand: font → just look for "bold"/"italic" inside the keyword. */
static void apply_font_shorthand(br_computed_style_t *cs,
                                 const br_css_value_t *v) {
    if (v->kind != BR_CSSV_KEYWORD) return;
    if (strstr(v->keyword, "bold") != NULL) cs->bold = 1;
    if (strstr(v->keyword, "italic") != NULL ||
        strstr(v->keyword, "oblique") != NULL) cs->italic = 1;
    if (strstr(v->keyword, "mono") != NULL ||
        strstr(v->keyword, "courier") != NULL) cs->monospace = 1;
}

void br_css_apply_decl(br_computed_style_t *cs, const br_css_decl_t *decl,
                       const br_computed_style_t *parent) {
    /* Handle the inherit / initial / unset keywords up front. */
    if (decl->value.kind == BR_CSSV_KEYWORD) {
        if (strcmp(decl->value.keyword, "inherit") == 0) {
            if (parent == NULL) return;
            switch (decl->prop) {
                case BR_CSS_PROP_COLOR:
                    cs->color = parent->color;
                    cs->have_color = parent->have_color;
                    return;
                case BR_CSS_PROP_BACKGROUND_COLOR:
                case BR_CSS_PROP_BACKGROUND:
                    cs->bg_color = parent->bg_color;
                    cs->have_bg = parent->have_bg;
                    return;
                case BR_CSS_PROP_FONT_WEIGHT:
                    cs->bold = parent->bold;
                    return;
                case BR_CSS_PROP_FONT_STYLE:
                    cs->italic = parent->italic;
                    return;
                case BR_CSS_PROP_FONT_SIZE:
                case BR_CSS_PROP_FONT:
                    cs->font_scale = parent->font_scale;
                    cs->have_scale = parent->have_scale;
                    return;
                case BR_CSS_PROP_FONT_FAMILY:
                    cs->monospace = parent->monospace;
                    return;
                case BR_CSS_PROP_TEXT_DECORATION:
                case BR_CSS_PROP_TEXT_DECORATION_LINE:
                    cs->underline = parent->underline;
                    cs->strike = parent->strike;
                    return;
                case BR_CSS_PROP_VISIBILITY:
                case BR_CSS_PROP_DISPLAY:
                    cs->hidden = parent->hidden;
                    return;
                default: return;
            }
        }
        if (strcmp(decl->value.keyword, "initial") == 0 ||
            strcmp(decl->value.keyword, "unset") == 0) {
            /* Reset to initial — close to no-op for most fields. */
            switch (decl->prop) {
                case BR_CSS_PROP_COLOR:            cs->have_color = 0; break;
                case BR_CSS_PROP_BACKGROUND_COLOR:
                case BR_CSS_PROP_BACKGROUND:       cs->have_bg = 0; cs->bg_color = 0; break;
                case BR_CSS_PROP_FONT_WEIGHT:      cs->bold = 0; break;
                case BR_CSS_PROP_FONT_STYLE:       cs->italic = 0; break;
                case BR_CSS_PROP_FONT_SIZE:
                case BR_CSS_PROP_FONT:             cs->have_scale = 0; cs->font_scale = 1; break;
                case BR_CSS_PROP_TEXT_DECORATION:
                case BR_CSS_PROP_TEXT_DECORATION_LINE:
                    cs->underline = 0; cs->strike = 0; break;
                case BR_CSS_PROP_DISPLAY:
                case BR_CSS_PROP_VISIBILITY:       cs->hidden = 0; break;
                default: break;
            }
            return;
        }
    }

    switch (decl->prop) {
        case BR_CSS_PROP_COLOR:
            if (decl->value.kind == BR_CSSV_COLOR) {
                cs->color = decl->value.color;
                cs->have_color = 1;
            }
            break;
        case BR_CSS_PROP_BACKGROUND_COLOR:
            if (decl->value.kind == BR_CSSV_COLOR) {
                cs->bg_color = decl->value.color;
                cs->have_bg = 1;
            } else if (keyword_eq(&decl->value, "transparent") ||
                       keyword_eq(&decl->value, "none")) {
                cs->bg_color = 0;
                cs->have_bg = 1;
            }
            break;
        case BR_CSS_PROP_BACKGROUND:
            apply_background_shorthand(cs, &decl->value);
            break;
        case BR_CSS_PROP_FONT_WEIGHT:
            cs->bold = weight_to_bold(&decl->value);
            break;
        case BR_CSS_PROP_FONT_STYLE:
            cs->italic = italic_from_value(&decl->value);
            break;
        case BR_CSS_PROP_FONT_SIZE:
            cs->font_scale = font_size_to_scale(&decl->value);
            cs->have_scale = 1;
            break;
        case BR_CSS_PROP_FONT_FAMILY:
            apply_font_family(cs, &decl->value);
            break;
        case BR_CSS_PROP_FONT:
            apply_font_shorthand(cs, &decl->value);
            break;
        case BR_CSS_PROP_TEXT_DECORATION:
        case BR_CSS_PROP_TEXT_DECORATION_LINE:
            apply_text_decoration(cs, &decl->value);
            break;
        case BR_CSS_PROP_DISPLAY:
            if (keyword_eq(&decl->value, "none")) cs->hidden = 1;
            else cs->hidden = 0;
            break;
        case BR_CSS_PROP_VISIBILITY:
            if (keyword_eq(&decl->value, "hidden") ||
                keyword_eq(&decl->value, "collapse")) cs->hidden = 1;
            else cs->hidden = 0;
            break;
        case BR_CSS_PROP_TEXT_ALIGN:
            /* renderer doesn't model alignment yet — ignore */
            break;
        default:
            break;
    }
}

/* ----- per-element collection + sort ----- */

typedef struct {
    const br_css_decl_t *decl;
    int specificity;
    int order;
    int from_inline;  /* inline style="" — overrides author rules */
} br_matched_decl_t;

static int decl_cmp(const void *a_, const void *b_) {
    const br_matched_decl_t *a = (const br_matched_decl_t *)a_;
    const br_matched_decl_t *b = (const br_matched_decl_t *)b_;
    /* Important wins. */
    if (a->decl->important != b->decl->important)
        return a->decl->important < b->decl->important ? -1 : 1;
    /* Inline style beats author rules of equal !important status. */
    if (a->from_inline != b->from_inline)
        return a->from_inline - b->from_inline;
    if (a->specificity != b->specificity)
        return a->specificity - b->specificity;
    return a->order - b->order;
}

/* Inherit from parent: for any property that's inherited but wasn't
 * explicitly set on `cs`, copy from `parent`. We approximate "wasn't set"
 * via the `have_*` flags for properties where that matters; for others
 * (bold/italic/etc.) we just copy from parent because the initial value
 * matches the unset flag-less state. */
static void inherit_from_parent(br_computed_style_t *cs,
                                const br_computed_style_t *parent) {
    if (parent == NULL) return;
    if (!cs->have_color) {
        cs->color = parent->color;
        cs->have_color = parent->have_color;
    }
    if (!cs->have_scale) {
        cs->font_scale = parent->font_scale;
        cs->have_scale = parent->have_scale;
    }
    /* bold/italic/underline/strike/monospace inherit from parent unless
     * already set non-zero on this element. We follow CSS literally only
     * for the inherited subset (font-weight/style/family + text-align).
     * Note: text-decoration in CSS doesn't actually inherit, but the
     * decoration drawn on an ancestor visually crosses descendants — we
     * approximate by inheriting it so links inside <p> stay underlined. */
    if (!cs->bold) cs->bold = parent->bold;
    if (!cs->italic) cs->italic = parent->italic;
    if (!cs->monospace) cs->monospace = parent->monospace;
    cs->underline = cs->underline || parent->underline;
    cs->strike = cs->strike || parent->strike;
    /* Visibility cascades down for `hidden`. */
    if (parent->hidden) cs->hidden = 1;
}

/* Initial style values (CSS initial values mapped to our subset). */
static br_computed_style_t initial_style(void) {
    br_computed_style_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.color = 0xFF18181Cu;     /* dark grey — matches paint.c default */
    cs.bg_color = 0;
    cs.font_scale = 1;
    return cs;
}

/* Walk `ss` for every rule whose selector matches `el_index`, collecting
 * declarations into `buf`. */
static int collect_decls(br_stylesheet_t *ss, const br_doc_t *doc, int el_index,
                         br_matched_decl_t **buf, int *count, int *cap) {
    if (ss == NULL) return 0;
    for (int r = 0; r < ss->rule_count; r++) {
        const br_css_rule_t *rule = &ss->rules[r];
        int best_spec = -1;
        for (int s = 0; s < rule->selector_count; s++) {
            if (br_css_selector_matches(&rule->selectors[s], doc, el_index)) {
                if (rule->selectors[s].specificity > best_spec)
                    best_spec = rule->selectors[s].specificity;
            }
        }
        if (best_spec < 0) continue;
        for (int d = 0; d < rule->decl_count; d++) {
            if (*count == *cap) {
                int want = *cap == 0 ? 32 : *cap * 2;
                br_matched_decl_t *p = (br_matched_decl_t *)realloc(
                    *buf, (size_t)want * sizeof(br_matched_decl_t));
                if (p == NULL) return -1;
                *buf = p;
                *cap = want;
            }
            br_matched_decl_t m;
            m.decl = &rule->decls[d];
            m.specificity = best_spec;
            m.order = rule->order;
            m.from_inline = 0;
            (*buf)[(*count)++] = m;
        }
    }
    return 0;
}

void br_css_apply_to_doc(br_doc_t *doc, br_stylesheet_t *ss) {
    if (doc == NULL) return;

    br_matched_decl_t *buf = NULL;
    int cap = 0;

    /* Per-element inline declarations need to outlive sorting, so we
     * keep a small scratch decl array per element. */
    br_css_decl_t *inline_decls = NULL;

    for (size_t i = 0; i < doc->element_count; i++) {
        br_element_t *e = &doc->elements[i];
        const br_computed_style_t *parent = (e->parent >= 0)
            ? &doc->elements[e->parent].computed : NULL;
        e->computed = initial_style();

        /* Collect matching declarations. */
        int count = 0;
        collect_decls(ss, doc, (int)i, &buf, &count, &cap);

        /* Inline style. */
        if (e->inline_style != NULL) {
            int inline_count = 0;
            inline_decls = NULL;
            br_css_parse_decl_block(e->inline_style, strlen(e->inline_style),
                                    &inline_decls, &inline_count);
            for (int k = 0; k < inline_count; k++) {
                if (count == cap) {
                    int want = cap == 0 ? 32 : cap * 2;
                    br_matched_decl_t *p = (br_matched_decl_t *)realloc(
                        buf, (size_t)want * sizeof(br_matched_decl_t));
                    if (p == NULL) break;
                    buf = p;
                    cap = want;
                }
                br_matched_decl_t m;
                m.decl = &inline_decls[k];
                m.specificity = 100000;
                m.order = 1 << 30;
                m.from_inline = 1;
                buf[count++] = m;
            }
        } else {
            inline_decls = NULL;
        }

        /* Sort ascending — last applied wins. */
        if (count > 1) qsort(buf, (size_t)count, sizeof(buf[0]), decl_cmp);

        /* Apply in order. */
        for (int k = 0; k < count; k++) {
            br_css_apply_decl(&e->computed, buf[k].decl, parent);
        }

        /* Inherit any unset inheritable properties from parent. */
        inherit_from_parent(&e->computed, parent);

        free(inline_decls);
        inline_decls = NULL;
    }

    free(buf);
}

const br_computed_style_t *br_css_run_style(const br_doc_t *doc,
                                            const br_run_t *run) {
    if (doc == NULL || run == NULL) return NULL;
    if (run->element_index < 0 ||
        (size_t)run->element_index >= doc->element_count) return NULL;
    return &doc->elements[run->element_index].computed;
}
