#ifndef ONETOOL_ELF_H
#define ONETOOL_ELF_H

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint32_t name_offset;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
} ot_elf_section_t;

typedef struct {
    const char *name;
    const unsigned char *data;
    size_t size;
    uint16_t type;
    uint16_t machine;
    uint64_t entry;
    uint16_t shstrndx;
    ot_elf_section_t *sections;
    size_t section_count;
} ot_elf_file_t;

void ot_elf_init(ot_elf_file_t *elf);
void ot_elf_free(ot_elf_file_t *elf);

int ot_elf_parse(ot_elf_file_t *elf, const char *name,
                 const unsigned char *data, size_t size,
                 char *err, size_t err_size);

int ot_elf_range_ok(size_t file_size, uint64_t off, uint64_t size);
char *ot_elf_strdup_from_strtab(const unsigned char *base, size_t size,
                                uint32_t off);

uint16_t ot_elf_get16(const unsigned char *p);
uint32_t ot_elf_get32(const unsigned char *p);
uint64_t ot_elf_get64(const unsigned char *p);
void ot_elf_put16(unsigned char *p, uint16_t v);
void ot_elf_put32(unsigned char *p, uint32_t v);
void ot_elf_put64(unsigned char *p, uint64_t v);

#endif
