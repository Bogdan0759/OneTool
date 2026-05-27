#include "../src/internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void skip_ws(js_lexer_t *lex) {
    for (;;) {
        if (*lex->p == '\0') return;
        if (isspace((unsigned char)*lex->p)) {
            if (*lex->p == '\n') {
                lex->line++;
                lex->col = 1;
            } else {
                lex->col++;
            }
            lex->p++;
            continue;
        }
        if (lex->p[0] == '/' && lex->p[1] == '/') {
            while (*lex->p != '\0' && *lex->p != '\n') {
                lex->p++;
                lex->col++;
            }
            continue;
        }
        if (lex->p[0] == '/' && lex->p[1] == '*') {
            lex->p += 2;
            lex->col += 2;
            while (*lex->p != '\0' && !(lex->p[0] == '*' && lex->p[1] == '/')) {
                if (*lex->p == '\n') {
                    lex->line++;
                    lex->col = 1;
                    lex->p++;
                } else {
                    lex->p++;
                    lex->col++;
                }
            }
            if (*lex->p != '\0') {
                lex->p += 2;
                lex->col += 2;
            }
            continue;
        }
        return;
    }
}

static js_token_kind_t keyword_kind(const char *s, size_t len) {
    if (len == 3 && strncmp(s, "let", 3) == 0) return JS_TOK_KW_LET;
    if (len == 5 && strncmp(s, "const", 5) == 0) return JS_TOK_KW_CONST;
    if (len == 8 && strncmp(s, "function", 8) == 0) return JS_TOK_KW_FUNCTION;
    if (len == 6 && strncmp(s, "return", 6) == 0) return JS_TOK_KW_RETURN;
    if (len == 2 && strncmp(s, "if", 2) == 0) return JS_TOK_KW_IF;
    if (len == 4 && strncmp(s, "else", 4) == 0) return JS_TOK_KW_ELSE;
    if (len == 5 && strncmp(s, "while", 5) == 0) return JS_TOK_KW_WHILE;
    if (len == 4 && strncmp(s, "true", 4) == 0) return JS_TOK_KW_TRUE;
    if (len == 5 && strncmp(s, "false", 5) == 0) return JS_TOK_KW_FALSE;
    if (len == 4 && strncmp(s, "null", 4) == 0) return JS_TOK_KW_NULL;
    if (len == 9 && strncmp(s, "undefined", 9) == 0) return JS_TOK_KW_UNDEFINED;
    return JS_TOK_IDENT;
}

void js_lex_init(js_lexer_t *lex, const char *src) {
    lex->src = src != NULL ? src : "";
    lex->p = lex->src;
    lex->line = 1;
    lex->col = 1;
}

void js_lex_next(js_lexer_t *lex, js_token_t *out) {
    skip_ws(lex);
    memset(out, 0, sizeof(*out));
    out->start = lex->p;
    out->line = lex->line;
    out->col = lex->col;

    char c = *lex->p;
    if (c == '\0') {
        out->kind = JS_TOK_EOF;
        return;
    }

    if (isalpha((unsigned char)c) || c == '_' || c == '$') {
        const char *start = lex->p;
        while (isalnum((unsigned char)*lex->p) || *lex->p == '_' || *lex->p == '$') {
            lex->p++;
            lex->col++;
        }
        out->start = start;
        out->len = (size_t)(lex->p - start);
        out->kind = keyword_kind(start, out->len);
        return;
    }

    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)lex->p[1]))) {
        char *endp = NULL;
        out->number = strtod(lex->p, &endp);
        out->len = (size_t)(endp - lex->p);
        out->kind = JS_TOK_NUMBER;
        lex->col += (int)out->len;
        lex->p = endp;
        return;
    }

    if (c == '"' || c == '\'') {
        char quote = c;
        const char *start = ++lex->p;
        lex->col++;
        while (*lex->p != '\0' && *lex->p != quote) {
            if (*lex->p == '\\' && lex->p[1] != '\0') {
                lex->p += 2;
                lex->col += 2;
                continue;
            }
            if (*lex->p == '\n') {
                lex->line++;
                lex->col = 1;
                lex->p++;
                continue;
            }
            lex->p++;
            lex->col++;
        }
        out->start = start;
        out->len = (size_t)(lex->p - start);
        out->kind = JS_TOK_STRING;
        if (*lex->p == quote) {
            lex->p++;
            lex->col++;
        }
        return;
    }

    #define ONE(ch, tok_kind) case ch: out->kind = tok_kind; lex->p++; lex->col++; return
    switch (c) {
        ONE('(', JS_TOK_LPAREN);
        ONE(')', JS_TOK_RPAREN);
        ONE('{', JS_TOK_LBRACE);
        ONE('}', JS_TOK_RBRACE);
        ONE('.', JS_TOK_DOT);
        ONE(':', JS_TOK_COLON);
        ONE(',', JS_TOK_COMMA);
        ONE(';', JS_TOK_SEMI);
        ONE('+', JS_TOK_PLUS);
        ONE('-', JS_TOK_MINUS);
        ONE('*', JS_TOK_STAR);
        ONE('%', JS_TOK_PERCENT);
        ONE('/', JS_TOK_SLASH);
    }
    #undef ONE

    if (c == '=' && lex->p[1] == '=') {
        out->kind = JS_TOK_EQEQ; out->len = 2; lex->p += 2; lex->col += 2; return;
    }
    if (c == '!' && lex->p[1] == '=') {
        out->kind = JS_TOK_NEQ; out->len = 2; lex->p += 2; lex->col += 2; return;
    }
    if (c == '<' && lex->p[1] == '=') {
        out->kind = JS_TOK_LTE; out->len = 2; lex->p += 2; lex->col += 2; return;
    }
    if (c == '>' && lex->p[1] == '=') {
        out->kind = JS_TOK_GTE; out->len = 2; lex->p += 2; lex->col += 2; return;
    }
    if (c == '&' && lex->p[1] == '&') {
        out->kind = JS_TOK_ANDAND; out->len = 2; lex->p += 2; lex->col += 2; return;
    }
    if (c == '|' && lex->p[1] == '|') {
        out->kind = JS_TOK_OROR; out->len = 2; lex->p += 2; lex->col += 2; return;
    }

    switch (c) {
        case '!': out->kind = JS_TOK_BANG; break;
        case '=': out->kind = JS_TOK_EQ; break;
        case '<': out->kind = JS_TOK_LT; break;
        case '>': out->kind = JS_TOK_GT; break;
        default: out->kind = JS_TOK_EOF; break;
    }
    lex->p++;
    lex->col++;
}
