#include "elf.h"
#include "../memory/memory.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error(char *err, size_t err_size, const char *fmt, ...) {
    va_list ap;

    if (err == NULL || err_size == 0) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

void ot_elf_init(ot_elf_file_t *elf) {
    memset(elf, 0, sizeof(*elf));
}

void ot_elf_free(ot_elf_file_t *elf) {
    if (elf == NULL) {
        return;
    }
    for (size_t i = 0; i < elf->section_count; i++) {
        free(elf->sections[i].name);
    }
    free(elf->sections);
    memset(elf, 0, sizeof(*elf));
}

int ot_elf_range_ok(size_t file_size, uint64_t off, uint64_t size) {
    return off <= file_size && size <= file_size - off;
}

char *ot_elf_strdup_from_strtab(const unsigned char *base, size_t size,
                                uint32_t off) {
    size_t end;
    char *s;

    if (off >= size) {
        return ot_xstrdup("<badstr>");
    }

    end = off;
    while (end < size && base[end] != '\0') {
        end++;
    }
    if (end >= size) {
        return ot_xstrdup("<badstr>");
    }

    s = ot_xmalloc(end - off + 1);
    memcpy(s, base + off, end - off);
    s[end - off] = '\0';
    return s;
}

int ot_elf_parse(ot_elf_file_t *elf, const char *name,
                 const unsigned char *data, size_t size,
                 char *err, size_t err_size) {
    uint64_t shoff;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
    ot_elf_section_t *sections;
    const unsigned char *shstr;
    size_t shstr_size;

    ot_elf_init(elf);

    if (size < EI_NIDENT || memcmp(data, ELFMAG, SELFMAG) != 0) {
        set_error(err, err_size, "%s: not an ELF file", name);
        return 1;
    }
    if (size < 64 || data[EI_CLASS] != ELFCLASS64 ||
        data[EI_DATA] != ELFDATA2LSB || data[EI_VERSION] != EV_CURRENT) {
        set_error(err, err_size, "%s: only little-endian ELF64 is supported", name);
        return 1;
    }

    shoff = ot_elf_get64(data + 40);
    shentsize = ot_elf_get16(data + 58);
    shnum = ot_elf_get16(data + 60);
    shstrndx = ot_elf_get16(data + 62);

    if (shentsize != 64 || shnum == 0 ||
        !ot_elf_range_ok(size, shoff, (uint64_t)shentsize * shnum)) {
        set_error(err, err_size, "%s: invalid section table", name);
        return 1;
    }
    if (shstrndx == SHN_UNDEF || shstrndx >= shnum) {
        set_error(err, err_size, "%s: invalid shstrndx", name);
        return 1;
    }

    sections = ot_xcalloc(shnum, sizeof(sections[0]));

    for (uint16_t i = 0; i < shnum; i++) {
        const unsigned char *p = data + shoff + (uint64_t)i * shentsize;
        sections[i].name_offset = ot_elf_get32(p);
        sections[i].type = ot_elf_get32(p + 4);
        sections[i].flags = ot_elf_get64(p + 8);
        sections[i].addr = ot_elf_get64(p + 16);
        sections[i].offset = ot_elf_get64(p + 24);
        sections[i].size = ot_elf_get64(p + 32);
        sections[i].link = ot_elf_get32(p + 40);
        sections[i].info = ot_elf_get32(p + 44);
        sections[i].addralign = ot_elf_get64(p + 48);
        sections[i].entsize = ot_elf_get64(p + 56);
    }

    if (!ot_elf_range_ok(size, sections[shstrndx].offset, sections[shstrndx].size)) {
        free(sections);
        set_error(err, err_size, "%s: invalid section string table", name);
        return 1;
    }

    shstr = data + sections[shstrndx].offset;
    shstr_size = (size_t)sections[shstrndx].size;
    for (size_t i = 0; i < shnum; i++) {
        sections[i].name = ot_elf_strdup_from_strtab(shstr, shstr_size,
                                                     sections[i].name_offset);
    }

    elf->name = name;
    elf->data = data;
    elf->size = size;
    elf->type = ot_elf_get16(data + 16);
    elf->machine = ot_elf_get16(data + 18);
    elf->entry = ot_elf_get64(data + 24);
    elf->shstrndx = shstrndx;
    elf->sections = sections;
    elf->section_count = shnum;
    return 0;
}

uint16_t ot_elf_get16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t ot_elf_get32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t ot_elf_get64(const unsigned char *p) {
    return (uint64_t)ot_elf_get32(p) | ((uint64_t)ot_elf_get32(p + 4) << 32);
}

void ot_elf_put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

void ot_elf_put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

void ot_elf_put64(unsigned char *p, uint64_t v) {
    ot_elf_put32(p, (uint32_t)(v & 0xffffffffU));
    ot_elf_put32(p + 4, (uint32_t)(v >> 32));
}
