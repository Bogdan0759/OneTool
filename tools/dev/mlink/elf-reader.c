#include "mlink.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} ml_elf_shdr_t;

static int range_ok(size_t file_size, uint64_t off, uint64_t size) {
    return off <= file_size && size <= file_size - off;
}

static char *dup_strtab_name(const unsigned char *base, size_t size, uint32_t off) {
    size_t end;

    if (off >= size) {
        return ml_xstrdup("<badstr>");
    }
    end = off;
    while (end < size && base[end] != '\0') {
        end++;
    }
    if (end >= size) {
        return ml_xstrdup("<badstr>");
    }
    return ml_xstrndup((const char *)base + off, end - off);
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

static int read_shdrs(ml_context_t *ctx, const char *name,
                      const unsigned char *data, size_t size,
                      ml_elf_shdr_t **out_shdrs, size_t *out_count,
                      uint16_t *out_shstrndx) {
    uint64_t shoff;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
    ml_elf_shdr_t *shdrs;

    shoff = ml_get64(data + 40);
    shentsize = ml_get16(data + 58);
    shnum = ml_get16(data + 60);
    shstrndx = ml_get16(data + 62);

    if (shentsize != 64 || shnum == 0 || !range_ok(size, shoff, (uint64_t)shentsize * shnum)) {
        ml_error(ctx, "%s: invalid section table", name);
        return 1;
    }

    shdrs = ml_xcalloc(shnum, sizeof(shdrs[0]));
    for (uint16_t i = 0; i < shnum; i++) {
        const unsigned char *p = data + shoff + (uint64_t)i * shentsize;
        shdrs[i].name = ml_get32(p);
        shdrs[i].type = ml_get32(p + 4);
        shdrs[i].flags = ml_get64(p + 8);
        shdrs[i].addr = ml_get64(p + 16);
        shdrs[i].offset = ml_get64(p + 24);
        shdrs[i].size = ml_get64(p + 32);
        shdrs[i].link = ml_get32(p + 40);
        shdrs[i].info = ml_get32(p + 44);
        shdrs[i].addralign = ml_get64(p + 48);
        shdrs[i].entsize = ml_get64(p + 56);
    }

    if (shstrndx == SHN_UNDEF || shstrndx >= shnum) {
        ml_error(ctx, "%s: invalid shstrndx", name);
        free(shdrs);
        return 1;
    }

    *out_shdrs = shdrs;
    *out_count = shnum;
    *out_shstrndx = shstrndx;
    return 0;
}

static int load_sections(ml_context_t *ctx, ml_object_t *obj,
                         const ml_elf_shdr_t *shdrs, size_t shnum,
                         uint16_t shstrndx) {
    const unsigned char *shstr;
    size_t shstr_size;

    if (!range_ok(obj->size, shdrs[shstrndx].offset, shdrs[shstrndx].size)) {
        ml_error(ctx, "%s: invalid section string table", obj->name);
        return 1;
    }

    shstr = obj->data + shdrs[shstrndx].offset;
    shstr_size = (size_t)shdrs[shstrndx].size;
    obj->section_count = shnum;
    obj->sections = ml_xcalloc(shnum, sizeof(obj->sections[0]));

    for (size_t i = 0; i < shnum; i++) {
        ml_section_t *s = &obj->sections[i];
        char *sec_name = dup_strtab_name(shstr, shstr_size, shdrs[i].name);

        s->name = sec_name;
        s->type = shdrs[i].type;
        s->flags = shdrs[i].flags;
        s->align = shdrs[i].addralign == 0 ? 1 : shdrs[i].addralign;
        s->size = shdrs[i].size;
        s->kind = classify_section(s->type, s->flags, s->name);

        if (s->kind == ML_SEC_SKIP || s->type == SHT_NOBITS || s->size == 0) {
            continue;
        }
        if (!range_ok(obj->size, shdrs[i].offset, shdrs[i].size)) {
            ml_error(ctx, "%s: section %s is outside file", obj->name, s->name);
            return 1;
        }
        s->image = ml_xmalloc((size_t)s->size);
        memcpy(s->image, obj->data + shdrs[i].offset, (size_t)s->size);
        s->file_size = s->size;
    }

    return 0;
}

static int load_symbols(ml_context_t *ctx, ml_object_t *obj,
                        const ml_elf_shdr_t *shdrs, size_t shnum) {
    for (size_t sec_i = 0; sec_i < shnum; sec_i++) {
        const ml_elf_shdr_t *symsec = &shdrs[sec_i];
        const unsigned char *strtab;
        size_t strtab_size;
        size_t count;

        if (symsec->type != SHT_SYMTAB) {
            continue;
        }
        if (symsec->entsize != 24 || symsec->link >= shnum ||
            !range_ok(obj->size, symsec->offset, symsec->size)) {
            ml_error(ctx, "%s: invalid symbol table", obj->name);
            return 1;
        }
        if (!range_ok(obj->size, shdrs[symsec->link].offset, shdrs[symsec->link].size)) {
            ml_error(ctx, "%s: invalid symbol string table", obj->name);
            return 1;
        }

        strtab = obj->data + shdrs[symsec->link].offset;
        strtab_size = (size_t)shdrs[symsec->link].size;
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
                       const ml_elf_shdr_t *shdrs, size_t shnum) {
    for (size_t sec_i = 0; sec_i < shnum; sec_i++) {
        const ml_elf_shdr_t *relsec = &shdrs[sec_i];
        size_t entsize;
        size_t count;

        if (relsec->type != SHT_RELA && relsec->type != SHT_REL) {
            continue;
        }
        if (relsec->info >= shnum || relsec->link >= shnum ||
            !range_ok(obj->size, relsec->offset, relsec->size)) {
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
    ml_elf_shdr_t *shdrs = NULL;
    size_t shnum = 0;
    uint16_t shstrndx = 0;

    if (size < 64 || memcmp(data, ELFMAG, SELFMAG) != 0) {
        ml_error(ctx, "%s: not an ELF file", name);
        return 1;
    }
    if (data[EI_CLASS] != ELFCLASS64 || data[EI_DATA] != ELFDATA2LSB ||
        data[EI_VERSION] != EV_CURRENT) {
        ml_error(ctx, "%s: only little-endian ELF64 is supported", name);
        return 1;
    }
    if (ml_get16(data + 16) != ET_REL || ml_get16(data + 18) != EM_X86_64) {
        ml_error(ctx, "%s: expected x86-64 relocatable object", name);
        return 1;
    }

    obj = ml_xcalloc(1, sizeof(*obj));
    obj->name = ml_xstrdup(name);
    obj->data = data;
    obj->size = size;
    obj->selected = selected;
    obj->from_archive = from_archive;

    if (read_shdrs(ctx, name, data, size, &shdrs, &shnum, &shstrndx) != 0 ||
        load_sections(ctx, obj, shdrs, shnum, shstrndx) != 0 ||
        load_symbols(ctx, obj, shdrs, shnum) != 0 ||
        load_relocs(ctx, obj, shdrs, shnum) != 0) {
        free(shdrs);
        obj->data = NULL;
        ml_object_free(obj);
        return 1;
    }

    free(shdrs);
    *out_obj = obj;
    ml_verbose(ctx, "loaded %s%s", name, from_archive ? " (archive member)" : "");
    return 0;
}
