/*
 * CSS value parsing.
 *
 * Handles:
 *   - keywords (red, italic, bold, none, ...)
 *   - hex colours (#rgb, #rrggbb, #rrggbbaa)
 *   - rgb()/rgba()/hsl()/hsla() — limited
 *   - lengths (12px, 1em, 80%)
 *   - bare numbers
 *   - strings ("Helvetica")
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ----- named colours ----- */

typedef struct {
    const char *name;
    uint32_t color;
} br_named_color_t;

/* CSS basic + extended named colours. ARGB (alpha 0xFF). */
static const br_named_color_t kNamedColors[] = {
    {"aliceblue", 0xFFF0F8FF}, {"antiquewhite", 0xFFFAEBD7},
    {"aqua", 0xFF00FFFF}, {"aquamarine", 0xFF7FFFD4},
    {"azure", 0xFFF0FFFF}, {"beige", 0xFFF5F5DC},
    {"bisque", 0xFFFFE4C4}, {"black", 0xFF000000},
    {"blanchedalmond", 0xFFFFEBCD}, {"blue", 0xFF0000FF},
    {"blueviolet", 0xFF8A2BE2}, {"brown", 0xFFA52A2A},
    {"burlywood", 0xFFDEB887}, {"cadetblue", 0xFF5F9EA0},
    {"chartreuse", 0xFF7FFF00}, {"chocolate", 0xFFD2691E},
    {"coral", 0xFFFF7F50}, {"cornflowerblue", 0xFF6495ED},
    {"cornsilk", 0xFFFFF8DC}, {"crimson", 0xFFDC143C},
    {"cyan", 0xFF00FFFF}, {"darkblue", 0xFF00008B},
    {"darkcyan", 0xFF008B8B}, {"darkgoldenrod", 0xFFB8860B},
    {"darkgray", 0xFFA9A9A9}, {"darkgrey", 0xFFA9A9A9},
    {"darkgreen", 0xFF006400}, {"darkkhaki", 0xFFBDB76B},
    {"darkmagenta", 0xFF8B008B}, {"darkolivegreen", 0xFF556B2F},
    {"darkorange", 0xFFFF8C00}, {"darkorchid", 0xFF9932CC},
    {"darkred", 0xFF8B0000}, {"darksalmon", 0xFFE9967A},
    {"darkseagreen", 0xFF8FBC8F}, {"darkslateblue", 0xFF483D8B},
    {"darkslategray", 0xFF2F4F4F}, {"darkslategrey", 0xFF2F4F4F},
    {"darkturquoise", 0xFF00CED1}, {"darkviolet", 0xFF9400D3},
    {"deeppink", 0xFFFF1493}, {"deepskyblue", 0xFF00BFFF},
    {"dimgray", 0xFF696969}, {"dimgrey", 0xFF696969},
    {"dodgerblue", 0xFF1E90FF}, {"firebrick", 0xFFB22222},
    {"floralwhite", 0xFFFFFAF0}, {"forestgreen", 0xFF228B22},
    {"fuchsia", 0xFFFF00FF}, {"gainsboro", 0xFFDCDCDC},
    {"ghostwhite", 0xFFF8F8FF}, {"gold", 0xFFFFD700},
    {"goldenrod", 0xFFDAA520}, {"gray", 0xFF808080},
    {"grey", 0xFF808080}, {"green", 0xFF008000},
    {"greenyellow", 0xFFADFF2F}, {"honeydew", 0xFFF0FFF0},
    {"hotpink", 0xFFFF69B4}, {"indianred", 0xFFCD5C5C},
    {"indigo", 0xFF4B0082}, {"ivory", 0xFFFFFFF0},
    {"khaki", 0xFFF0E68C}, {"lavender", 0xFFE6E6FA},
    {"lavenderblush", 0xFFFFF0F5}, {"lawngreen", 0xFF7CFC00},
    {"lemonchiffon", 0xFFFFFACD}, {"lightblue", 0xFFADD8E6},
    {"lightcoral", 0xFFF08080}, {"lightcyan", 0xFFE0FFFF},
    {"lightgoldenrodyellow", 0xFFFAFAD2}, {"lightgray", 0xFFD3D3D3},
    {"lightgrey", 0xFFD3D3D3}, {"lightgreen", 0xFF90EE90},
    {"lightpink", 0xFFFFB6C1}, {"lightsalmon", 0xFFFFA07A},
    {"lightseagreen", 0xFF20B2AA}, {"lightskyblue", 0xFF87CEFA},
    {"lightslategray", 0xFF778899}, {"lightslategrey", 0xFF778899},
    {"lightsteelblue", 0xFFB0C4DE}, {"lightyellow", 0xFFFFFFE0},
    {"lime", 0xFF00FF00}, {"limegreen", 0xFF32CD32},
    {"linen", 0xFFFAF0E6}, {"magenta", 0xFFFF00FF},
    {"maroon", 0xFF800000}, {"mediumaquamarine", 0xFF66CDAA},
    {"mediumblue", 0xFF0000CD}, {"mediumorchid", 0xFFBA55D3},
    {"mediumpurple", 0xFF9370DB}, {"mediumseagreen", 0xFF3CB371},
    {"mediumslateblue", 0xFF7B68EE}, {"mediumspringgreen", 0xFF00FA9A},
    {"mediumturquoise", 0xFF48D1CC}, {"mediumvioletred", 0xFFC71585},
    {"midnightblue", 0xFF191970}, {"mintcream", 0xFFF5FFFA},
    {"mistyrose", 0xFFFFE4E1}, {"moccasin", 0xFFFFE4B5},
    {"navajowhite", 0xFFFFDEAD}, {"navy", 0xFF000080},
    {"oldlace", 0xFFFDF5E6}, {"olive", 0xFF808000},
    {"olivedrab", 0xFF6B8E23}, {"orange", 0xFFFFA500},
    {"orangered", 0xFFFF4500}, {"orchid", 0xFFDA70D6},
    {"palegoldenrod", 0xFFEEE8AA}, {"palegreen", 0xFF98FB98},
    {"paleturquoise", 0xFFAFEEEE}, {"palevioletred", 0xFFDB7093},
    {"papayawhip", 0xFFFFEFD5}, {"peachpuff", 0xFFFFDAB9},
    {"peru", 0xFFCD853F}, {"pink", 0xFFFFC0CB},
    {"plum", 0xFFDDA0DD}, {"powderblue", 0xFFB0E0E6},
    {"purple", 0xFF800080}, {"rebeccapurple", 0xFF663399},
    {"red", 0xFFFF0000}, {"rosybrown", 0xFFBC8F8F},
    {"royalblue", 0xFF4169E1}, {"saddlebrown", 0xFF8B4513},
    {"salmon", 0xFFFA8072}, {"sandybrown", 0xFFF4A460},
    {"seagreen", 0xFF2E8B57}, {"seashell", 0xFFFFF5EE},
    {"sienna", 0xFFA0522D}, {"silver", 0xFFC0C0C0},
    {"skyblue", 0xFF87CEEB}, {"slateblue", 0xFF6A5ACD},
    {"slategray", 0xFF708090}, {"slategrey", 0xFF708090},
    {"snow", 0xFFFFFAFA}, {"springgreen", 0xFF00FF7F},
    {"steelblue", 0xFF4682B4}, {"tan", 0xFFD2B48C},
    {"teal", 0xFF008080}, {"thistle", 0xFFD8BFD8},
    {"tomato", 0xFFFF6347}, {"turquoise", 0xFF40E0D0},
    {"violet", 0xFFEE82EE}, {"wheat", 0xFFF5DEB3},
    {"white", 0xFFFFFFFF}, {"whitesmoke", 0xFFF5F5F5},
    {"yellow", 0xFFFFFF00}, {"yellowgreen", 0xFF9ACD32},
    /* transparent has alpha 0 — special: cascade treats it as "no paint". */
    {"transparent", 0x00000000},
    {NULL, 0},
};

int br_css_named_color(const char *name, size_t len, uint32_t *out_color) {
    char buf[40];
    if (len >= sizeof(buf)) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        buf[i] = c;
    }
    buf[len] = '\0';
    for (int i = 0; kNamedColors[i].name != NULL; i++) {
        if (strcmp(kNamedColors[i].name, buf) == 0) {
            *out_color = kNamedColors[i].color;
            return 1;
        }
    }
    return 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_color(const char *s, size_t len, uint32_t *out) {
    if (len == 3 || len == 4) {
        int v[4] = {0, 0, 0, 0xF};
        for (size_t i = 0; i < len; i++) {
            int d = hex_digit(s[i]);
            if (d < 0) return -1;
            v[i] = d;
        }
        uint32_t r = (uint32_t)(v[0] * 17);
        uint32_t g = (uint32_t)(v[1] * 17);
        uint32_t b = (uint32_t)(v[2] * 17);
        uint32_t a = (uint32_t)(v[3] * 17);
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        return 0;
    }
    if (len == 6 || len == 8) {
        int v[8];
        for (size_t i = 0; i < len; i++) {
            int d = hex_digit(s[i]);
            if (d < 0) return -1;
            v[i] = d;
        }
        uint32_t r = (uint32_t)(v[0] * 16 + v[1]);
        uint32_t g = (uint32_t)(v[2] * 16 + v[3]);
        uint32_t b = (uint32_t)(v[4] * 16 + v[5]);
        uint32_t a = len == 8 ? (uint32_t)(v[6] * 16 + v[7]) : 0xFF;
        *out = (a << 24) | (r << 16) | (g << 8) | b;
        return 0;
    }
    return -1;
}

/* Parse "rgb(255, 0, 0)" / "rgba(...)" / "hsl(...)" given the argument
 * span between '(' and ')'. */
static int parse_func_color(const char *name, size_t name_len,
                            const char *args, size_t args_len,
                            uint32_t *out) {
    /* Tokenise the args; expect 3 or 4 numbers separated by commas
     * (or whitespace, modern syntax). */
    double comps[4] = {0, 0, 0, 1.0};
    int n = 0;
    const char *p = args;
    const char *end = args + args_len;
    while (p < end && n < 4) {
        while (p < end && (isspace((unsigned char)*p) || *p == ',' || *p == '/')) p++;
        if (p >= end) break;
        const char *start = p;
        while (p < end && !isspace((unsigned char)*p) && *p != ',' && *p != '/' &&
               *p != ')') p++;
        size_t slen = (size_t)(p - start);
        if (slen == 0) break;
        char numbuf[32];
        size_t take = slen < sizeof(numbuf) - 1 ? slen : sizeof(numbuf) - 1;
        memcpy(numbuf, start, take);
        numbuf[take] = '\0';
        int is_percent = (take > 0 && numbuf[take - 1] == '%');
        if (is_percent) numbuf[take - 1] = '\0';
        char *endp = NULL;
        double v = strtod(numbuf, &endp);
        if (endp == numbuf) return -1;
        if (is_percent) v = v / 100.0;
        comps[n++] = v;
    }
    if (n < 3) return -1;

    int is_hsl = (name_len == 3 && (strncasecmp(name, "hsl", 3) == 0)) ||
                 (name_len == 4 && (strncasecmp(name, "hsla", 4) == 0));

    double r, g, b;
    if (is_hsl) {
        double h = comps[0];
        double s = comps[1];
        double l = comps[2];
        /* If the first arg was passed without % the typical interpretation
         * is degrees (0..360) for H, raw 0..1 for S/L when written as
         * fractions. We accept both: if s>1 or l>1, treat as percent
         * already divided. */
        if (s > 1.5) s = s / 100.0;
        if (l > 1.5) l = l / 100.0;
        h = fmod(h, 360.0);
        if (h < 0) h += 360.0;
        double c = (1 - fabs(2 * l - 1)) * s;
        double hp = h / 60.0;
        double x = c * (1 - fabs(fmod(hp, 2.0) - 1));
        double rp = 0, gp = 0, bp = 0;
        if (hp < 1) { rp = c; gp = x; bp = 0; }
        else if (hp < 2) { rp = x; gp = c; bp = 0; }
        else if (hp < 3) { rp = 0; gp = c; bp = x; }
        else if (hp < 4) { rp = 0; gp = x; bp = c; }
        else if (hp < 5) { rp = x; gp = 0; bp = c; }
        else            { rp = c; gp = 0; bp = x; }
        double m = l - c / 2.0;
        r = (rp + m) * 255.0;
        g = (gp + m) * 255.0;
        b = (bp + m) * 255.0;
    } else {
        r = comps[0];
        g = comps[1];
        b = comps[2];
    }
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    double a = comps[3];
    if (a < 0) a = 0; if (a > 1.0001) a = a / 255.0;  /* accept 0..255 alpha */
    if (a < 0) a = 0; if (a > 1) a = 1;
    uint32_t R = (uint32_t)(r + 0.5);
    uint32_t G = (uint32_t)(g + 0.5);
    uint32_t B = (uint32_t)(b + 0.5);
    uint32_t A = (uint32_t)(a * 255.0 + 0.5);
    *out = (A << 24) | (R << 16) | (G << 8) | B;
    return 0;
}

static void lower_copy(const char *src, size_t len, char *dst, size_t cap) {
    if (cap == 0) return;
    if (len >= cap) len = cap - 1;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    dst[len] = '\0';
}

static br_css_unit_t unit_from_text(const char *u, size_t len) {
    if (len == 0) return BR_CSS_UNIT_NONE;
    if (len == 1 && u[0] == '%') return BR_CSS_UNIT_PCT;
    if (len == 2) {
        if (strncasecmp(u, "px", 2) == 0) return BR_CSS_UNIT_PX;
        if (strncasecmp(u, "em", 2) == 0) return BR_CSS_UNIT_EM;
        if (strncasecmp(u, "pt", 2) == 0) return BR_CSS_UNIT_PT;
    }
    if (len == 3 && strncasecmp(u, "rem", 3) == 0) return BR_CSS_UNIT_REM;
    return BR_CSS_UNIT_NONE;
}

int br_css_value_parse(const char *src, size_t len, br_css_value_t *out) {
    memset(out, 0, sizeof(*out));
    /* Trim surrounding whitespace. */
    while (len > 0 && (src[0] == ' ' || src[0] == '\t' || src[0] == '\n' ||
                       src[0] == '\r')) { src++; len--; }
    while (len > 0 && (src[len-1] == ' ' || src[len-1] == '\t' ||
                       src[len-1] == '\n' || src[len-1] == '\r')) len--;
    if (len == 0) return -1;

    /* String literal. */
    if (src[0] == '"' || src[0] == '\'') {
        char q = src[0];
        if (len >= 2 && src[len-1] == q) {
            size_t inner_len = len - 2;
            if (inner_len >= sizeof(out->keyword))
                inner_len = sizeof(out->keyword) - 1;
            memcpy(out->keyword, src + 1, inner_len);
            out->keyword[inner_len] = '\0';
            out->kind = BR_CSSV_STRING;
            return 0;
        }
    }

    /* Hex colour. */
    if (src[0] == '#') {
        if (parse_hex_color(src + 1, len - 1, &out->color) == 0) {
            out->kind = BR_CSSV_COLOR;
            return 0;
        }
        return -1;
    }

    /* Functional notation: name(args). */
    const char *lparen = NULL;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '(') { lparen = src + i; break; }
    }
    if (lparen != NULL && src[len - 1] == ')') {
        size_t name_len = (size_t)(lparen - src);
        const char *args = lparen + 1;
        size_t args_len = (size_t)((src + len - 1) - args);
        char name_lower[16];
        lower_copy(src, name_len, name_lower, sizeof(name_lower));
        if (strcmp(name_lower, "rgb") == 0 || strcmp(name_lower, "rgba") == 0 ||
            strcmp(name_lower, "hsl") == 0 || strcmp(name_lower, "hsla") == 0) {
            if (parse_func_color(src, name_len, args, args_len, &out->color) == 0) {
                out->kind = BR_CSSV_COLOR;
                return 0;
            }
        }
        /* unknown function — record as keyword for visibility */
        lower_copy(src, len < sizeof(out->keyword) ? len :
                   sizeof(out->keyword) - 1, out->keyword, sizeof(out->keyword));
        out->kind = BR_CSSV_KEYWORD;
        return 0;
    }

    /* Number / dimension. */
    char first = src[0];
    if (first == '-' || first == '+' || first == '.' ||
        (first >= '0' && first <= '9')) {
        char buf[64];
        size_t take = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, src, take);
        buf[take] = '\0';
        char *endp = NULL;
        double v = strtod(buf, &endp);
        if (endp != buf) {
            out->num = v;
            const char *unit_start = src + (size_t)(endp - buf);
            size_t unit_len = (size_t)(src + len - unit_start);
            while (unit_len > 0 && (*unit_start == ' ' || *unit_start == '\t')) {
                unit_start++; unit_len--;
            }
            if (unit_len == 0) {
                out->kind = BR_CSSV_NUMBER;
                return 0;
            }
            out->unit = unit_from_text(unit_start, unit_len);
            out->kind = (out->unit != BR_CSS_UNIT_NONE) ? BR_CSSV_LENGTH
                                                       : BR_CSSV_NUMBER;
            return 0;
        }
    }

    /* Named colour? */
    uint32_t named = 0;
    if (br_css_named_color(src, len, &named)) {
        out->color = named;
        out->kind = BR_CSSV_COLOR;
        return 0;
    }

    /* Keyword. */
    lower_copy(src, len, out->keyword, sizeof(out->keyword));
    out->kind = BR_CSSV_KEYWORD;
    return 0;
}
