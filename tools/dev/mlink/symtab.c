#include "mlink.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int is_global_symbol(const ml_symbol_t *sym) {
    return sym->bind == STB_GLOBAL || sym->bind == STB_WEAK;
}

static int is_weak_symbol(const ml_symbol_t *sym) {
    return sym->bind == STB_WEAK;
}

static int symbol_has_name(const ml_symbol_t *sym) {
    return sym->name != NULL && sym->name[0] != '\0';
}

static ml_global_t *intern_global(ml_context_t *ctx, const char *name) {
    ml_global_t *g = ml_find_global(ctx, name);
    if (g != NULL) {
        return g;
    }

    if (ctx->global_count == ctx->global_cap) {
        size_t next = ctx->global_cap == 0 ? 64 : ctx->global_cap * 2;
        ctx->globals = ml_xrealloc(ctx->globals, next * sizeof(ctx->globals[0]));
        memset(ctx->globals + ctx->global_cap, 0, (next - ctx->global_cap) * sizeof(ctx->globals[0]));
        ctx->global_cap = next;
    }

    g = &ctx->globals[ctx->global_count++];
    memset(g, 0, sizeof(*g));
    g->name = name;
    return g;
}

ml_global_t *ml_find_global(ml_context_t *ctx, const char *name) {
    for (size_t i = 0; i < ctx->global_count; i++) {
        if (strcmp(ctx->globals[i].name, name) == 0) {
            return &ctx->globals[i];
        }
    }
    return NULL;
}

static int selected_objects_define(ml_context_t *ctx, const char *name) {
    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->symbol_count; j++) {
            ml_symbol_t *sym = &obj->symbols[j];
            if (!symbol_has_name(sym) || sym->undefined || sym->common || !is_global_symbol(sym)) {
                continue;
            }
            if (strcmp(sym->name, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int selected_objects_reference(ml_context_t *ctx, const char *name) {
    if (selected_objects_define(ctx, name)) {
        return 0;
    }

    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->symbol_count; j++) {
            ml_symbol_t *sym = &obj->symbols[j];
            if (symbol_has_name(sym) && sym->undefined && is_global_symbol(sym) &&
                !is_weak_symbol(sym) &&
                strcmp(sym->name, name) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int archive_member_needed(ml_context_t *ctx, ml_object_t *obj) {
    for (size_t i = 0; i < obj->symbol_count; i++) {
        ml_symbol_t *sym = &obj->symbols[i];
        if (!symbol_has_name(sym) || sym->undefined || sym->common || !is_global_symbol(sym)) {
            continue;
        }
        if (selected_objects_reference(ctx, sym->name)) {
            return 1;
        }
    }
    return 0;
}

static void select_archive_members(ml_context_t *ctx) {
    int changed = 1;

    while (changed) {
        changed = 0;
        for (size_t i = 0; i < ctx->object_count; i++) {
            ml_object_t *obj = ctx->objects[i];
            if (obj->selected || !obj->from_archive) {
                continue;
            }
            if (archive_member_needed(ctx, obj)) {
                obj->selected = 1;
                changed = 1;
                ml_verbose(ctx, "selected %s", obj->name);
            }
        }
    }
}

static int define_global(ml_context_t *ctx, ml_object_t *obj, ml_symbol_t *sym) {
    ml_global_t *g = intern_global(ctx, sym->name);
    int weak = is_weak_symbol(sym);

    if (sym->common) {
        if (!g->defined && (!g->common || sym->size > g->size)) {
            g->common = 1;
            g->weak = weak;
            g->object = obj;
            g->symbol = sym;
            g->size = sym->size;
            g->common_align = sym->value == 0 ? 1 : sym->value;
        }
        return 0;
    }

    if (g->defined) {
        if (g->weak && !weak) {
            /* strong beats weak */
        } else if (!g->weak || !weak) {
            ml_error(ctx, "duplicate symbol %s in %s and %s", sym->name,
                     g->object ? g->object->name : "<absolute>", obj->name);
            return 1;
        } else {
            return 0;
        }
    }

    g->defined = 1;
    g->common = 0;
    g->weak = weak;
    g->absolute = sym->shndx == SHN_ABS;
    g->object = obj;
    g->symbol = sym;
    g->size = sym->size;
    return 0;
}

static void record_undefined(ml_context_t *ctx, ml_symbol_t *sym) {
    ml_global_t *g;

    if (!symbol_has_name(sym) || !is_global_symbol(sym)) {
        return;
    }
    g = intern_global(ctx, sym->name);
    g->referenced = 1;
    if (!is_weak_symbol(sym)) {
        g->strong_ref = 1;
    }
}

int ml_resolve_symbols(ml_context_t *ctx) {
    select_archive_members(ctx);

    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->symbol_count; j++) {
            ml_symbol_t *sym = &obj->symbols[j];
            if (!symbol_has_name(sym) || !is_global_symbol(sym)) {
                continue;
            }
            if (sym->undefined) {
                record_undefined(ctx, sym);
            } else if (define_global(ctx, obj, sym) != 0) {
                return 1;
            }
        }
    }

    for (size_t i = 0; i < ctx->global_count; i++) {
        ml_global_t *g = &ctx->globals[i];
        if (g->referenced && g->strong_ref && !g->defined && !g->common) {
            ml_error(ctx, "undefined symbol: %s", g->name);
        }
    }

    return ctx->error_count != 0;
}
