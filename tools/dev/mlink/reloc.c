#include "mlink.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static int checked_u32(uint64_t v, uint32_t *out) {
    if (v > UINT32_MAX) {
        return 1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int checked_i32(int64_t v, int32_t *out) {
    if (v < INT32_MIN || v > INT32_MAX) {
        return 1;
    }
    *out = (int32_t)v;
    return 0;
}

static int get_symbol_value(ml_context_t *ctx, ml_object_t *obj,
                            uint32_t symbol_index, uint64_t *value_out) {
    ml_symbol_t *sym;

    if (symbol_index >= obj->symbol_count) {
        ml_error(ctx, "%s: relocation uses bad symbol index %u",
                 obj->name, symbol_index);
        return 1;
    }

    sym = &obj->symbols[symbol_index];
    if (sym->shndx == SHN_ABS) {
        *value_out = sym->value;
        return 0;
    }
    if (sym->shndx != SHN_UNDEF && sym->shndx != SHN_COMMON) {
        if (sym->shndx >= obj->section_count ||
            obj->sections[sym->shndx].kind == ML_SEC_SKIP) {
            ml_error(ctx, "%s: symbol %s points to unsupported section",
                     obj->name, sym->name);
            return 1;
        }
        *value_out = obj->sections[sym->shndx].out_addr + sym->value;
        return 0;
    }

    if (sym->name != NULL && sym->name[0] != '\0') {
        ml_global_t *g = ml_find_global(ctx, sym->name);
        if (g != NULL && (g->defined || g->common)) {
            *value_out = g->value;
            return 0;
        }
        if (sym->bind == STB_WEAK) {
            *value_out = 0;
            return 0;
        }
    }

    ml_error(ctx, "%s: unresolved relocation symbol %s",
             obj->name, sym->name != NULL ? sym->name : "<bad>");
    return 1;
}

static int read_implicit_addend(ml_section_t *sec, ml_reloc_t *rel, int64_t *out) {
    unsigned char *p;

    if (rel->offset >= sec->file_size) {
        return 1;
    }
    p = sec->image + rel->offset;
    switch (rel->type) {
    case R_X86_64_NONE:
        *out = 0;
        return 0;
    case R_X86_64_64:
        if (rel->offset + 8 > sec->file_size) {
            return 1;
        }
        *out = (int64_t)ml_get64(p);
        return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
    case R_X86_64_32:
    case R_X86_64_32S:
        if (rel->offset + 4 > sec->file_size) {
            return 1;
        }
        *out = (int32_t)ml_get32(p);
        return 0;
    default:
        return 0;
    }
}

static int patch_reloc(ml_context_t *ctx, ml_object_t *obj,
                       ml_section_t *sec, ml_reloc_t *rel) {
    uint64_t s;
    uint64_t p_addr;
    int64_t addend;
    unsigned char *where;

    if (rel->type == R_X86_64_NONE) {
        return 0;
    }
    if (rel->offset >= sec->file_size) {
        ml_error(ctx, "%s: relocation offset outside %s", obj->name, sec->name);
        return 1;
    }

    if (get_symbol_value(ctx, obj, rel->symbol_index, &s) != 0) {
        return 1;
    }
    if (rel->has_addend) {
        addend = rel->addend;
    } else if (read_implicit_addend(sec, rel, &addend) != 0) {
        ml_error(ctx, "%s: relocation addend outside %s", obj->name, sec->name);
        return 1;
    }

    p_addr = sec->out_addr + rel->offset;
    where = sec->image + rel->offset;

    switch (rel->type) {
    case R_X86_64_64: {
        ml_put64(where, s + (uint64_t)addend);
        return 0;
    }
    case R_X86_64_PC32:
    case R_X86_64_PLT32: {
        int64_t v = (int64_t)s + addend - (int64_t)p_addr;
        int32_t out;
        if (checked_i32(v, &out) != 0) {
            ml_error(ctx, "%s: %s overflow in %s",
                     obj->name, ml_reloc_name(rel->type), sec->name);
            return 1;
        }
        ml_put32(where, (uint32_t)out);
        return 0;
    }
    case R_X86_64_32: {
        uint32_t out;
        int64_t v = (int64_t)s + addend;
        if (v < 0) {
            ml_error(ctx, "%s: R_X86_64_32 underflow in %s", obj->name, sec->name);
            return 1;
        }
        if (checked_u32((uint64_t)v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_32 overflow in %s", obj->name, sec->name);
            return 1;
        }
        ml_put32(where, out);
        return 0;
    }
    case R_X86_64_32S: {
        int64_t v = (int64_t)s + addend;
        int32_t out;
        if (checked_i32(v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_32S overflow in %s", obj->name, sec->name);
            return 1;
        }
        ml_put32(where, (uint32_t)out);
        return 0;
    }
    default:
        ml_error(ctx, "%s: unsupported relocation %s(%u) in %s",
                 obj->name, ml_reloc_name(rel->type), rel->type, sec->name);
        return 1;
    }
}

int ml_apply_relocations(ml_context_t *ctx) {
    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t r = 0; r < obj->reloc_count; r++) {
            ml_reloc_t *rel = &obj->relocs[r];
            ml_section_t *sec;

            if (rel->target_section >= obj->section_count) {
                ml_error(ctx, "%s: relocation uses bad section index %u",
                         obj->name, rel->target_section);
                continue;
            }
            sec = &obj->sections[rel->target_section];
            if (sec->kind == ML_SEC_SKIP) {
                continue;
            }
            if (sec->type == SHT_NOBITS) {
                ml_error(ctx, "%s: relocation targets unsupported section %s",
                         obj->name, sec->name);
                continue;
            }
            patch_reloc(ctx, obj, sec, rel);
        }
    }
    return ctx->error_count != 0;
}
