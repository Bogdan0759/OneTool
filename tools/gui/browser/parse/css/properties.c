/*
 * CSS property table.
 *
 * Names are mapped to the small enum the engine actually models. Properties
 * outside this list are silently ignored — the renderer can't express
 * them, so collecting them would just waste memory.
 */
#include "internal.h"

#include <string.h>

typedef struct {
    const char *name;
    br_css_prop_t prop;
    int inherits;
} br_css_prop_entry_t;

static const br_css_prop_entry_t kProps[] = {
    {"color",                  BR_CSS_PROP_COLOR,                1},
    {"background-color",       BR_CSS_PROP_BACKGROUND_COLOR,     0},
    {"background",             BR_CSS_PROP_BACKGROUND,           0},
    {"font-weight",            BR_CSS_PROP_FONT_WEIGHT,          1},
    {"font-style",             BR_CSS_PROP_FONT_STYLE,           1},
    {"font-size",              BR_CSS_PROP_FONT_SIZE,            1},
    {"font-family",            BR_CSS_PROP_FONT_FAMILY,          1},
    {"font",                   BR_CSS_PROP_FONT,                 1},
    {"text-decoration",        BR_CSS_PROP_TEXT_DECORATION,      0},
    {"text-decoration-line",   BR_CSS_PROP_TEXT_DECORATION_LINE, 0},
    {"text-align",             BR_CSS_PROP_TEXT_ALIGN,           1},
    {"white-space",            BR_CSS_PROP_WHITE_SPACE,          1},
    {"width",                  BR_CSS_PROP_WIDTH,                0},
    {"max-width",              BR_CSS_PROP_MAX_WIDTH,            0},
    {"min-width",              BR_CSS_PROP_MIN_WIDTH,            0},
    {"float",                  BR_CSS_PROP_FLOAT,                0},
    {"margin",                 BR_CSS_PROP_MARGIN,               0},
    {"margin-left",            BR_CSS_PROP_MARGIN_LEFT,          0},
    {"margin-right",           BR_CSS_PROP_MARGIN_RIGHT,         0},
    {"display",                BR_CSS_PROP_DISPLAY,              0},
    {"visibility",             BR_CSS_PROP_VISIBILITY,           1},
    {NULL, BR_CSS_PROP_UNKNOWN, 0},
};

br_css_prop_t br_css_prop_lookup(const char *name, size_t len) {
    /* Lowercase compare. */
    char buf[40];
    if (len == 0 || len >= sizeof(buf)) return BR_CSS_PROP_UNKNOWN;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[len] = '\0';
    for (int i = 0; kProps[i].name != NULL; i++) {
        if (strcmp(kProps[i].name, buf) == 0) return kProps[i].prop;
    }
    return BR_CSS_PROP_UNKNOWN;
}

int br_css_prop_inherits(br_css_prop_t p) {
    for (int i = 0; kProps[i].name != NULL; i++) {
        if (kProps[i].prop == p) return kProps[i].inherits;
    }
    return 0;
}
