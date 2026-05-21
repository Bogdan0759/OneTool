#include "mlink.h"

#include <stdlib.h>
#include <string.h>

static uint64_t common_cursor_start(ml_context_t *ctx) {
    uint64_t end = ctx->data_offset + ctx->data_memsz;
    return ml_align_up(end, 16);
}

static int local_symbol_addr(ml_object_t *obj, ml_symbol_t *sym, uint64_t *out) {
    if (sym->shndx == SHN_ABS) {
        *out = sym->value;
        return 0;
    }
    if (sym->shndx == SHN_COMMON || sym->undefined ||
        sym->shndx >= obj->section_count) {
        return 1;
    }
    if (obj->sections[sym->shndx].kind == ML_SEC_SKIP) {
        return 1;
    }
    *out = obj->sections[sym->shndx].out_addr + sym->value;
    return 0;
}

static int compute_global_values(ml_context_t *ctx) {
    uint64_t common_cursor = common_cursor_start(ctx);
    uint64_t common_start = common_cursor;

    for (size_t i = 0; i < ctx->global_count; i++) {
        ml_global_t *g = &ctx->globals[i];
        if (g->defined) {
            if (g->absolute) {
                g->value = g->symbol->value;
            } else if (local_symbol_addr(g->object, g->symbol, &g->value) != 0) {
                ml_error(ctx, "symbol %s points to unsupported section", g->name);
            }
            continue;
        }

        if (g->common) {
            uint64_t align = g->common_align == 0 ? 1 : g->common_align;
            if (align > 4096) {
                align = 4096;
            }
            common_cursor = ml_align_up(common_cursor, align);
            g->common_addr = ctx->base_addr + common_cursor;
            g->value = g->common_addr;
            common_cursor += g->size;
        }
    }

    if (common_cursor > common_start) {
        uint64_t common_end = common_cursor - ctx->data_offset;
        if (common_end > ctx->data_memsz) {
            ctx->data_memsz = common_end;
        }
        ctx->has_rw_segment = 1;
    }

    return ctx->error_count != 0;
}

int ml_layout(ml_context_t *ctx) {
    uint64_t header_size;
    uint64_t rx_cursor;
    uint64_t ro_cursor;
    uint64_t data_cursor;
    uint64_t bss_cursor;
    int has_ro = 0;
    int has_data = 0;
    int has_bss = 0;
    int has_common = 0;
    int has_rw = 0;

    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->section_count; j++) {
            if (obj->sections[j].size == 0) {
                continue;
            }
            switch (obj->sections[j].kind) {
            case ML_SEC_RODATA:
                has_ro = 1;
                break;
            case ML_SEC_DATA:
                has_data = 1;
                break;
            case ML_SEC_BSS:
                has_bss = 1;
                break;
            default:
                break;
            }
        }
    }

    for (size_t i = 0; i < ctx->global_count; i++) {
        if (ctx->globals[i].common) {
            has_common = 1;
            break;
        }
    }

    has_rw = has_data || has_bss || has_common;
    ctx->has_rodata_segment = has_ro;
    ctx->has_rw_segment = has_rw;
    ctx->phnum = 1 + (has_ro ? 1 : 0) + (has_rw ? 1 : 0);

    header_size = sizeof(Elf64_Ehdr) + (uint64_t)ctx->phnum * sizeof(Elf64_Phdr);
    rx_cursor = ml_align_up(header_size, 16);

    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->section_count; j++) {
            ml_section_t *s = &obj->sections[j];
            if (s->kind != ML_SEC_TEXT || s->size == 0) {
                continue;
            }
            rx_cursor = ml_align_up(rx_cursor, s->align);
            s->out_offset = rx_cursor;
            s->out_addr = ctx->base_addr + rx_cursor;
            rx_cursor += s->size;
        }
    }

    ctx->rx_filesz = rx_cursor;
    ro_cursor = has_ro ? ml_align_up(rx_cursor, ML_PAGE_SIZE) : rx_cursor;
    ctx->ro_offset = ro_cursor;

    if (has_ro) {
        for (size_t i = 0; i < ctx->object_count; i++) {
            ml_object_t *obj = ctx->objects[i];
            if (!obj->selected) {
                continue;
            }
            for (size_t j = 0; j < obj->section_count; j++) {
                ml_section_t *s = &obj->sections[j];
                if (s->kind != ML_SEC_RODATA || s->size == 0) {
                    continue;
                }
                ro_cursor = ml_align_up(ro_cursor, s->align);
                s->out_offset = ro_cursor;
                s->out_addr = ctx->base_addr + ro_cursor;
                ro_cursor += s->size;
            }
        }
        ctx->ro_filesz = ro_cursor - ctx->ro_offset;
    }

    data_cursor = has_rw ? ml_align_up(ro_cursor, ML_PAGE_SIZE) : ro_cursor;
    ctx->data_offset = data_cursor;
    bss_cursor = data_cursor;

    if (has_rw) {
        for (size_t i = 0; i < ctx->object_count; i++) {
            ml_object_t *obj = ctx->objects[i];
            if (!obj->selected) {
                continue;
            }
            for (size_t j = 0; j < obj->section_count; j++) {
                ml_section_t *s = &obj->sections[j];
                if (s->kind != ML_SEC_DATA || s->size == 0) {
                    continue;
                }
                data_cursor = ml_align_up(data_cursor, s->align);
                s->out_offset = data_cursor;
                s->out_addr = ctx->base_addr + data_cursor;
                data_cursor += s->size;
            }
        }
        ctx->data_filesz = data_cursor - ctx->data_offset;
        bss_cursor = data_cursor;

        for (size_t i = 0; i < ctx->object_count; i++) {
            ml_object_t *obj = ctx->objects[i];
            if (!obj->selected) {
                continue;
            }
            for (size_t j = 0; j < obj->section_count; j++) {
                ml_section_t *s = &obj->sections[j];
                if (s->kind != ML_SEC_BSS || s->size == 0) {
                    continue;
                }
                bss_cursor = ml_align_up(bss_cursor, s->align);
                s->out_offset = bss_cursor;
                s->out_addr = ctx->base_addr + bss_cursor;
                bss_cursor += s->size;
            }
        }
        ctx->data_memsz = bss_cursor - ctx->data_offset;
    }

    if (compute_global_values(ctx) != 0) {
        return 1;
    }

    ctx->final_file_size = ctx->has_rw_segment ?
        ctx->data_offset + ctx->data_filesz :
        (ctx->has_rodata_segment ? ctx->ro_offset + ctx->ro_filesz : ctx->rx_filesz);

    ml_global_t *entry = ml_find_global(ctx, ctx->entry_name);
    if (entry == NULL || (!entry->defined && !entry->common)) {
        ml_error(ctx, "entry symbol not found: %s", ctx->entry_name);
        return 1;
    }
    ctx->entry_addr = entry->value;

    return ctx->error_count != 0;
}
