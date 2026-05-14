#include "mlink.h"
#include "../../../libs/elf/elf.h"

#include <stdlib.h>
#include <string.h>

static char *dup_strtab_name(const unsigned char *base, size_t size, uint32_t off) {
    char *name = ot_elf_strdup_from_strtab(base, size, off);
    return name == NULL ? ml_xstrdup("<badstr>") : name;
}

static ml_section_kind_t classify_section(uint32_t type, uint64_t flags, const char *name) {
    if ((flags & SHF_ALLOC) == 0) {
        return ML_SEC_SKIP;
    }
    if ((flags & SHF_EXECINSTR) != 0) {
        return ML_SEC_TEXT;
    }
    if ((flags & SHF_WRITE) != 0) {
        if (type == SHT_NOBITS) {
            return ML_SEC_BSS;
        }
        return ML_SEC_DATA;
    }
    if (strcmp(name, ".eh_frame") == 0 || strcmp(name, ".comment") == 0 ||
        strcmp(name, ".note.GNU-stack") == 0) {
        return ML_SEC_SKIP;
    }
    return ML_SEC_RODATA;
}

static int load_sections(ml_context_t *ctx, ml_object_t *obj, const ot_elf_file_t *elf) {
    obj->section_count = elf->section_count;
    obj->sections = ml_xcalloc(elf->section_count, sizeof(obj->sections[0]));

    for (size_t i = 0; i < elf->section_count; i++) {
        const ot_elf_section_t *shdr = &elf->sections[i];
        ml_section_t *s = &obj->sections[i];

        s->name = ml_xstrdup(shdr->name);
        s->type = shdr->type;
        s->flags = shdr->flags;
        s->align = shdr->addralign == 0 ? 1 : shdr->addralign;
        s->size = shdr->size;
        s->kind = classify_section(s->type, s->flags, s->name);

        if (s->kind == ML_SEC_SKIP || s->type == SHT_NOBITS || s->size == 0) {
            continue;
        }
        if (!ot_elf_range_ok(obj->size, shdr->offset, shdr->size)) {
            ml_error(ctx, "%s: section %s is outside file", obj->name, s->name);
            return 1;
        }
        s->image = ml_xmalloc((size_t)s->size);
        memcpy(s->image, obj->data + shdr->offset, (size_t)s->size);
        s->file_size = s->size;
    }

    return 0;
}

static int load_symbols(ml_context_t *ctx, ml_object_t *obj, const ot_elf_file_t *elf) {
    for (size_t sec_i = 0; sec_i < elf->section_count; sec_i++) {
        const ot_elf_section_t *symsec = &elf->sections[sec_i];
        const unsigned char *strtab;
        size_t strtab_size;
        size_t count;

        if (symsec->type != SHT_SYMTAB) {
            continue;
        }
        if (symsec->entsize != 24 || symsec->link >= elf->section_count ||
            !ot_elf_range_ok(obj->size, symsec->offset, symsec->size)) {
            ml_error(ctx, "%s: invalid symbol table", obj->name);
            return 1;
        }
        if (!ot_elf_range_ok(obj->size, elf->sections[symsec->link].offset,
                             elf->sections[symsec->link].size)) {
            ml_error(ctx, "%s: invalid symbol string table", obj->name);
            return 1;
        }

        strtab = obj->data + elf->sections[symsec->link].offset;
        strtab_size = (size_t)elf->sections[symsec->link].size;
        count = (size_t)(symsec->size / symsec->entsize);
        obj->symbols = ml_xcalloc(count, sizeof(obj->symbols[0]));
        obj->symbol_count = count;

        for (size_t i = 0; i < count; i++) {
            const unsigned char *p = obj->data + symsec->offset + i * symsec->entsize;
            uint32_t st_name = ml_get32(p);
            unsigned char info = p[4];
            uint16_t shndx = ml_get16(p + 6);
            ml_symbol_t *sym = &obj->symbols[i];

            sym->name = dup_strtab_name(strtab, strtab_size, st_name);
            sym->bind = ELF64_ST_BIND(info);
            sym->type = ELF64_ST_TYPE(info);
            sym->shndx = shndx;
            sym->value = ml_get64(p + 8);
            sym->size = ml_get64(p + 16);
            sym->undefined = shndx == SHN_UNDEF;
            sym->common = shndx == SHN_COMMON;
        }
        return 0;
    }

    obj->symbols = ml_xcalloc(1, sizeof(obj->symbols[0]));
    obj->symbol_count = 1;
    obj->symbols[0].name = ml_xstrdup("");
    return 0;
}

static int append_reloc(ml_object_t *obj, const ml_reloc_t *rel) {
    obj->relocs = ml_xrealloc(obj->relocs, (obj->reloc_count + 1) * sizeof(obj->relocs[0]));
    obj->relocs[obj->reloc_count++] = *rel;
    return 0;
}

static int load_relocs(ml_context_t *ctx, ml_object_t *obj,
                       const ot_elf_file_t *elf) {
    for (size_t sec_i = 0; sec_i < elf->section_count; sec_i++) {
        const ot_elf_section_t *relsec = &elf->sections[sec_i];
        size_t entsize;
        size_t count;

        if (relsec->type != SHT_RELA && relsec->type != SHT_REL) {
            continue;
        }
        if (relsec->info >= elf->section_count || relsec->link >= elf->section_count ||
            !ot_elf_range_ok(obj->size, relsec->offset, relsec->size)) {
            ml_error(ctx, "%s: invalid relocation section", obj->name);
            return 1;
        }

        entsize = relsec->type == SHT_RELA ? 24 : 16;
        if (relsec->entsize != 0 && relsec->entsize != entsize) {
            ml_error(ctx, "%s: unsupported relocation entry size", obj->name);
            return 1;
        }
        count = (size_t)(relsec->size / entsize);

        for (size_t i = 0; i < count; i++) {
            const unsigned char *p = obj->data + relsec->offset + i * entsize;
            uint64_t info;
            ml_reloc_t rel;

            memset(&rel, 0, sizeof(rel));
            rel.target_section = relsec->info;
            rel.offset = ml_get64(p);
            info = ml_get64(p + 8);
            rel.symbol_index = ELF64_R_SYM(info);
            rel.type = ELF64_R_TYPE(info);
            rel.has_addend = relsec->type == SHT_RELA;
            if (rel.has_addend) {
                rel.addend = (int64_t)ml_get64(p + 16);
            }
            append_reloc(obj, &rel);
        }
    }
    return 0;
}

int ml_parse_elf_object(ml_context_t *ctx, const char *name,
                        unsigned char *data, size_t size, int selected,
                        int from_archive, ml_object_t **out_obj) {
    ml_object_t *obj;
    ot_elf_file_t elf;
    char err[256];

    if (ot_elf_parse(&elf, name, data, size, err, sizeof(err)) != 0) {
        ml_error(ctx, "%s", err);
        return 1;
    }
    if (elf.type != ET_REL || elf.machine != EM_X86_64) {
        ml_error(ctx, "%s: expected x86-64 relocatable object", name);
        ot_elf_free(&elf);
        return 1;
    }

    obj = ml_xcalloc(1, sizeof(*obj));
    obj->name = ml_xstrdup(name);
    obj->data = data;
    obj->size = size;
    obj->selected = selected;
    obj->from_archive = from_archive;

    if (load_sections(ctx, obj, &elf) != 0 ||
        load_symbols(ctx, obj, &elf) != 0 ||
        load_relocs(ctx, obj, &elf) != 0) {
        ot_elf_free(&elf);
        obj->data = NULL;
        ml_object_free(obj);
        return 1;
    }

    ot_elf_free(&elf);
    *out_obj = obj;
    ml_verbose(ctx, "loaded %s%s", name, from_archive ? " (archive member)" : "");
    return 0;
}
