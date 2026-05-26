/*
 * CSS tokenizer implementation.
 *
 * We deliberately don't follow the CSS Syntax Module 3 spec exactly — the
 * goal is to produce a sequence the downstream parser can walk without
 * surprises. Unknown characters become DELIM tokens; comments are
 * silently dropped.
 */
#include "tokens.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void br_css_lex_init(br_css_lex_t *l, const char *src, size_t len) {
    l->p = src;
    l->end = src + len;
}

static int is_ident_start(int c) {
    return isalpha(c) || c == '_' || c == '-' || (c & 0x80);
}

static int is_ident_continue(int c) {
    return isalnum(c) || c == '_' || c == '-' || (c & 0x80);
}

static void skip_comment(br_css_lex_t *l) {
    if (l->p + 1 < l->end && l->p[0] == '/' && l->p[1] == '*') {
        l->p += 2;
        while (l->p + 1 < l->end && !(l->p[0] == '*' && l->p[1] == '/'))
            l->p++;
        if (l->p + 1 < l->end) l->p += 2;
        else l->p = l->end;
    }
}

void br_css_lex_skip_ws(br_css_lex_t *l) {
    for (;;) {
        if (l->p >= l->end) return;
        unsigned char c = (unsigned char)*l->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f') {
            l->p++;
            continue;
        }
        if (c == '/' && l->p + 1 < l->end && l->p[1] == '*') {
            skip_comment(l);
            continue;
        }
        return;
    }
}

static void read_ident(br_css_lex_t *l, const char **out_text,
                       size_t *out_len) {
    const char *start = l->p;
    if (l->p < l->end && *l->p == '-') l->p++; /* leading hyphen */
    while (l->p < l->end && is_ident_continue((unsigned char)*l->p)) l->p++;
    *out_text = start;
    *out_len = (size_t)(l->p - start);
}

static void read_string(br_css_lex_t *l, br_css_tok_t *out) {
    char quote = *l->p;
    l->p++;
    const char *start = l->p;
    while (l->p < l->end && *l->p != quote) {
        if (*l->p == '\\' && l->p + 1 < l->end) l->p++;  /* allow escape */
        l->p++;
    }
    out->kind = BR_CSST_STRING;
    out->text = start;
    out->len = (size_t)(l->p - start);
    if (l->p < l->end) l->p++; /* consume closing quote */
}

static int parse_double(const char *s, size_t len, double *out) {
    /* a small, dependency-free parser for [+-]?digits[.digits]?(e[+-]?digits)? */
    char buf[64];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    char *endp = NULL;
    double v = strtod(buf, &endp);
    if (endp == buf) return -1;
    *out = v;
    return 0;
}

static void read_number(br_css_lex_t *l, br_css_tok_t *out) {
    const char *start = l->p;
    if (*l->p == '+' || *l->p == '-') l->p++;
    while (l->p < l->end && isdigit((unsigned char)*l->p)) l->p++;
    if (l->p < l->end && *l->p == '.') {
        l->p++;
        while (l->p < l->end && isdigit((unsigned char)*l->p)) l->p++;
    }
    if (l->p < l->end && (*l->p == 'e' || *l->p == 'E')) {
        l->p++;
        if (l->p < l->end && (*l->p == '+' || *l->p == '-')) l->p++;
        while (l->p < l->end && isdigit((unsigned char)*l->p)) l->p++;
    }
    size_t numlen = (size_t)(l->p - start);
    double v = 0;
    parse_double(start, numlen, &v);
    out->text = start;
    out->len = numlen;
    out->num = v;
    out->unit[0] = '\0';

    if (l->p < l->end && *l->p == '%') {
        out->kind = BR_CSST_PERCENT;
        out->unit[0] = '%';
        out->unit[1] = '\0';
        l->p++;
        return;
    }
    if (l->p < l->end && is_ident_start((unsigned char)*l->p)) {
        const char *us = l->p;
        while (l->p < l->end && is_ident_continue((unsigned char)*l->p)) l->p++;
        size_t ul = (size_t)(l->p - us);
        if (ul >= sizeof(out->unit)) ul = sizeof(out->unit) - 1;
        memcpy(out->unit, us, ul);
        out->unit[ul] = '\0';
        out->kind = BR_CSST_DIMENSION;
        return;
    }
    out->kind = BR_CSST_NUMBER;
}

void br_css_lex_next(br_css_lex_t *l, br_css_tok_t *out) {
    memset(out, 0, sizeof(*out));
    /* Whitespace + comments collapse into a single WHITESPACE token. */
    if (l->p < l->end) {
        unsigned char c = (unsigned char)*l->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            (c == '/' && l->p + 1 < l->end && l->p[1] == '*')) {
            const char *start = l->p;
            br_css_lex_skip_ws(l);
            out->kind = BR_CSST_WHITESPACE;
            out->text = start;
            out->len = (size_t)(l->p - start);
            return;
        }
    }

    if (l->p >= l->end) {
        out->kind = BR_CSST_EOF;
        return;
    }

    unsigned char c = (unsigned char)*l->p;

    if (c == '"' || c == '\'') { read_string(l, out); return; }

    if (c == '#') {
        l->p++;
        const char *t; size_t n;
        read_ident(l, &t, &n);
        out->kind = BR_CSST_HASH;
        out->text = t;
        out->len = n;
        return;
    }
    if (c == '@') {
        l->p++;
        const char *t; size_t n;
        read_ident(l, &t, &n);
        out->kind = BR_CSST_AT;
        out->text = t;
        out->len = n;
        return;
    }

    if (isdigit(c) || (c == '.' && l->p + 1 < l->end &&
                       isdigit((unsigned char)l->p[1])) ||
        ((c == '+' || c == '-') && l->p + 1 < l->end &&
         (isdigit((unsigned char)l->p[1]) ||
          (l->p[1] == '.' && l->p + 2 < l->end &&
           isdigit((unsigned char)l->p[2]))))) {
        read_number(l, out);
        return;
    }

    if (is_ident_start(c)) {
        const char *t; size_t n;
        read_ident(l, &t, &n);
        out->text = t;
        out->len = n;
        if (l->p < l->end && *l->p == '(') {
            out->kind = BR_CSST_FUNCTION;
            l->p++;
        } else {
            out->kind = BR_CSST_IDENT;
        }
        return;
    }

    /* Single-char tokens. */
    out->text = l->p;
    out->len = 1;
    out->delim = (char)c;
    switch (c) {
        case ':':  out->kind = BR_CSST_COLON; break;
        case ';':  out->kind = BR_CSST_SEMI; break;
        case ',':  out->kind = BR_CSST_COMMA; break;
        case '{':  out->kind = BR_CSST_LBRACE; break;
        case '}':  out->kind = BR_CSST_RBRACE; break;
        case '(':  out->kind = BR_CSST_LPAREN; break;
        case ')':  out->kind = BR_CSST_RPAREN; break;
        case '[':  out->kind = BR_CSST_LBRACKET; break;
        case ']':  out->kind = BR_CSST_RBRACKET; break;
        case '!':  out->kind = BR_CSST_BANG; break;
        case '.':  out->kind = BR_CSST_DOT; break;
        case '>':  out->kind = BR_CSST_GT; break;
        case '+':  out->kind = BR_CSST_PLUS; break;
        case '~':  out->kind = BR_CSST_TILDE; break;
        case '*':  out->kind = BR_CSST_STAR; break;
        default:   out->kind = BR_CSST_DELIM; break;
    }
    l->p++;
}

void br_css_lex_peek_skipws(br_css_lex_t *l, br_css_tok_t *out) {
    br_css_lex_skip_ws(l);
    const char *save = l->p;
    br_css_lex_next(l, out);
    l->p = save;
}
