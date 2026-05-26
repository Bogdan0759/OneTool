/*
 * CSS selector parsing + matching.
 *
 * Supports a useful subset: tag, .class, #id, *, plus the descendant
 * combinator (whitespace). Pseudo-classes are tolerated but most are no-
 * ops; :hover is recorded so the renderer could implement it, but the
 * cascade just treats matching as "always" for now. We don't model child
 * (>) or sibling (+/~) combinators — they're parsed but degrade into
 * "matches once descendant somewhere above" so the rule is over-broad
 * rather than failing closed.
 */
#include "internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

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

/* Parse a compound selector (no whitespace inside) starting at `*pp`.
 * Advances `*pp` past the compound. Returns 0 on success. */
static int parse_compound(const char **pp, const char *end,
                          br_css_compound_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = *pp;
    int got_any = 0;

    while (p < end) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' ||
            c == '+' || c == '~' || c == ',') break;
        if (c == '*') {
            /* universal — leave tag empty */
            p++;
            got_any = 1;
            continue;
        }
        if (c == '.') {
            p++;
            const char *start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                               *p == '_' || (*p & 0x80))) p++;
            size_t len = (size_t)(p - start);
            if (len > 0 && out->class_count < BROWSER_ELEMENT_CLASSES) {
                size_t take = len < BROWSER_ELEMENT_CLASS_MAX
                              ? len : BROWSER_ELEMENT_CLASS_MAX - 1;
                memcpy(out->classes[out->class_count], start, take);
                out->classes[out->class_count][take] = '\0';
                out->class_count++;
            }
            got_any = 1;
            continue;
        }
        if (c == '#') {
            p++;
            const char *start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                               *p == '_' || (*p & 0x80))) p++;
            lower_copy(start, (size_t)(p - start), out->id, sizeof(out->id));
            got_any = 1;
            continue;
        }
        if (c == ':') {
            p++;
            /* allow ::pseudo-element */
            if (p < end && *p == ':') p++;
            const char *start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-')) p++;
            char name[24];
            lower_copy(start, (size_t)(p - start), name, sizeof(name));
            /* Skip a parenthesised argument, e.g. :nth-child(2n) */
            if (p < end && *p == '(') {
                int depth = 1;
                p++;
                while (p < end && depth > 0) {
                    if (*p == '(') depth++;
                    else if (*p == ')') depth--;
                    p++;
                }
            }
            if (strcmp(name, "hover") == 0) out->pseudo_hover = 1;
            else if (strcmp(name, "link") == 0 ||
                     strcmp(name, "visited") == 0 ||
                     strcmp(name, "any-link") == 0) out->pseudo_link = 1;
            else if (strcmp(name, "first-child") == 0 ||
                     strcmp(name, "first-of-type") == 0) out->pseudo_first = 1;
            else if (strcmp(name, "root") == 0) {
                /* allow — treated as plain tag selector against root */
            } else {
                out->pseudo_unsupported = 1;
            }
            got_any = 1;
            continue;
        }
        if (c == '[') {
            /* Attribute selector — parse-and-ignore for now. */
            p++;
            while (p < end && *p != ']') p++;
            if (p < end) p++;
            out->pseudo_unsupported = 1;
            got_any = 1;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '-' || c == '_' || (c & 0x80)) {
            const char *start = p;
            while (p < end && (isalnum((unsigned char)*p) || *p == '-' ||
                               *p == '_' || (*p & 0x80))) p++;
            lower_copy(start, (size_t)(p - start), out->tag, sizeof(out->tag));
            got_any = 1;
            continue;
        }
        /* unknown char — bail out to avoid an infinite loop */
        break;
    }

    *pp = p;
    return got_any ? 0 : -1;
}

int br_css_selector_parse(const char *src, size_t len,
                          br_css_selector_t *out) {
    memset(out, 0, sizeof(*out));
    const char *p = src;
    const char *end = src + len;

    while (p < end && (*p == ' ' || *p == '\t')) p++;

    while (p < end && out->part_count < BR_CSS_SELECTOR_PARTS) {
        /* Skip combinator chars; we don't differentiate them. */
        while (p < end && (*p == ' ' || *p == '\t' || *p == '>' ||
                           *p == '+' || *p == '~' || *p == '\n')) p++;
        if (p >= end) break;
        br_css_compound_t c;
        if (parse_compound(&p, end, &c) != 0) return -1;
        out->parts[out->part_count++] = c;
    }
    if (out->part_count == 0) return -1;

    /* Compute specificity: a = ids, b = classes+pseudoclasses, c = tags */
    int a = 0, b = 0, c_ = 0;
    for (int i = 0; i < out->part_count; i++) {
        const br_css_compound_t *cp = &out->parts[i];
        if (cp->id[0] != '\0') a++;
        b += cp->class_count;
        if (cp->pseudo_hover) b++;
        if (cp->pseudo_first) b++;
        if (cp->pseudo_link) b++;
        if (cp->tag[0] != '\0') c_++;
    }
    out->specificity = a * 10000 + b * 100 + c_;
    return 0;
}

static int compound_matches(const br_css_compound_t *c,
                            const br_element_t *e) {
    if (c->tag[0] != '\0' && strcmp(c->tag, e->tag) != 0) return 0;
    if (c->id[0] != '\0' && strcmp(c->id, e->id) != 0) return 0;
    for (int i = 0; i < c->class_count; i++) {
        int found = 0;
        for (int j = 0; j < e->class_count; j++) {
            if (strcmp(c->classes[i], e->classes[j]) == 0) { found = 1; break; }
        }
        if (!found) return 0;
    }
    if (c->pseudo_link) {
        /* matches only links — element must be <a> */
        if (strcmp(e->tag, "a") != 0) return 0;
    }
    /* :hover / :first-* — accept liberally; the renderer doesn't track
     * either state. */
    if (c->pseudo_unsupported) return 0;
    return 1;
}

int br_css_selector_matches(const br_css_selector_t *sel,
                            const br_doc_t *doc, int el_index) {
    if (sel->part_count <= 0) return 0;
    if (el_index < 0 || (size_t)el_index >= doc->element_count) return 0;

    /* Subject must match the rightmost compound. */
    int part = sel->part_count - 1;
    const br_element_t *e = &doc->elements[el_index];
    if (!compound_matches(&sel->parts[part], e)) return 0;
    part--;

    /* Walk up the ancestor chain trying to satisfy the remaining compounds
     * in order. Descendant combinator semantics: each previous compound
     * matches some ancestor. */
    int idx = e->parent;
    while (part >= 0 && idx >= 0) {
        const br_element_t *anc = &doc->elements[idx];
        if (compound_matches(&sel->parts[part], anc)) {
            part--;
        }
        idx = anc->parent;
    }
    return part < 0 ? 1 : 0;
}
