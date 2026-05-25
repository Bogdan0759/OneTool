/*
 * Tiny HTML tokenizer.
 *
 * Walks the input bytewise, splitting it into a stream of inline runs
 * separated by BREAK / PARAGRAPH markers. The model is intentionally
 * simple — no DOM tree, no CSS, no script. Block tags collapse into
 * paragraph breaks; inline tags push/pop a small style stack; <a href>
 * registers a link and tags the runs between <a> and </a>.
 *
 * &entities are decoded for a small common set. Anything else we leave
 * literal (or for "&xxx;" forms we just drop the leading '&').
 */
#include "html.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BR_TEXT_FLUSH_CAP   (32 * 1024)
#define BR_MAX_STYLE_STACK  16

typedef struct {
    const char *p;
    const char *end;
    br_doc_t   *doc;
    /* Style stack tracks nested inline tags. */
    br_style_t style_stack[BR_MAX_STYLE_STACK];
    int        style_depth;
    int        link_stack[BR_MAX_STYLE_STACK]; /* index into doc->links, or -1 */
    int        link_depth;
    /* Block-level flags. */
    int        in_pre;
    int        in_script;
    int        in_style;
    int        in_title;
    int        title_off;
    int        in_head;
    int        in_textarea;
    /* When set, we drop all bytes/tags until we see `</skip_tag>`. Used for
     * <svg>, <math>, <select>, <option>, where the contents would just be
     * noise in a text browser. */
    char       skip_tag[16];
    int        skip_depth;
    int        list_depth;        /* nested <ul>/<ol> depth, for indent */
    int        ol_counter[BR_MAX_STYLE_STACK]; /* per-list <ol> counter (-1 if <ul>) */
    /* Buffer for current text accumulator. */
    char      *buf;
    size_t     buf_len;
    size_t     buf_cap;
    /* Sticky pending block separation. We coalesce successive block
       transitions so multiple <br>/</p> in a row don't blow up vertical
       gaps. */
    int        pending_break;     /* 1 == break, 2 == paragraph */
} br_parser_t;

/* ----- doc allocation ----- */

br_doc_t *br_doc_create(void) {
    br_doc_t *d = (br_doc_t *)calloc(1, sizeof(br_doc_t));
    return d;
}

void br_doc_clear(br_doc_t *doc) {
    if (doc == NULL) return;
    for (size_t i = 0; i < doc->run_count; i++) {
        free(doc->runs[i].text);
    }
    free(doc->runs);
    doc->runs = NULL;
    doc->run_count = 0;
    doc->run_cap = 0;
    for (size_t i = 0; i < doc->link_count; i++) {
        free(doc->links[i].href);
    }
    free(doc->links);
    doc->links = NULL;
    doc->link_count = 0;
    doc->link_cap = 0;
    doc->title[0] = '\0';
}

void br_doc_destroy(br_doc_t *doc) {
    if (doc == NULL) return;
    br_doc_clear(doc);
    free(doc);
}

static int doc_grow_runs(br_doc_t *d) {
    size_t want = d->run_cap == 0 ? 64 : d->run_cap * 2;
    br_run_t *p = (br_run_t *)realloc(d->runs, want * sizeof(br_run_t));
    if (p == NULL) return -1;
    d->runs = p;
    d->run_cap = want;
    return 0;
}

static int doc_grow_links(br_doc_t *d) {
    size_t want = d->link_cap == 0 ? 16 : d->link_cap * 2;
    br_link_t *p = (br_link_t *)realloc(d->links, want * sizeof(br_link_t));
    if (p == NULL) return -1;
    d->links = p;
    d->link_cap = want;
    return 0;
}

static int doc_emit_text(br_doc_t *d, const char *text, br_style_t style,
                         int link_index) {
    if (text == NULL || text[0] == '\0') return 0;
    if (d->run_count == d->run_cap && doc_grow_runs(d) != 0) return -1;
    br_run_t *r = &d->runs[d->run_count++];
    r->kind = BR_RUN_TEXT;
    r->style = style;
    r->text = strdup(text);
    r->link_index = link_index;
    return r->text == NULL ? -1 : 0;
}

static int doc_emit_marker(br_doc_t *d, br_run_kind_t kind) {
    if (d->run_count == d->run_cap && doc_grow_runs(d) != 0) return -1;
    br_run_t *r = &d->runs[d->run_count++];
    r->kind = kind;
    r->style = BR_STYLE_NORMAL;
    r->text = NULL;
    r->link_index = -1;
    return 0;
}

static int doc_add_link(br_doc_t *d, const char *href, size_t href_len) {
    if (d->link_count == d->link_cap && doc_grow_links(d) != 0) return -1;
    br_link_t *l = &d->links[d->link_count];
    l->href = (char *)malloc(href_len + 1);
    if (l->href == NULL) return -1;
    memcpy(l->href, href, href_len);
    l->href[href_len] = '\0';
    l->first_run = (int)d->run_count;
    return (int)d->link_count++;
}

/* ----- parser helpers ----- */

static br_style_t cur_style(const br_parser_t *p) {
    if (p->link_depth > 0) return BR_STYLE_LINK;
    if (p->style_depth > 0) return p->style_stack[p->style_depth - 1];
    if (p->in_pre)  return BR_STYLE_PRE;
    return BR_STYLE_NORMAL;
}

static int cur_link(const br_parser_t *p) {
    return p->link_depth > 0 ? p->link_stack[p->link_depth - 1] : -1;
}

static int buf_reserve(br_parser_t *p, size_t extra) {
    if (p->buf_len + extra + 1 <= p->buf_cap) return 0;
    size_t want = p->buf_cap == 0 ? 256 : p->buf_cap;
    while (want < p->buf_len + extra + 1) want *= 2;
    char *q = (char *)realloc(p->buf, want);
    if (q == NULL) return -1;
    p->buf = q;
    p->buf_cap = want;
    return 0;
}

static int buf_putc(br_parser_t *p, char c) {
    if (buf_reserve(p, 1) != 0) return -1;
    p->buf[p->buf_len++] = c;
    return 0;
}

static int flush_text(br_parser_t *p) {
    if (p->buf_len == 0) return 0;
    p->buf[p->buf_len] = '\0';
    int rc = doc_emit_text(p->doc, p->buf, cur_style(p), cur_link(p));
    p->buf_len = 0;
    return rc;
}

static void apply_pending_break(br_parser_t *p) {
    if (p->pending_break == 0) return;
    flush_text(p);
    if (p->pending_break >= 2)
        doc_emit_marker(p->doc, BR_RUN_PARAGRAPH);
    else
        doc_emit_marker(p->doc, BR_RUN_BREAK);
    p->pending_break = 0;
}

static void request_break(br_parser_t *p, int paragraph) {
    /* Don't emit leading breaks before any content. */
    if (p->doc->run_count == 0 && p->buf_len == 0) return;
    if (paragraph) {
        if (p->pending_break < 2) p->pending_break = 2;
    } else {
        if (p->pending_break < 1) p->pending_break = 1;
    }
}

/* Entity decoding. Common set of named entities; the parser also handles
 * numeric &#NN; and &#xNN; entities separately. */
static int decode_entity(const char *name, size_t name_len) {
    struct { const char *n; int cp; } table[] = {
        /* core */
        {"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '},
        /* punctuation / typography */
        {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122},
        {"laquo", 0xAB}, {"raquo", 0xBB}, {"ndash", 0x2013}, {"mdash", 0x2014},
        {"hellip", 0x2026}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022},
        {"middot", 0xB7}, {"deg", 0xB0}, {"sect", 0xA7}, {"para", 0xB6},
        {"dagger", 0x2020}, {"Dagger", 0x2021}, {"permil", 0x2030},
        {"prime", 0x2032}, {"Prime", 0x2033}, {"lsaquo", 0x2039}, {"rsaquo", 0x203A},
        {"sbquo", 0x201A}, {"bdquo", 0x201E}, {"oline", 0x203E},
        {"ensp", 0x2002}, {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C},
        {"zwj", 0x200D}, {"shy", 0xAD},
        /* math / symbols */
        {"plusmn", 0xB1}, {"times", 0xD7}, {"divide", 0xF7},
        {"micro", 0xB5}, {"sup2", 0xB2}, {"sup3", 0xB3}, {"sup1", 0xB9},
        {"frac14", 0xBC}, {"frac12", 0xBD}, {"frac34", 0xBE},
        {"larr", 0x2190}, {"uarr", 0x2191}, {"rarr", 0x2192}, {"darr", 0x2193},
        {"harr", 0x2194}, {"crarr", 0x21B5},
        {"lArr", 0x21D0}, {"uArr", 0x21D1}, {"rArr", 0x21D2}, {"dArr", 0x21D3},
        {"hArr", 0x21D4},
        {"forall", 0x2200}, {"exist", 0x2203}, {"empty", 0x2205},
        {"isin", 0x2208}, {"notin", 0x2209}, {"ni", 0x220B}, {"sum", 0x2211},
        {"prod", 0x220F}, {"radic", 0x221A}, {"infin", 0x221E},
        {"and", 0x2227}, {"or", 0x2228}, {"cap", 0x2229}, {"cup", 0x222A},
        {"int", 0x222B}, {"asymp", 0x2248}, {"ne", 0x2260}, {"equiv", 0x2261},
        {"le", 0x2264}, {"ge", 0x2265},
        {"sub", 0x2282}, {"sup", 0x2283}, {"nsub", 0x2284},
        {"sube", 0x2286}, {"supe", 0x2287}, {"oplus", 0x2295}, {"otimes", 0x2297},
        /* currency */
        {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
        {"curren", 0xA4},
        /* latin extended (selected) */
        {"agrave", 0xE0}, {"Agrave", 0xC0}, {"aacute", 0xE1}, {"Aacute", 0xC1},
        {"acirc", 0xE2}, {"Acirc", 0xC2}, {"atilde", 0xE3}, {"Atilde", 0xC3},
        {"auml", 0xE4}, {"Auml", 0xC4}, {"aring", 0xE5}, {"Aring", 0xC5},
        {"aelig", 0xE6}, {"AElig", 0xC6}, {"ccedil", 0xE7}, {"Ccedil", 0xC7},
        {"egrave", 0xE8}, {"Egrave", 0xC8}, {"eacute", 0xE9}, {"Eacute", 0xC9},
        {"ecirc", 0xEA}, {"Ecirc", 0xCA}, {"euml", 0xEB}, {"Euml", 0xCB},
        {"igrave", 0xEC}, {"Igrave", 0xCC}, {"iacute", 0xED}, {"Iacute", 0xCD},
        {"icirc", 0xEE}, {"Icirc", 0xCE}, {"iuml", 0xEF}, {"Iuml", 0xCF},
        {"ntilde", 0xF1}, {"Ntilde", 0xD1},
        {"ograve", 0xF2}, {"Ograve", 0xD2}, {"oacute", 0xF3}, {"Oacute", 0xD3},
        {"ocirc", 0xF4}, {"Ocirc", 0xD4}, {"otilde", 0xF5}, {"Otilde", 0xD5},
        {"ouml", 0xF6}, {"Ouml", 0xD6}, {"oslash", 0xF8}, {"Oslash", 0xD8},
        {"ugrave", 0xF9}, {"Ugrave", 0xD9}, {"uacute", 0xFA}, {"Uacute", 0xDA},
        {"ucirc", 0xFB}, {"Ucirc", 0xDB}, {"uuml", 0xFC}, {"Uuml", 0xDC},
        {"yacute", 0xFD}, {"Yacute", 0xDD}, {"yuml", 0xFF}, {"szlig", 0xDF},
        {"iexcl", 0xA1}, {"iquest", 0xBF},
        /* greek (selected) */
        {"alpha", 0x3B1}, {"beta", 0x3B2}, {"gamma", 0x3B3}, {"delta", 0x3B4},
        {"epsilon", 0x3B5}, {"zeta", 0x3B6}, {"eta", 0x3B7}, {"theta", 0x3B8},
        {"iota", 0x3B9}, {"kappa", 0x3BA}, {"lambda", 0x3BB}, {"mu", 0x3BC},
        {"nu", 0x3BD}, {"xi", 0x3BE}, {"omicron", 0x3BF}, {"pi", 0x3C0},
        {"rho", 0x3C1}, {"sigma", 0x3C3}, {"tau", 0x3C4}, {"upsilon", 0x3C5},
        {"phi", 0x3C6}, {"chi", 0x3C7}, {"psi", 0x3C8}, {"omega", 0x3C9},
        {"Alpha", 0x391}, {"Beta", 0x392}, {"Gamma", 0x393}, {"Delta", 0x394},
        {"Sigma", 0x3A3}, {"Omega", 0x3A9},
        {NULL, 0}
    };
    for (int i = 0; table[i].n != NULL; i++) {
        size_t ln = strlen(table[i].n);
        if (ln == name_len && strncmp(table[i].n, name, name_len) == 0)
            return table[i].cp;
    }
    /* Case-insensitive fallback (HTML entities are technically case-sensitive,
     * but many real-world docs lowercase Alphas etc.) */
    for (int i = 0; table[i].n != NULL; i++) {
        size_t ln = strlen(table[i].n);
        if (ln == name_len && strncasecmp(table[i].n, name, name_len) == 0)
            return table[i].cp;
    }
    return -1;
}

static int emit_codepoint(br_parser_t *p, int cp) {
    if (cp < 0) cp = '?';
    if (cp < 0x80) {
        return buf_putc(p, (char)cp);
    }
    if (cp < 0x800) {
        if (buf_putc(p, (char)(0xC0 | (cp >> 6))) != 0) return -1;
        return buf_putc(p, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp < 0x10000) {
        if (buf_putc(p, (char)(0xE0 | (cp >> 12))) != 0) return -1;
        if (buf_putc(p, (char)(0x80 | ((cp >> 6) & 0x3F))) != 0) return -1;
        return buf_putc(p, (char)(0x80 | (cp & 0x3F)));
    }
    if (buf_putc(p, (char)(0xF0 | (cp >> 18))) != 0) return -1;
    if (buf_putc(p, (char)(0x80 | ((cp >> 12) & 0x3F))) != 0) return -1;
    if (buf_putc(p, (char)(0x80 | ((cp >> 6) & 0x3F))) != 0) return -1;
    return buf_putc(p, (char)(0x80 | (cp & 0x3F)));
}

/* Try to consume an HTML entity starting at p->p. Returns 1 on consumed,
 * 0 if not an entity (caller should treat '&' literally), -1 on alloc fail. */
static int try_entity(br_parser_t *p) {
    const char *s = p->p;
    if (s >= p->end || *s != '&') return 0;
    const char *q = s + 1;
    int cp = -1;
    if (q < p->end && *q == '#') {
        q++;
        int base = 10;
        if (q < p->end && (*q == 'x' || *q == 'X')) { base = 16; q++; }
        const char *start = q;
        long v = 0;
        while (q < p->end && ((base == 10 && isdigit((unsigned char)*q)) ||
                              (base == 16 && isxdigit((unsigned char)*q)))) {
            int d = *q;
            d = isdigit(d) ? d - '0' : (d >= 'a' ? d - 'a' + 10 : d - 'A' + 10);
            v = v * base + d;
            q++;
        }
        if (q == start) return 0;
        cp = (int)v;
        if (q < p->end && *q == ';') q++;
    } else {
        const char *start = q;
        while (q < p->end && isalpha((unsigned char)*q)) q++;
        if (q == start) return 0;
        cp = decode_entity(start, (size_t)(q - start));
        if (cp < 0) return 0;
        if (q < p->end && *q == ';') q++;
    }
    if (emit_codepoint(p, cp) != 0) return -1;
    p->p = q;
    return 1;
}

/* ----- tag parsing ----- */

typedef struct {
    char  name[32];
    int   closing;
    int   self_closing;
    /* Attribute "href" value, if present. */
    char  href[2048];
    int   has_href;
    /* Attribute "alt" value (used for <img>). */
    char  alt[512];
    int   has_alt;
    /* Attribute "title" value (fallback for <img>). */
    char  title[512];
    int   has_title;
    /* Attribute "src" value (last-resort fallback for <img>). */
    char  src[2048];
    int   has_src;
} br_tag_t;

static void lower_inplace(char *s) {
    for (; *s != '\0'; s++) *s = (char)tolower((unsigned char)*s);
}

/* Parse a tag starting at p->p (which points at '<'). On success p->p is
 * advanced past '>'. Returns 1 if tag parsed, 0 if not a tag. */
static int parse_tag(br_parser_t *p, br_tag_t *out) {
    const char *s = p->p;
    if (s >= p->end || *s != '<') return 0;
    const char *q = s + 1;

    /* Comments / DOCTYPE / CDATA - skip until matching '>'. */
    if (q < p->end && *q == '!') {
        /* <!-- comment --> */
        if (q + 2 < p->end && q[1] == '-' && q[2] == '-') {
            const char *end = q + 3;
            while (end + 2 < p->end &&
                   !(end[0] == '-' && end[1] == '-' && end[2] == '>')) end++;
            p->p = (end + 2 < p->end) ? end + 3 : p->end;
            return 2;
        }
        /* <!DOCTYPE ...> */
        while (q < p->end && *q != '>') q++;
        if (q < p->end) q++;
        p->p = q;
        return 2;
    }
    if (q < p->end && *q == '?') {
        while (q < p->end && *q != '>') q++;
        if (q < p->end) q++;
        p->p = q;
        return 2;
    }

    memset(out, 0, sizeof(*out));
    if (q < p->end && *q == '/') { out->closing = 1; q++; }
    /* Read tag name. */
    size_t ni = 0;
    while (q < p->end && (isalnum((unsigned char)*q) || *q == '-') &&
           ni + 1 < sizeof(out->name)) {
        out->name[ni++] = *q++;
    }
    out->name[ni] = '\0';
    if (ni == 0) return 0;
    lower_inplace(out->name);

    /* Read attributes until '>'. */
    while (q < p->end && *q != '>') {
        while (q < p->end && isspace((unsigned char)*q)) q++;
        if (q < p->end && *q == '/') { out->self_closing = 1; q++; continue; }
        if (q >= p->end || *q == '>') break;
        /* attr name */
        const char *an = q;
        while (q < p->end && *q != '=' && *q != '>' &&
               !isspace((unsigned char)*q) && *q != '/') q++;
        size_t alen = (size_t)(q - an);
        int is_href = (alen == 4) && (strncasecmp(an, "href", 4) == 0);
        int is_alt = (alen == 3) && (strncasecmp(an, "alt", 3) == 0);
        int is_title = (alen == 5) && (strncasecmp(an, "title", 5) == 0);
        int is_src = (alen == 3) && (strncasecmp(an, "src", 3) == 0);
        /* attr value */
        if (q < p->end && *q == '=') {
            q++;
            char quote = 0;
            if (q < p->end && (*q == '"' || *q == '\'')) { quote = *q; q++; }
            const char *vs = q;
            if (quote != 0) {
                while (q < p->end && *q != quote) q++;
            } else {
                while (q < p->end && !isspace((unsigned char)*q) && *q != '>') q++;
            }
            size_t vlen = (size_t)(q - vs);
            if (is_href && !out->has_href) {
                size_t take = vlen;
                if (take >= sizeof(out->href)) take = sizeof(out->href) - 1;
                memcpy(out->href, vs, take);
                out->href[take] = '\0';
                out->has_href = 1;
            } else if (is_alt && !out->has_alt) {
                size_t take = vlen;
                if (take >= sizeof(out->alt)) take = sizeof(out->alt) - 1;
                memcpy(out->alt, vs, take);
                out->alt[take] = '\0';
                out->has_alt = 1;
            } else if (is_title && !out->has_title) {
                size_t take = vlen;
                if (take >= sizeof(out->title)) take = sizeof(out->title) - 1;
                memcpy(out->title, vs, take);
                out->title[take] = '\0';
                out->has_title = 1;
            } else if (is_src && !out->has_src) {
                size_t take = vlen;
                if (take >= sizeof(out->src)) take = sizeof(out->src) - 1;
                memcpy(out->src, vs, take);
                out->src[take] = '\0';
                out->has_src = 1;
            }
            if (quote != 0 && q < p->end) q++;
        }
    }
    if (q < p->end && *q == '>') q++;
    p->p = q;
    return 1;
}

/* ----- block / inline handling ----- */

static int is_block_tag(const char *name) {
    static const char *blocks[] = {
        "p", "div", "section", "article", "header", "footer", "main", "aside",
        "nav", "ul", "ol", "li", "table", "tr", "form", "blockquote",
        "h1", "h2", "h3", "h4", "h5", "h6", "pre", "hr", "tbody", "thead",
        "tfoot", "fieldset", "address", "figure", "figcaption", "dl", "dt", "dd",
        "details", "summary", "menu", "menuitem", "video", "audio", "canvas",
        "noscript",
        NULL
    };
    for (int i = 0; blocks[i] != NULL; i++) {
        if (strcmp(blocks[i], name) == 0) return 1;
    }
    return 0;
}

static int is_void_tag(const char *name) {
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr", NULL
    };
    for (int i = 0; voids[i] != NULL; i++) {
        if (strcmp(voids[i], name) == 0) return 1;
    }
    return 0;
}

static int push_style(br_parser_t *p, br_style_t s) {
    if (p->style_depth >= BR_MAX_STYLE_STACK) return 0;
    p->style_stack[p->style_depth++] = s;
    return 0;
}

static void pop_style(br_parser_t *p) {
    if (p->style_depth > 0) p->style_depth--;
}

static void handle_open_tag(br_parser_t *p, const br_tag_t *t) {
    const char *n = t->name;

    /* Drop content inside skip-tags (svg, math, select, option). */
    if (strcmp(n, "svg") == 0 || strcmp(n, "math") == 0 ||
        strcmp(n, "select") == 0 || strcmp(n, "option") == 0 ||
        strcmp(n, "object") == 0 || strcmp(n, "iframe") == 0) {
        if (p->skip_tag[0] == '\0') {
            size_t nlen = strlen(n);
            if (nlen >= sizeof(p->skip_tag)) nlen = sizeof(p->skip_tag) - 1;
            memcpy(p->skip_tag, n, nlen);
            p->skip_tag[nlen] = '\0';
            p->skip_depth = 1;
            flush_text(p);
        } else if (strcmp(p->skip_tag, n) == 0) {
            p->skip_depth++;
        }
        return;
    }
    if (p->skip_tag[0] != '\0') return;

    /* In <head>, drop everything except <title>. */
    if (strcmp(n, "head") == 0) { p->in_head = 1; return; }
    if (strcmp(n, "body") == 0) { p->in_head = 0; return; }
    if (p->in_head && strcmp(n, "title") != 0) {
        if (strcmp(n, "script") == 0) { p->in_script = 1; return; }
        if (strcmp(n, "style") == 0) { p->in_style = 1; return; }
        return;
    }

    if (strcmp(n, "br") == 0) {
        request_break(p, 0);
        return;
    }
    if (strcmp(n, "hr") == 0) {
        flush_text(p);
        doc_emit_marker(p->doc, BR_RUN_RULE);
        request_break(p, 0);
        return;
    }
    if (strcmp(n, "wbr") == 0) {
        /* word break opportunity - just emit a space */
        buf_putc(p, ' ');
        return;
    }
    if (strcmp(n, "title") == 0) {
        p->in_title = 1;
        p->title_off = (int)p->doc->run_count;
        return;
    }
    if (strcmp(n, "script") == 0 || strcmp(n, "noscript") == 0) {
        /* drop noscript content too — usually contains "enable JS" notices */
        p->in_script = 1;
        return;
    }
    if (strcmp(n, "style") == 0) { p->in_style = 1; return; }
    if (strcmp(n, "textarea") == 0) {
        /* render textarea content as a code block */
        flush_text(p);
        push_style(p, BR_STYLE_CODE);
        p->in_textarea = 1;
        return;
    }
    if (strcmp(n, "pre") == 0) { p->in_pre = 1; request_break(p, 1); return; }
    if (strcmp(n, "h1") == 0) { request_break(p, 1); flush_text(p); push_style(p, BR_STYLE_H1); return; }
    if (strcmp(n, "h2") == 0) { request_break(p, 1); flush_text(p); push_style(p, BR_STYLE_H2); return; }
    if (strcmp(n, "h3") == 0 || strcmp(n, "h4") == 0 ||
        strcmp(n, "h5") == 0 || strcmp(n, "h6") == 0) {
        request_break(p, 1); flush_text(p); push_style(p, BR_STYLE_H3);
        return;
    }
    if (strcmp(n, "b") == 0 || strcmp(n, "strong") == 0) {
        flush_text(p); push_style(p, BR_STYLE_BOLD); return;
    }
    if (strcmp(n, "i") == 0 || strcmp(n, "em") == 0 ||
        strcmp(n, "cite") == 0 || strcmp(n, "var") == 0 ||
        strcmp(n, "dfn") == 0) {
        flush_text(p); push_style(p, BR_STYLE_ITALIC); return;
    }
    if (strcmp(n, "code") == 0 || strcmp(n, "tt") == 0 ||
        strcmp(n, "kbd") == 0 || strcmp(n, "samp") == 0) {
        flush_text(p); push_style(p, BR_STYLE_CODE); return;
    }
    if (strcmp(n, "ul") == 0 || strcmp(n, "ol") == 0) {
        request_break(p, 1);
        if (p->list_depth < BR_MAX_STYLE_STACK) {
            p->ol_counter[p->list_depth] = (strcmp(n, "ol") == 0) ? 1 : -1;
            p->list_depth++;
        }
        return;
    }
    if (strcmp(n, "li") == 0) {
        request_break(p, 0);
        flush_text(p);
        apply_pending_break(p);
        /* Indent by list depth. */
        for (int i = 1; i < p->list_depth; i++) {
            doc_emit_text(p->doc, "  ", BR_STYLE_NORMAL, -1);
        }
        if (p->list_depth > 0 && p->ol_counter[p->list_depth - 1] >= 0) {
            char buf[16];
            int n_num = snprintf(buf, sizeof(buf), "%d. ",
                                 p->ol_counter[p->list_depth - 1]++);
            (void)n_num;
            doc_emit_text(p->doc, buf, BR_STYLE_LIST_BULLET, -1);
        } else {
            doc_emit_text(p->doc, "• ", BR_STYLE_LIST_BULLET, -1);
        }
        return;
    }
    if (strcmp(n, "dt") == 0) {
        request_break(p, 1);
        flush_text(p);
        push_style(p, BR_STYLE_BOLD);
        return;
    }
    if (strcmp(n, "dd") == 0) {
        request_break(p, 0);
        flush_text(p);
        doc_emit_text(p->doc, "    ", BR_STYLE_NORMAL, -1);
        return;
    }
    if (strcmp(n, "blockquote") == 0) {
        request_break(p, 1);
        flush_text(p);
        doc_emit_text(p->doc, "    │ ", BR_STYLE_NORMAL, -1);
        push_style(p, BR_STYLE_ITALIC);
        return;
    }
    if (strcmp(n, "table") == 0) {
        request_break(p, 1);
        return;
    }
    if (strcmp(n, "tr") == 0) {
        request_break(p, 0);
        return;
    }
    if (strcmp(n, "td") == 0 || strcmp(n, "th") == 0) {
        flush_text(p);
        if (p->buf_len == 0 && p->doc->run_count > 0) {
            const br_run_t *last = &p->doc->runs[p->doc->run_count - 1];
            if (last->kind == BR_RUN_TEXT) {
                doc_emit_text(p->doc, " │ ", BR_STYLE_NORMAL, -1);
            }
        }
        if (strcmp(n, "th") == 0) push_style(p, BR_STYLE_BOLD);
        return;
    }
    if (strcmp(n, "img") == 0) {
        /* Render alt/title/src as inline placeholder. */
        const char *label = NULL;
        if (t->has_alt && t->alt[0] != '\0') label = t->alt;
        else if (t->has_title && t->title[0] != '\0') label = t->title;
        char buf[600];
        if (label != NULL) {
            snprintf(buf, sizeof(buf), "[img: %s]", label);
        } else if (t->has_src) {
            const char *src = t->src;
            const char *slash = strrchr(src, '/');
            snprintf(buf, sizeof(buf), "[img: %s]",
                     slash != NULL ? slash + 1 : src);
        } else {
            snprintf(buf, sizeof(buf), "[img]");
        }
        flush_text(p);
        doc_emit_text(p->doc, buf, BR_STYLE_ITALIC, cur_link(p));
        return;
    }
    if (strcmp(n, "a") == 0) {
        flush_text(p);
        int link_idx = -1;
        if (t->has_href && t->href[0] != '\0' && t->href[0] != '#') {
            link_idx = doc_add_link(p->doc, t->href, strlen(t->href));
        }
        if (p->link_depth < BR_MAX_STYLE_STACK) {
            p->link_stack[p->link_depth++] = link_idx;
        }
        return;
    }
    if (is_block_tag(n)) {
        request_break(p, 1);
    }
}

static void handle_close_tag(br_parser_t *p, const br_tag_t *t) {
    const char *n = t->name;
    if (p->skip_tag[0] != '\0') {
        if (strcmp(p->skip_tag, n) == 0) {
            p->skip_depth--;
            if (p->skip_depth <= 0) {
                p->skip_tag[0] = '\0';
                p->skip_depth = 0;
            }
        }
        return;
    }
    if (strcmp(n, "head") == 0) { p->in_head = 0; return; }
    if (strcmp(n, "title") == 0) {
        /* Snapshot title from accumulator text. */
        if (p->buf_len > 0) {
            size_t take = p->buf_len;
            if (take >= sizeof(p->doc->title)) take = sizeof(p->doc->title) - 1;
            memcpy(p->doc->title, p->buf, take);
            p->doc->title[take] = '\0';
        } else {
            /* title may have been emitted as runs; gather them as title text. */
            size_t off = 0;
            for (size_t i = (size_t)p->title_off; i < p->doc->run_count; i++) {
                const br_run_t *r = &p->doc->runs[i];
                if (r->kind != BR_RUN_TEXT || r->text == NULL) continue;
                size_t tl = strlen(r->text);
                if (off + tl >= sizeof(p->doc->title)) {
                    tl = sizeof(p->doc->title) - 1 - off;
                }
                memcpy(p->doc->title + off, r->text, tl);
                off += tl;
                if (off >= sizeof(p->doc->title) - 1) break;
            }
            p->doc->title[off] = '\0';
        }
        /* Drop accumulated title content (don't render in body). */
        p->buf_len = 0;
        /* Pop accumulated title runs from the document. */
        while (p->doc->run_count > (size_t)p->title_off) {
            free(p->doc->runs[--p->doc->run_count].text);
        }
        p->in_title = 0;
        return;
    }
    if (strcmp(n, "script") == 0 || strcmp(n, "noscript") == 0) { p->in_script = 0; return; }
    if (strcmp(n, "style") == 0) { p->in_style = 0; return; }
    if (strcmp(n, "textarea") == 0) {
        flush_text(p);
        pop_style(p);
        p->in_textarea = 0;
        return;
    }
    if (strcmp(n, "pre") == 0) {
        flush_text(p);
        p->in_pre = 0;
        request_break(p, 1);
        return;
    }
    if (strcmp(n, "h1") == 0 || strcmp(n, "h2") == 0 ||
        strcmp(n, "h3") == 0 || strcmp(n, "h4") == 0 ||
        strcmp(n, "h5") == 0 || strcmp(n, "h6") == 0) {
        flush_text(p); pop_style(p); request_break(p, 1); return;
    }
    if (strcmp(n, "b") == 0 || strcmp(n, "strong") == 0 ||
        strcmp(n, "i") == 0 || strcmp(n, "em") == 0 ||
        strcmp(n, "cite") == 0 || strcmp(n, "var") == 0 ||
        strcmp(n, "dfn") == 0 ||
        strcmp(n, "code") == 0 || strcmp(n, "tt") == 0 ||
        strcmp(n, "kbd") == 0 || strcmp(n, "samp") == 0) {
        flush_text(p); pop_style(p); return;
    }
    if (strcmp(n, "ul") == 0 || strcmp(n, "ol") == 0) {
        if (p->list_depth > 0) p->list_depth--;
        request_break(p, 1);
        return;
    }
    if (strcmp(n, "dt") == 0) {
        flush_text(p); pop_style(p); return;
    }
    if (strcmp(n, "blockquote") == 0) {
        flush_text(p); pop_style(p); request_break(p, 1); return;
    }
    if (strcmp(n, "th") == 0) {
        flush_text(p); pop_style(p); return;
    }
    if (strcmp(n, "a") == 0) {
        flush_text(p);
        if (p->link_depth > 0) p->link_depth--;
        return;
    }
    if (is_block_tag(n)) {
        request_break(p, 1);
    }
}

/* ----- main parse loop ----- */

int br_doc_parse_html(br_doc_t *doc, const char *html, size_t len) {
    if (doc == NULL || html == NULL) return -1;
    br_doc_clear(doc);

    br_parser_t p = {0};
    p.p = html;
    p.end = html + len;
    p.doc = doc;

    while (p.p < p.end) {
        if (p.in_script || p.in_style) {
            /* Skip until matching </script> or </style>. */
            const char *needle = p.in_script ? "</script" : "</style";
            size_t nlen = strlen(needle);
            const char *q = p.p;
            while (q + nlen <= p.end) {
                if (strncasecmp(q, needle, nlen) == 0) break;
                q++;
            }
            p.p = (q + nlen <= p.end) ? q : p.end;
            if (p.p < p.end) {
                br_tag_t t;
                if (parse_tag(&p, &t) == 1 && t.closing) {
                    if (p.in_script && strcmp(t.name, "script") == 0) p.in_script = 0;
                    if (p.in_style && strcmp(t.name, "style") == 0) p.in_style = 0;
                }
            }
            continue;
        }

        /* When inside a skip_tag (svg/math/etc.), only react to its close. */
        if (p.skip_tag[0] != '\0' && *p.p != '<') {
            p.p++;
            continue;
        }

        if (*p.p == '<') {
            apply_pending_break(&p);
            flush_text(&p);
            br_tag_t t;
            int r = parse_tag(&p, &t);
            if (r == 0) {
                /* not actually a tag — treat as literal '<' */
                if (buf_putc(&p, '<') != 0) goto oom;
                p.p++;
                continue;
            }
            if (r == 2) {
                /* comment/doctype — already consumed */
                continue;
            }
            if (t.closing) handle_close_tag(&p, &t);
            else handle_open_tag(&p, &t);
            continue;
        }

        if (*p.p == '&') {
            int r = try_entity(&p);
            if (r < 0) goto oom;
            if (r == 0) {
                if (buf_putc(&p, '&') != 0) goto oom;
                p.p++;
            }
            continue;
        }

        if (p.in_pre) {
            apply_pending_break(&p);
            if (*p.p == '\n') {
                flush_text(&p);
                doc_emit_marker(p.doc, BR_RUN_BREAK);
                p.p++;
                continue;
            }
            if (buf_putc(&p, *p.p) != 0) goto oom;
            p.p++;
            continue;
        }

        /* Collapse whitespace into single spaces between non-space text. */
        if (isspace((unsigned char)*p.p)) {
            /* Skip a run of whitespace. */
            while (p.p < p.end && isspace((unsigned char)*p.p)) p.p++;
            /* Don't emit leading whitespace at start of buffer if a break is
               pending; let the break "absorb" surrounding ws. */
            if (p.pending_break > 0) continue;
            if (p.buf_len == 0 && p.doc->run_count == 0) continue;
            /* Don't put a space at the very start of a fresh line. */
            if (p.buf_len == 0) {
                /* but only emit a single space if previous run ended without
                   whitespace */
                if (p.doc->run_count > 0) {
                    if (buf_putc(&p, ' ') != 0) goto oom;
                }
                continue;
            }
            if (p.buf_len > 0 && p.buf[p.buf_len - 1] != ' ') {
                if (buf_putc(&p, ' ') != 0) goto oom;
            }
            continue;
        }

        apply_pending_break(&p);
        if (buf_putc(&p, *p.p) != 0) goto oom;
        p.p++;
    }

    apply_pending_break(&p);
    flush_text(&p);
    free(p.buf);
    return 0;

oom:
    free(p.buf);
    return -1;
}
