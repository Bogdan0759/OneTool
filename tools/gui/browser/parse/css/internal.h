/*
 * Internal types shared between CSS engine sources. Not part of the
 * public API (browser code outside parse/css/ should use css.h).
 */
#ifndef ONETOOL_TOOLS_GUI_BROWSER_PARSE_CSS_INTERNAL_H
#define ONETOOL_TOOLS_GUI_BROWSER_PARSE_CSS_INTERNAL_H

#include "css.h"

#include <stddef.h>
#include <stdint.h>

/* ----- properties ----- */

typedef enum {
    BR_CSS_PROP_UNKNOWN = 0,
    BR_CSS_PROP_COLOR,
    BR_CSS_PROP_BACKGROUND_COLOR,
    BR_CSS_PROP_BACKGROUND,
    BR_CSS_PROP_FONT_WEIGHT,
    BR_CSS_PROP_FONT_STYLE,
    BR_CSS_PROP_FONT_SIZE,
    BR_CSS_PROP_FONT_FAMILY,
    BR_CSS_PROP_FONT,
    BR_CSS_PROP_TEXT_DECORATION,
    BR_CSS_PROP_TEXT_DECORATION_LINE,
    BR_CSS_PROP_TEXT_ALIGN,
    BR_CSS_PROP_DISPLAY,
    BR_CSS_PROP_VISIBILITY,
    BR_CSS_PROP_COUNT
} br_css_prop_t;

/* Properties that inherit by default. Indexed by br_css_prop_t. */
int br_css_prop_inherits(br_css_prop_t p);

/* Lookup a property identifier from its lowercase name. Returns
 * BR_CSS_PROP_UNKNOWN for unrecognised properties. */
br_css_prop_t br_css_prop_lookup(const char *name, size_t len);

/* ----- values ----- */

typedef enum {
    BR_CSSV_NONE = 0,
    BR_CSSV_KEYWORD,
    BR_CSSV_COLOR,
    BR_CSSV_NUMBER,
    BR_CSSV_LENGTH,    /* with unit: px/em/rem/% */
    BR_CSSV_STRING,
} br_css_value_kind_t;

typedef enum {
    BR_CSS_UNIT_NONE = 0,
    BR_CSS_UNIT_PX,
    BR_CSS_UNIT_EM,
    BR_CSS_UNIT_REM,
    BR_CSS_UNIT_PCT,
    BR_CSS_UNIT_PT,
} br_css_unit_t;

typedef struct {
    br_css_value_kind_t kind;
    /* keyword and string buffers share storage */
    char     keyword[40];
    uint32_t color;     /* 0xAARRGGBB; alpha 0 means "transparent" */
    double   num;
    br_css_unit_t unit;
} br_css_value_t;

/* Parse a single value from `src`/`len` into `out`. Returns 0 on success,
 * -1 if the input cannot be interpreted. */
int br_css_value_parse(const char *src, size_t len, br_css_value_t *out);

/* Named colour lookup (lowercase). Returns 1 on hit. */
int br_css_named_color(const char *name, size_t len, uint32_t *out_color);

/* ----- selectors ----- */

typedef struct {
    char tag[BROWSER_ELEMENT_TAG_MAX];                  /* empty = "*" */
    char id[BROWSER_ELEMENT_ID_MAX];                    /* empty = no id */
    char classes[BROWSER_ELEMENT_CLASSES][BROWSER_ELEMENT_CLASS_MAX];
    int  class_count;
    char attr_name[BROWSER_ATTR_NAME_MAX];
    char attr_value[BROWSER_ATTR_VALUE_MAX];
    int  attr_has_value;
    /* Pseudo flags. Only :hover is supported but parsed so :pseudo doesn't
     * break matching. */
    int  pseudo_link;     /* :link / :visited match a tag, no extra constraint */
    int  pseudo_hover;
    int  pseudo_first;    /* :first-child — approximated as "no preceding sibling" (best-effort) */
    int  pseudo_unsupported; /* set to 1 if we saw a pseudo we don't model */
} br_css_compound_t;

#define BR_CSS_SELECTOR_PARTS 6

typedef struct {
    br_css_compound_t parts[BR_CSS_SELECTOR_PARTS];
    int    part_count;   /* >=1, last entry is the subject */
    int    specificity;  /* a*100 + b*10 + c */
} br_css_selector_t;

/* Parse one comma-free selector from `src`/`len` (possibly with descendant
 * combinators). Returns 0 on success, -1 on parse error. */
int br_css_selector_parse(const char *src, size_t len, br_css_selector_t *out);

/* Returns 1 if `sel` matches the element at `el_index` of `doc`. */
int br_css_selector_matches(const br_css_selector_t *sel,
                            const br_doc_t *doc, int el_index);

/* ----- declarations & rules ----- */

typedef struct {
    br_css_prop_t  prop;
    br_css_value_t value;
    int            important;
} br_css_decl_t;

typedef struct {
    br_css_selector_t *selectors;
    int                selector_count;
    br_css_decl_t     *decls;
    int                decl_count;
    int                order;     /* source-order index, for cascade tie-break */
} br_css_rule_t;

struct br_stylesheet {
    br_css_rule_t *rules;
    int            rule_count;
    int            rule_cap;
};

int br_css_stylesheet_push_rule(br_stylesheet_t *ss, br_css_rule_t rule);

/* Parse a declaration list (the body between `{` and `}` ) and append all
 * declarations into `out_decls`. `*out_count` is updated. Allocates
 * `*out_decls` as needed via realloc. Returns 0 on success. */
int br_css_parse_decl_block(const char *src, size_t len,
                            br_css_decl_t **out_decls, int *out_count);

/* Apply a single declaration to a br_computed_style_t. parent_style is
 * used to resolve `inherit`. */
void br_css_apply_decl(br_computed_style_t *cs, const br_css_decl_t *decl,
                       const br_computed_style_t *parent);

/* Built-in user-agent rules — a small set that gives bare HTML reasonable
 * defaults (link colour, heading weight, monospace for <code>, etc.).
 * Idempotent on the stylesheet. */
void br_css_apply_user_agent(br_stylesheet_t *ss);

#endif
