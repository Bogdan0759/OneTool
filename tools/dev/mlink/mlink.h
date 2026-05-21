#ifndef ONETOOL_MLINK_H
#define ONETOOL_MLINK_H

#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define ML_PAGE_SIZE 0x1000ULL

typedef enum {
    ML_SEC_SKIP = 0,
    ML_SEC_TEXT,
    ML_SEC_RODATA,
    ML_SEC_DATA,
    ML_SEC_BSS
} ml_section_kind_t;

typedef struct {
    char *name;
    uint32_t type;
    uint64_t flags;
    uint64_t align;
    uint64_t size;
    uint64_t file_size;
    unsigned char *image;
    ml_section_kind_t kind;
    uint64_t out_offset;
    uint64_t out_addr;
} ml_section_t;

typedef struct {
    char *name;
    unsigned char bind;
    unsigned char type;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
    int undefined;
    int common;
} ml_symbol_t;

typedef struct {
    uint32_t target_section;
    uint32_t symbol_index;
    uint32_t type;
    uint64_t offset;
    int64_t addend;
    int has_addend;
} ml_reloc_t;

typedef struct ml_object {
    char *name;
    unsigned char *data;
    size_t size;
    int from_archive;
    int selected;
    ml_section_t *sections;
    size_t section_count;
    ml_symbol_t *symbols;
    size_t symbol_count;
    ml_reloc_t *relocs;
    size_t reloc_count;
} ml_object_t;

typedef struct {
    const char *name;
    ml_object_t *object;
    ml_symbol_t *symbol;
    int defined;
    int weak;
    int common;
    int absolute;
    int referenced;
    int strong_ref;
    uint64_t value;
    uint64_t size;
    uint64_t common_align;
    uint64_t common_addr;
} ml_global_t;

typedef struct {
    char *name;
    uint64_t value;
} ml_defsym_t;

typedef struct {
    const char *output_path;
    const char *entry_name;
    const char *map_path;
    uint64_t base_addr;
    int print_map;
    int print_symbols;
    int dry_run;
    int verbose;
    int no_gnu_stack;

    char **inputs;
    size_t input_count;
    size_t input_cap;

    char **lib_paths;
    size_t lib_path_count;
    size_t lib_path_cap;

    ml_defsym_t *defsyms;
    size_t defsym_count;
    size_t defsym_cap;

    ml_object_t **objects;
    size_t object_count;
    size_t object_cap;

    ml_global_t *globals;
    size_t global_count;
    size_t global_cap;

    int error_count;
    uint16_t phnum;
    int has_rodata_segment;
    int has_rw_segment;
    uint64_t rx_filesz;
    uint64_t ro_offset;
    uint64_t ro_filesz;
    uint64_t data_offset;
    uint64_t data_filesz;
    uint64_t data_memsz;
    uint64_t final_file_size;
    uint64_t entry_addr;
} ml_context_t;

void ml_context_init(ml_context_t *ctx);
void ml_context_free(ml_context_t *ctx);
int ml_add_input_path(ml_context_t *ctx, const char *path);
int ml_add_object(ml_context_t *ctx, ml_object_t *obj);
int ml_add_lib_path(ml_context_t *ctx, const char *path);
int ml_add_lib_name(ml_context_t *ctx, const char *name);
int ml_add_defsym(ml_context_t *ctx, const char *spec);
void ml_object_free(ml_object_t *obj);

void *ml_xmalloc(size_t size);
void *ml_xcalloc(size_t count, size_t size);
void *ml_xrealloc(void *ptr, size_t size);
char *ml_xstrdup(const char *s);
char *ml_xstrndup(const char *s, size_t len);
uint64_t ml_align_up(uint64_t value, uint64_t align);
int ml_parse_u64(const char *s, uint64_t *out);
int ml_read_file(const char *path, unsigned char **data_out, size_t *size_out);
int ml_write_file_mode(const char *path, const unsigned char *data, size_t size, int mode);
uint16_t ml_get16(const unsigned char *p);
uint32_t ml_get32(const unsigned char *p);
uint64_t ml_get64(const unsigned char *p);
void ml_put16(unsigned char *p, uint16_t v);
void ml_put32(unsigned char *p, uint32_t v);
void ml_put64(unsigned char *p, uint64_t v);

void ml_error(ml_context_t *ctx, const char *fmt, ...);
void ml_verbose(ml_context_t *ctx, const char *fmt, ...);

int ml_load_input(ml_context_t *ctx, const char *path);
int ml_parse_elf_object(ml_context_t *ctx, const char *name,
                        unsigned char *data, size_t size, int selected,
                        int from_archive, ml_object_t **out_obj);

int ml_resolve_symbols(ml_context_t *ctx);
ml_global_t *ml_find_global(ml_context_t *ctx, const char *name);
int ml_inject_defsyms(ml_context_t *ctx);
void ml_inject_linker_symbols(ml_context_t *ctx);
int ml_report_undefined(ml_context_t *ctx);

int ml_layout(ml_context_t *ctx);
int ml_apply_relocations(ml_context_t *ctx);
int ml_emit_output(ml_context_t *ctx);
void ml_print_map(ml_context_t *ctx, FILE *out);
void ml_print_symbols(ml_context_t *ctx, FILE *out);

const char *ml_section_kind_name(ml_section_kind_t kind);
const char *ml_reloc_name(uint32_t type);

#endif
