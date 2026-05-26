/*
 * CSS rule parser: turns a token stream into a list of (selectors,
 * declarations) rules.
 *
 * @-rules (@media, @import, @keyframes, @font-face, …) are recognised
 * and skipped en-bloc: we eat tokens until the matching `;` (at-rule
 * with no block) or `{ }` block ends. We don't currently honour
 * @media query bodies — every rule inside is just ignored. Most pages
 * still look fine because their @media-only rules tend to be visual
 * polish.
 */
#include "internal.h"
#include "tokens.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

br_stylesheet_t *br_css_stylesheet_create(void) {
    br_stylesheet_t *ss = (br_stylesheet_t *)calloc(1, sizeof(br_stylesheet_t));
    return ss;
}

void br_css_stylesheet_destroy(br_stylesheet_t *ss) {
    if (ss == NULL) return;
    for (int i = 0; i < ss->rule_count; i++) {
        free(ss->rules[i].selectors);
        free(ss->rules[i].decls);
    }
    free(ss->rules);
    free(ss);
}

int br_css_stylesheet_push_rule(br_stylesheet_t *ss, br_css_rule_t rule) {
    if (ss->rule_count == ss->rule_cap) {
        int want = ss->rule_cap == 0 ? 32 : ss->rule_cap * 2;
        br_css_rule_t *p = (br_css_rule_t *)realloc(ss->rules,
                                                    (size_t)want * sizeof(br_css_rule_t));
        if (p == NULL) {
            free(rule.selectors);
            free(rule.decls);
            return -1;
        }
        ss->rules = p;
        ss->rule_cap = want;
    }
    rule.order = ss->rule_count;
    ss->rules[ss->rule_count++] = rule;
    return 0;
}

/* Reusable buffer for parse_decl_block. */
typedef struct {
    br_css_decl_t *decls;
    int            count;
    int            cap;
} decl_buf_t;

static int decl_buf_push(decl_buf_t *b, br_css_decl_t d) {
    if (b->count == b->cap) {
        int want = b->cap == 0 ? 8 : b->cap * 2;
        br_css_decl_t *p = (br_css_decl_t *)realloc(b->decls,
                                                    (size_t)want * sizeof(br_css_decl_t));
        if (p == NULL) return -1;
        b->decls = p;
        b->cap = want;
    }
    b->decls[b->count++] = d;
    return 0;
}

/* Parse 'prop: value;' style declarations from a contiguous block. */
int br_css_parse_decl_block(const char *src, size_t len,
                            br_css_decl_t **out_decls, int *out_count) {
    decl_buf_t buf = { *out_decls, *out_count, *out_count };
    (void)0;  /* keep the existing array; new entries are appended */
    /* Re-init buf so a NULL/cap mismatch doesn't blow up. */
    buf.cap = *out_count;

    const char *p = src;
    const char *end = src + len;

    while (p < end) {
        /* Skip whitespace and stray semicolons. */
        while (p < end && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (p >= end) break;

        /* Property name. */
        const char *name_start = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                           *p == '_')) p++;
        size_t name_len = (size_t)(p - name_start);
        if (name_len == 0) {
            /* malformed — skip until next semicolon */
            while (p < end && *p != ';') p++;
            continue;
        }
        /* Skip whitespace, then expect ':'. */
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != ':') {
            while (p < end && *p != ';') p++;
            continue;
        }
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;

        /* Value spans until the next top-level ';' or end. We have to be
         * careful with parentheses (rgb(255,0,0)) and strings. */
        const char *val_start = p;
        int paren = 0;
        char quote = 0;
        while (p < end) {
            char c = *p;
            if (quote != 0) {
                if (c == quote) quote = 0;
                else if (c == '\\' && p + 1 < end) p++;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') paren++;
            else if (c == ')' && paren > 0) paren--;
            else if (c == ';' && paren == 0) break;
            p++;
        }
        size_t val_len = (size_t)(p - val_start);

        /* Trim. */
        while (val_len > 0 && (val_start[val_len - 1] == ' ' ||
                               val_start[val_len - 1] == '\t' ||
                               val_start[val_len - 1] == '\n' ||
                               val_start[val_len - 1] == '\r')) val_len--;

        /* Detect !important. */
        int important = 0;
        if (val_len >= 10) {
            const char *bang = NULL;
            for (size_t i = val_len; i > 0; i--) {
                if (val_start[i - 1] == '!') { bang = val_start + i - 1; break; }
                if (val_start[i - 1] == ' ' || val_start[i - 1] == '\t') continue;
                if (i >= 9 && strncasecmp(val_start + i - 9, "important", 9) == 0)
                    continue;
                break;
            }
            if (bang != NULL) {
                const char *q = bang + 1;
                while (q < val_start + val_len && isspace((unsigned char)*q)) q++;
                if (val_start + val_len - q >= 9 &&
                    strncasecmp(q, "important", 9) == 0) {
                    important = 1;
                    /* trim the !important suffix from value */
                    val_len = (size_t)(bang - val_start);
                    while (val_len > 0 && (val_start[val_len - 1] == ' ' ||
                                           val_start[val_len - 1] == '\t'))
                        val_len--;
                }
            }
        }

        br_css_prop_t prop = br_css_prop_lookup(name_start, name_len);
        if (prop != BR_CSS_PROP_UNKNOWN) {
            br_css_value_t v;
            if (br_css_value_parse(val_start, val_len, &v) == 0) {
                br_css_decl_t d;
                d.prop = prop;
                d.value = v;
                d.important = important;
                if (decl_buf_push(&buf, d) != 0) {
                    *out_decls = buf.decls;
                    *out_count = buf.count;
                    return -1;
                }
            }
        }
        if (p < end && *p == ';') p++;
    }

    *out_decls = buf.decls;
    *out_count = buf.count;
    return 0;
}

/* Skip the next "block": either a `{...}` (with brace matching) or — if
 * we don't see `{` — everything up to the next `;`. */
static void skip_block_or_semicolon(br_css_lex_t *l) {
    br_css_tok_t t;
    int depth = 0;
    int saw_brace = 0;
    for (;;) {
        if (l->p >= l->end) return;
        br_css_lex_next(l, &t);
        if (t.kind == BR_CSST_EOF) return;
        if (t.kind == BR_CSST_LBRACE) { depth++; saw_brace = 1; }
        else if (t.kind == BR_CSST_RBRACE) {
            if (depth > 0) depth--;
            if (saw_brace && depth == 0) return;
        } else if (t.kind == BR_CSST_SEMI && !saw_brace) {
            return;
        }
    }
}

/* Parse a single rule starting from the lexer's current position. Returns
 * 1 if a rule was produced (and appended to ss), 0 on EOF, -1 on error. */
static int parse_rule(br_css_lex_t *l, br_stylesheet_t *ss) {
    br_css_lex_skip_ws(l);
    if (l->p >= l->end) return 0;

    /* @-rule? */
    br_css_tok_t peek;
    br_css_lex_peek_skipws(l, &peek);
    if (peek.kind == BR_CSST_AT) {
        skip_block_or_semicolon(l);
        return 1;
    }
    if (peek.kind == BR_CSST_EOF) return 0;
    if (peek.kind == BR_CSST_RBRACE) {
        /* stray brace — consume and continue */
        br_css_lex_next(l, &peek);
        return 1;
    }

    /* Collect selector text until '{' or end. We tokenise to handle
     * quoted strings inside selectors (rare) and to know where '{' is,
     * but we hand the raw bytes to br_css_selector_parse to keep this
     * simpler. */
    const char *sel_start = l->p;
    int depth = 0;
    const char *sel_end = NULL;
    while (l->p < l->end) {
        char c = *l->p;
        if (c == '{' && depth == 0) {
            sel_end = l->p;
            l->p++;
            break;
        }
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth > 0) depth--;
        l->p++;
    }
    if (sel_end == NULL) {
        /* Selector with no body. */
        return 1;
    }

    /* Selector list — split on commas, ignoring commas inside parens. */
    br_css_selector_t *selectors = NULL;
    int sel_count = 0;
    int sel_cap = 0;
    const char *q = sel_start;
    const char *e = sel_end;
    int dep = 0;
    const char *cur_start = q;
    while (q <= e) {
        char c = q < e ? *q : ',';
        if ((c == ',' && dep == 0) || q == e) {
            const char *cs = cur_start;
            size_t cl = (size_t)(q - cs);
            /* trim */
            while (cl > 0 && (cs[0] == ' ' || cs[0] == '\t' || cs[0] == '\n' ||
                              cs[0] == '\r')) { cs++; cl--; }
            while (cl > 0 && (cs[cl-1] == ' ' || cs[cl-1] == '\t' ||
                              cs[cl-1] == '\n' || cs[cl-1] == '\r')) cl--;
            if (cl > 0) {
                br_css_selector_t s;
                if (br_css_selector_parse(cs, cl, &s) == 0) {
                    if (sel_count == sel_cap) {
                        int want = sel_cap == 0 ? 4 : sel_cap * 2;
                        br_css_selector_t *p = (br_css_selector_t *)realloc(
                            selectors, (size_t)want * sizeof(br_css_selector_t));
                        if (p == NULL) {
                            free(selectors);
                            /* skip block */
                            skip_block_or_semicolon(l);
                            return -1;
                        }
                        selectors = p;
                        sel_cap = want;
                    }
                    selectors[sel_count++] = s;
                }
            }
            if (q == e) break;
            q++;
            cur_start = q;
            continue;
        }
        if (c == '(' || c == '[') dep++;
        else if ((c == ')' || c == ']') && dep > 0) dep--;
        q++;
    }

    /* Declaration block — find matching '}'. */
    const char *body_start = l->p;
    int br_depth = 1;
    while (l->p < l->end && br_depth > 0) {
        char c = *l->p;
        if (c == '{') br_depth++;
        else if (c == '}') { br_depth--; if (br_depth == 0) break; }
        l->p++;
    }
    const char *body_end = l->p;
    if (l->p < l->end) l->p++;  /* consume '}' */

    if (sel_count == 0) {
        free(selectors);
        return 1;
    }

    br_css_decl_t *decls = NULL;
    int decl_count = 0;
    br_css_parse_decl_block(body_start, (size_t)(body_end - body_start),
                            &decls, &decl_count);

    br_css_rule_t rule;
    rule.selectors = selectors;
    rule.selector_count = sel_count;
    rule.decls = decls;
    rule.decl_count = decl_count;
    rule.order = 0;
    if (br_css_stylesheet_push_rule(ss, rule) != 0) return -1;
    return 1;
}

int br_css_parse_into(br_stylesheet_t *ss, const char *css, size_t len) {
    if (ss == NULL || css == NULL || len == 0) return 0;
    br_css_lex_t lex;
    br_css_lex_init(&lex, css, len);
    while (lex.p < lex.end) {
        int r = parse_rule(&lex, ss);
        if (r < 0) return -1;
        if (r == 0) break;
    }
    return 0;
}
