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

static int checked_u16(uint64_t v, uint16_t *out) {
    if (v > UINT16_MAX) {
        return 1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int checked_u8(uint64_t v, unsigned char *out) {
    if (v > UINT8_MAX) {
        return 1;
    }
    *out = (unsigned char)v;
    return 0;
}

static int checked_i32(int64_t v, int32_t *out) {
    if (v < INT32_MIN || v > INT32_MAX) {
        return 1;
    }
    *out = (int32_t)v;
    return 0;
}

static int checked_i16(int64_t v, int16_t *out) {
    if (v < INT16_MIN || v > INT16_MAX) {
        return 1;
    }
    *out = (int16_t)v;
    return 0;
}

static int checked_i8(int64_t v, int8_t *out) {
    if (v < INT8_MIN || v > INT8_MAX) {
        return 1;
    }
    *out = (int8_t)v;
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

static int get_symbol_size(ml_context_t *ctx, ml_object_t *obj,
                           uint32_t symbol_index, uint64_t *size_out) {
    ml_symbol_t *sym;

    if (symbol_index >= obj->symbol_count) {
        ml_error(ctx, "%s: relocation uses bad symbol index %u",
                 obj->name, symbol_index);
        return 1;
    }

    sym = &obj->symbols[symbol_index];
    if (sym->name != NULL && sym->name[0] != '\0') {
        ml_global_t *g = ml_find_global(ctx, sym->name);
        if (g != NULL && (g->defined || g->common)) {
            *size_out = g->size;
            return 0;
        }
    }

    *size_out = sym->size;
    return 0;
}

static size_t reloc_width(uint32_t type) {
    switch (type) {
    case R_X86_64_NONE:
        return 0;
    case R_X86_64_64:
    case R_X86_64_PC64:
    case R_X86_64_SIZE64:
        return 8;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
    case R_X86_64_32:
    case R_X86_64_32S:
    case R_X86_64_SIZE32:
    case R_X86_64_GOTPCREL:
#ifdef R_X86_64_GOTPCRELX
    case R_X86_64_GOTPCRELX:
#endif
#ifdef R_X86_64_REX_GOTPCRELX
    case R_X86_64_REX_GOTPCRELX:
#endif
        return 4;
    case R_X86_64_16:
    case R_X86_64_PC16:
        return 2;
    case R_X86_64_8:
    case R_X86_64_PC8:
        return 1;
    default:
        return 0;
    }
}

static int relax_gotpcrel(ml_context_t *ctx, ml_object_t *obj,
                          ml_section_t *sec, ml_reloc_t *rel,
                          uint64_t s, int64_t addend) {
    unsigned char *where = sec->image + rel->offset;
    int64_t v;
    int32_t out;

    if (rel->offset < 2) {
        ml_error(ctx, "%s: cannot relax %s in %s (no instruction prefix)",
                 obj->name, ml_reloc_name(rel->type), sec->name);
        return 1;
    }

    if (where[-2] == 0x8b) {
        where[-2] = 0x8d;
    } else if (where[-2] == 0xff && where[-1] == 0x15) {
        where[-2] = 0x67;
        where[-1] = 0xe8;
    } else if (where[-2] == 0xff && where[-1] == 0x25) {
        where[-2] = 0x67;
        where[-1] = 0xe9;
    } else {
        ml_error(ctx, "%s: cannot relax %s in %s (opcode %02x %02x)",
                 obj->name, ml_reloc_name(rel->type), sec->name,
                 where[-2], where[-1]);
        return 1;
    }

    v = (int64_t)s + addend - (int64_t)(sec->out_addr + rel->offset);
    if (checked_i32(v, &out) != 0) {
        ml_error(ctx, "%s: %s relax overflow in %s",
                 obj->name, ml_reloc_name(rel->type), sec->name);
        return 1;
    }
    ml_put32(where, (uint32_t)out);
    return 0;
}

static int reloc_fits_section(const ml_section_t *sec, const ml_reloc_t *rel,
                              size_t width) {
    return rel->offset <= sec->file_size && width <= sec->file_size - rel->offset;
}

static int read_implicit_addend(ml_section_t *sec, ml_reloc_t *rel, int64_t *out) {
    unsigned char *p;
    size_t width;

    if (rel->offset > sec->file_size) {
        return 1;
    }
    width = reloc_width(rel->type);
    if (width == 0) {
        *out = 0;
        return 0;
    }
    if (!reloc_fits_section(sec, rel, width)) {
        return 1;
    }

    p = sec->image + rel->offset;
    if (width == 8) {
        if (rel->offset + 8 > sec->file_size) {
            return 1;
        }
        *out = (int64_t)ml_get64(p);
        return 0;
    }
    if (width == 4) {
        *out = (int32_t)ml_get32(p);
        return 0;
    }
    if (width == 2) {
        *out = (int16_t)ml_get16(p);
        return 0;
    }
    *out = (int8_t)p[0];
    return 0;
}

static int patch_reloc(ml_context_t *ctx, ml_object_t *obj,
                       ml_section_t *sec, ml_reloc_t *rel) {
    uint64_t s;
    uint64_t z;
    uint64_t p_addr;
    int64_t addend;
    unsigned char *where;
    size_t width;

    if (rel->type == R_X86_64_NONE) {
        return 0;
    }
    if (rel->offset > sec->file_size) {
        ml_error(ctx, "%s: relocation offset outside %s", obj->name, sec->name);
        return 1;
    }
    width = reloc_width(rel->type);
    if (width == 0) {
        ml_error(ctx, "%s: unsupported %s(%u) in %s",
                 obj->name, ml_reloc_name(rel->type), rel->type, sec->name);
        return 1;
    }
    if (!reloc_fits_section(sec, rel, width)) {
        ml_error(ctx, "%s: relocation field outside %s", obj->name, sec->name);
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
    case R_X86_64_PC64: {
        ml_put64(where, s + (uint64_t)addend - p_addr);
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
    case R_X86_64_PC16: {
        int64_t v = (int64_t)s + addend - (int64_t)p_addr;
        int16_t out;
        if (checked_i16(v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_PC16 overflow in %s", obj->name, sec->name);
            return 1;
        }
        ml_put16(where, (uint16_t)out);
        return 0;
    }
    case R_X86_64_PC8: {
        int64_t v = (int64_t)s + addend - (int64_t)p_addr;
        int8_t out;
        if (checked_i8(v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_PC8 overflow in %s", obj->name, sec->name);
            return 1;
        }
        where[0] = (unsigned char)out;
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
    case R_X86_64_16: {
        uint16_t out;
        int64_t v = (int64_t)s + addend;
        if (v < 0 || checked_u16((uint64_t)v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_16 overflow in %s", obj->name, sec->name);
            return 1;
        }
        ml_put16(where, out);
        return 0;
    }
    case R_X86_64_8: {
        unsigned char out;
        int64_t v = (int64_t)s + addend;
        if (v < 0 || checked_u8((uint64_t)v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_8 overflow in %s", obj->name, sec->name);
            return 1;
        }
        where[0] = out;
        return 0;
    }
    case R_X86_64_SIZE32: {
        uint32_t out;
        int64_t v;
        if (get_symbol_size(ctx, obj, rel->symbol_index, &z) != 0) {
            return 1;
        }
        v = (int64_t)z + addend;
        if (v < 0 || checked_u32((uint64_t)v, &out) != 0) {
            ml_error(ctx, "%s: R_X86_64_SIZE32 overflow in %s", obj->name, sec->name);
            return 1;
        }
        ml_put32(where, out);
        return 0;
    }
    case R_X86_64_SIZE64:
        if (get_symbol_size(ctx, obj, rel->symbol_index, &z) != 0) {
            return 1;
        }
        ml_put64(where, z + (uint64_t)addend);
        return 0;
    case R_X86_64_GOTPCREL:
#ifdef R_X86_64_GOTPCRELX
    case R_X86_64_GOTPCRELX:
#endif
#ifdef R_X86_64_REX_GOTPCRELX
    case R_X86_64_REX_GOTPCRELX:
#endif
        return relax_gotpcrel(ctx, obj, sec, rel, s, addend);
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
