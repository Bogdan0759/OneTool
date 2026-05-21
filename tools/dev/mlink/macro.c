#include "mlink.h"

#include <stdlib.h>
#include <string.h>

static ml_macro_t *append_macro(ml_context_t *ctx) {
    ml_macro_t *m;
    if (ctx->macro_count == ctx->macro_cap) {
        size_t next = ctx->macro_cap == 0 ? 8 : ctx->macro_cap * 2;
        ctx->macros = ml_xrealloc(ctx->macros, next * sizeof(ctx->macros[0]));
        ctx->macro_cap = next;
    }
    m = &ctx->macros[ctx->macro_count++];
    memset(m, 0, sizeof(*m));
    return m;
}

static const char *skip_spaces(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char *take_word(const char **pp) {
    const char *p = skip_spaces(*pp);
    const char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '=') p++;
    if (p == start) {
        return NULL;
    }
    *pp = p;
    return ml_xstrndup(start, (size_t)(p - start));
}

static int parse_primary(const char *tok, ml_macro_term_part_t *out) {
    if (tok[0] >= '0' && tok[0] <= '9') {
        uint64_t v;
        if (ml_parse_u64(tok, &v) != 0) {
            return 1;
        }
        out->is_symbol = 0;
        out->value = v;
        out->name = NULL;
        return 0;
    }
    out->is_symbol = 1;
    out->name = ml_xstrdup(tok);
    out->value = 0;
    return 0;
}

static int parse_expr(const char *raw, ml_macro_term_t *out) {
    const char *p = skip_spaces(raw);
    int sign = 1;

    out->parts = NULL;
    out->count = 0;

    if (*p == '\0') {
        return 1;
    }
    if (*p == '+') { sign = 1;  p++; }
    else if (*p == '-') { sign = -1; p++; }

    for (;;) {
        const char *start;
        char *tok;
        ml_macro_term_part_t part;

        p = skip_spaces(p);
        start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '+' && *p != '-') p++;
        if (p == start) {
            return 1;
        }
        tok = ml_xstrndup(start, (size_t)(p - start));
        if (parse_primary(tok, &part) != 0) {
            free(tok);
            return 1;
        }
        free(tok);
        part.sign = sign;

        out->parts = ml_xrealloc(out->parts,
                                 (out->count + 1) * sizeof(out->parts[0]));
        out->parts[out->count++] = part;

        p = skip_spaces(p);
        if (*p == '\0') {
            return 0;
        }
        if (*p == '+') { sign = 1;  p++; continue; }
        if (*p == '-') { sign = -1; p++; continue; }
        return 1;
    }
}

void ml_macro_term_free(ml_macro_term_t *t) {
    for (size_t i = 0; i < t->count; i++) {
        free(t->parts[i].name);
    }
    free(t->parts);
    t->parts = NULL;
    t->count = 0;
}

static char *take_term(const char **pp) {
    const char *p = skip_spaces(*pp);
    const char *start = p;
    const char *end;
    while (*p && *p != ',' &&
           *p != '<' && *p != '>' && *p != '=' && *p != '!') {
        p++;
    }
    if (p == start) {
        return NULL;
    }
    end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
    if (end == start) {
        return NULL;
    }
    *pp = p;
    return ml_xstrndup(start, (size_t)(end - start));
}

static char *take_op(const char **pp) {
    const char *p = skip_spaces(*pp);
    char buf[3] = {0, 0, 0};
    size_t n = 0;
    if (*p == '<' || *p == '>' || *p == '=' || *p == '!') {
        buf[n++] = *p++;
        if (*p == '=') {
            buf[n++] = *p++;
        }
    }
    if (n == 0) {
        return NULL;
    }
    *pp = p;
    return ml_xstrdup(buf);
}

static int parse_op(const char *s, ml_assert_op_t *out) {
    if (strcmp(s, "<") == 0)  { *out = ML_ASSERT_LT; return 0; }
    if (strcmp(s, "<=") == 0) { *out = ML_ASSERT_LE; return 0; }
    if (strcmp(s, ">") == 0)  { *out = ML_ASSERT_GT; return 0; }
    if (strcmp(s, ">=") == 0) { *out = ML_ASSERT_GE; return 0; }
    if (strcmp(s, "==") == 0) { *out = ML_ASSERT_EQ; return 0; }
    if (strcmp(s, "!=") == 0) { *out = ML_ASSERT_NE; return 0; }
    return 1;
}

static const char *op_name(ml_assert_op_t op) {
    switch (op) {
    case ML_ASSERT_LT: return "<";
    case ML_ASSERT_LE: return "<=";
    case ML_ASSERT_GT: return ">";
    case ML_ASSERT_GE: return ">=";
    case ML_ASSERT_EQ: return "==";
    case ML_ASSERT_NE: return "!=";
    }
    return "?";
}

static int eval_op(uint64_t l, ml_assert_op_t op, uint64_t r) {
    switch (op) {
    case ML_ASSERT_LT: return l <  r;
    case ML_ASSERT_LE: return l <= r;
    case ML_ASSERT_GT: return l >  r;
    case ML_ASSERT_GE: return l >= r;
    case ML_ASSERT_EQ: return l == r;
    case ML_ASSERT_NE: return l != r;
    }
    return 0;
}

static int parse_name_eq_value(ml_context_t *ctx, ml_object_t *obj, const char *line,
                               char **name_out, uint64_t *val_out) {
    const char *eq = strchr(line, '=');
    char *name;
    char *value_str;
    const char *vp;
    size_t n;

    if (eq == NULL) {
        ml_error(ctx, "%s: macro expects NAME = VALUE: '%s'", obj->name, line);
        return 1;
    }
    n = (size_t)(eq - line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
    if (n == 0) {
        ml_error(ctx, "%s: macro has empty name: '%s'", obj->name, line);
        return 1;
    }
    name = ml_xstrndup(line, n);
    vp = skip_spaces(eq + 1);
    value_str = ml_xstrdup(vp);
    if (ml_parse_u64(value_str, val_out) != 0) {
        ml_error(ctx, "%s: macro: invalid value '%s'", obj->name, value_str);
        free(value_str);
        free(name);
        return 1;
    }
    free(value_str);
    *name_out = name;
    return 0;
}

static int parse_assert(ml_context_t *ctx, ml_object_t *obj, const char *args,
                        ml_macro_t *m) {
    const char *comma = strchr(args, ',');
    char *expr;
    char *lhs_str = NULL;
    char *op_str = NULL;
    char *rhs_str = NULL;
    const char *p;
    size_t n;
    int rc = 1;

    if (comma != NULL) {
        n = (size_t)(comma - args);
        expr = ml_xstrndup(args, n);
        p = skip_spaces(comma + 1);
        if (*p == '"') {
            const char *end;
            p++;
            end = strrchr(p, '"');
            if (end != NULL && end > p) {
                m->message = ml_xstrndup(p, (size_t)(end - p));
            }
        }
        if (m->message == NULL) {
            m->message = ml_xstrdup(skip_spaces(comma + 1));
        }
    } else {
        expr = ml_xstrdup(args);
    }

    p = expr;
    lhs_str = take_term(&p);
    op_str = take_op(&p);
    p = skip_spaces(p);
    rhs_str = ml_xstrdup(p);

    if (lhs_str == NULL || op_str == NULL || rhs_str[0] == '\0') {
        ml_error(ctx, "%s: ASSERT expects TERM OP TERM: '%s'", obj->name, expr);
        goto out;
    }
    if (parse_op(op_str, &m->op) != 0) {
        ml_error(ctx, "%s: ASSERT: unknown operator '%s'", obj->name, op_str);
        goto out;
    }
    if (parse_expr(lhs_str, &m->lhs) != 0) {
        ml_error(ctx, "%s: ASSERT: bad lhs '%s'", obj->name, lhs_str);
        goto out;
    }
    if (parse_expr(rhs_str, &m->rhs) != 0) {
        ml_error(ctx, "%s: ASSERT: bad rhs '%s'", obj->name, rhs_str);
        goto out;
    }
    rc = 0;
out:
    free(expr);
    free(lhs_str);
    free(op_str);
    free(rhs_str);
    return rc;
}

static int parse_macro_line(ml_context_t *ctx, ml_object_t *obj, const char *line) {
    const char *p = skip_spaces(line);
    char *kw;
    const char *rest;
    ml_macro_t *m;
    int rc;

    if (*p == '\0' || *p == '#') {
        return 0;
    }
    kw = take_word(&p);
    if (kw == NULL) {
        return 0;
    }
    rest = skip_spaces(p);

    m = append_macro(ctx);
    m->origin = obj;

    if (strcmp(kw, "PROVIDE") == 0) {
        m->kind = ML_MACRO_PROVIDE;
        rc = parse_name_eq_value(ctx, obj, rest, &m->name, &m->value);
    } else if (strcmp(kw, "DEFINE") == 0) {
        m->kind = ML_MACRO_DEFINE;
        rc = parse_name_eq_value(ctx, obj, rest, &m->name, &m->value);
    } else if (strcmp(kw, "ENTRY") == 0) {
        m->kind = ML_MACRO_ENTRY;
        m->name = ml_xstrdup(rest);
        rc = (m->name[0] == '\0') ? 1 : 0;
        if (rc) ml_error(ctx, "%s: ENTRY needs a symbol name", obj->name);
    } else if (strcmp(kw, "BASE") == 0) {
        m->kind = ML_MACRO_BASE;
        rc = ml_parse_u64(rest, &m->value);
        if (rc) ml_error(ctx, "%s: BASE: invalid address '%s'", obj->name, rest);
    } else if (strcmp(kw, "STACK_EXEC") == 0) {
        m->kind = ML_MACRO_STACK_EXEC;
        rc = 0;
    } else if (strcmp(kw, "KEEP") == 0) {
        m->kind = ML_MACRO_KEEP;
        m->name = ml_xstrdup(rest);
        rc = (m->name[0] == '\0') ? 1 : 0;
        if (rc) ml_error(ctx, "%s: KEEP needs a section name", obj->name);
    } else if (strcmp(kw, "ASSERT") == 0) {
        m->kind = ML_MACRO_ASSERT;
        rc = parse_assert(ctx, obj, rest, m);
    } else {
        ml_error(ctx, "%s: unknown macro '%s'", obj->name, kw);
        rc = 1;
    }

    free(kw);
    if (rc != 0) {
        ctx->macro_count--;  /* drop the failed entry */
    }
    return rc;
}

int ml_macros_add_from_object(ml_context_t *ctx, ml_object_t *obj,
                              const unsigned char *data, size_t size) {
    size_t i = 0;
    int saw_error = 0;

    while (i < size) {
        const char *line = (const char *)(data + i);
        size_t len = 0;
        while (i + len < size && line[len] != '\0' && line[len] != '\n') len++;

        if (len > 0) {
            char *copy = ml_xstrndup(line, len);
            if (parse_macro_line(ctx, obj, copy) != 0) {
                saw_error = 1;
            }
            free(copy);
        }
        i += len;
        while (i < size && (data[i] == '\0' || data[i] == '\n')) i++;
    }
    return saw_error ? 1 : 0;
}

void ml_macros_apply_pre_layout(ml_context_t *ctx) {
    const char *entry_origin = NULL;
    const char *base_origin = NULL;

    for (size_t i = 0; i < ctx->macro_count; i++) {
        ml_macro_t *m = &ctx->macros[i];
        const char *src = m->origin ? m->origin->name : "<input>";

        if (m->kind == ML_MACRO_ENTRY) {
            if (ctx->entry_from_cli) {
                ml_verbose(ctx, "%s: ENTRY %s ignored (CLI override)", src, m->name);
                continue;
            }
            if (entry_origin != NULL && strcmp(ctx->entry_name, m->name) != 0) {
                ml_error(ctx, "%s: ENTRY %s conflicts with %s from %s",
                         src, m->name, ctx->entry_name, entry_origin);
                continue;
            }
            ctx->entry_name = m->name;
            entry_origin = src;
        } else if (m->kind == ML_MACRO_BASE) {
            if (ctx->base_from_cli) {
                ml_verbose(ctx, "%s: BASE 0x%llx ignored (CLI override)",
                           src, (unsigned long long)m->value);
                continue;
            }
            if ((m->value & (ML_PAGE_SIZE - 1)) != 0) {
                ml_error(ctx, "%s: BASE 0x%llx must be page aligned",
                         src, (unsigned long long)m->value);
                continue;
            }
            if (base_origin != NULL && ctx->base_addr != m->value) {
                ml_error(ctx, "%s: BASE 0x%llx conflicts with 0x%llx from %s",
                         src, (unsigned long long)m->value,
                         (unsigned long long)ctx->base_addr, base_origin);
                continue;
            }
            ctx->base_addr = m->value;
            base_origin = src;
        } else if (m->kind == ML_MACRO_STACK_EXEC) {
            ctx->stack_exec = 1;
            ml_verbose(ctx, "%s: STACK_EXEC", src);
        } else if (m->kind == ML_MACRO_KEEP) {
            ml_verbose(ctx, "%s: KEEP %s (informational)", src, m->name);
        }
    }
}

int ml_macros_inject_symbols(ml_context_t *ctx) {
    for (size_t i = 0; i < ctx->macro_count; i++) {
        ml_macro_t *m = &ctx->macros[i];
        ml_global_t *g;
        const char *src = m->origin ? m->origin->name : "<input>";

        if (m->kind != ML_MACRO_PROVIDE && m->kind != ML_MACRO_DEFINE) {
            continue;
        }

        g = ml_find_global(ctx, m->name);
        if (g != NULL && g->defined) {
            if (m->kind == ML_MACRO_DEFINE) {
                ml_error(ctx, "%s: DEFINE %s conflicts with definition in %s",
                         src, m->name,
                         g->object ? g->object->name : "<input>");
                return 1;
            }
            ml_verbose(ctx, "%s: PROVIDE %s skipped (already defined)", src, m->name);
            continue;
        }
        if (g == NULL) {
            if (ctx->global_count == ctx->global_cap) {
                size_t next = ctx->global_cap == 0 ? 64 : ctx->global_cap * 2;
                ctx->globals = ml_xrealloc(ctx->globals,
                                           next * sizeof(ctx->globals[0]));
                memset(ctx->globals + ctx->global_cap, 0,
                       (next - ctx->global_cap) * sizeof(ctx->globals[0]));
                ctx->global_cap = next;
            }
            g = &ctx->globals[ctx->global_count++];
            memset(g, 0, sizeof(*g));
            g->name = m->name;
        }
        g->defined = 1;
        g->absolute = 1;
        g->common = 0;
        g->weak = (m->kind == ML_MACRO_PROVIDE);
        g->object = NULL;
        g->symbol = NULL;
        g->value = m->value;
        g->size = 0;
        ml_verbose(ctx, "%s: %s %s = 0x%llx",
                   src,
                   m->kind == ML_MACRO_PROVIDE ? "PROVIDE" : "DEFINE",
                   m->name, (unsigned long long)m->value);
    }
    return 0;
}

static int resolve_term(ml_context_t *ctx, const ml_macro_term_t *t,
                        const char *src, uint64_t *out) {
    uint64_t total = 0;
    for (size_t i = 0; i < t->count; i++) {
        const ml_macro_term_part_t *p = &t->parts[i];
        uint64_t v;
        if (p->is_symbol) {
            ml_global_t *g = ml_find_global(ctx, p->name);
            if (g == NULL || (!g->defined && !g->common)) {
                ml_error(ctx, "%s: ASSERT references undefined symbol %s",
                         src, p->name);
                return 1;
            }
            v = g->value;
        } else {
            v = p->value;
        }
        if (p->sign > 0) total += v;
        else total -= v;
    }
    *out = total;
    return 0;
}

int ml_macros_apply_post_layout(ml_context_t *ctx) {
    int failures = 0;
    for (size_t i = 0; i < ctx->macro_count; i++) {
        ml_macro_t *m = &ctx->macros[i];
        const char *src = m->origin ? m->origin->name : "<input>";
        uint64_t l, r;

        if (m->kind != ML_MACRO_ASSERT) {
            continue;
        }
        if (resolve_term(ctx, &m->lhs, src, &l) != 0 ||
            resolve_term(ctx, &m->rhs, src, &r) != 0) {
            failures++;
            continue;
        }
        if (!eval_op(l, m->op, r)) {
            ml_error(ctx, "%s: ASSERT failed: 0x%llx %s 0x%llx%s%s",
                     src, (unsigned long long)l, op_name(m->op),
                     (unsigned long long)r,
                     m->message ? ": " : "",
                     m->message ? m->message : "");
            failures++;
        } else {
            ml_verbose(ctx, "%s: ASSERT ok (0x%llx %s 0x%llx)",
                       src, (unsigned long long)l, op_name(m->op),
                       (unsigned long long)r);
        }
    }
    return failures == 0 ? 0 : 1;
}
