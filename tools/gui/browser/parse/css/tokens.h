/*
 * CSS tokenizer — splits raw CSS text into a stream of tokens.
 *
 * The model is intentionally simple. Only a few token kinds matter for the
 * downstream parser: identifiers, strings, numbers/dimensions, hash
 * (#abc), at-keyword (@media), and the punctuation that delimits rule
 * blocks ({ } ; : , ( )).
 */
#ifndef ONETOOL_TOOLS_GUI_BROWSER_PARSE_CSS_TOKENS_H
#define ONETOOL_TOOLS_GUI_BROWSER_PARSE_CSS_TOKENS_H

#include <stddef.h>

typedef enum {
    BR_CSST_EOF = 0,
    BR_CSST_IDENT,
    BR_CSST_FUNCTION,    /* ident immediately followed by '(' */
    BR_CSST_HASH,        /* "#" + ident — id selector or hex colour */
    BR_CSST_AT,          /* "@" + ident */
    BR_CSST_STRING,
    BR_CSST_NUMBER,
    BR_CSST_DIMENSION,   /* number + unit */
    BR_CSST_PERCENT,
    BR_CSST_DELIM,       /* one-byte punctuation we don't otherwise name */
    BR_CSST_COLON,
    BR_CSST_SEMI,
    BR_CSST_COMMA,
    BR_CSST_LBRACE,
    BR_CSST_RBRACE,
    BR_CSST_LPAREN,
    BR_CSST_RPAREN,
    BR_CSST_LBRACKET,
    BR_CSST_RBRACKET,
    BR_CSST_WHITESPACE,
    BR_CSST_BANG,        /* '!' */
    BR_CSST_DOT,         /* '.' */
    BR_CSST_GT,          /* '>' (child combinator) */
    BR_CSST_PLUS,        /* '+' (adjacent) */
    BR_CSST_TILDE,       /* '~' (general sibling) */
    BR_CSST_STAR,        /* '*' */
} br_css_tok_kind_t;

typedef struct {
    br_css_tok_kind_t kind;
    const char *text;     /* borrowed pointer into the source */
    size_t      len;
    double      num;      /* for NUMBER/DIMENSION/PERCENT */
    char        unit[8];  /* for DIMENSION */
    char        delim;    /* for DELIM */
} br_css_tok_t;

typedef struct {
    const char *p;
    const char *end;
} br_css_lex_t;

void br_css_lex_init(br_css_lex_t *l, const char *src, size_t len);

/* Read the next token. End-of-input yields BR_CSST_EOF and stays there. */
void br_css_lex_next(br_css_lex_t *l, br_css_tok_t *out);

/* Skip a run of BR_CSST_WHITESPACE / comments. After return, the lexer
 * position points at the first non-whitespace byte. */
void br_css_lex_skip_ws(br_css_lex_t *l);

/* Peek the next non-whitespace token (advances the position past
 * whitespace; the token is read normally on the following call). */
void br_css_lex_peek_skipws(br_css_lex_t *l, br_css_tok_t *out);

#endif
