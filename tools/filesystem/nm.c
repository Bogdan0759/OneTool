// nm tool: print ELF symbols
#include "../../libs/elf/elf.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint64_t value;
    unsigned char info;
    unsigned char other;
    uint16_t shndx;
    char type;
    const char *name;
} nm_symbol_t;

static void help(const char *tool) {
    printf("nm - print ELF symbols\n");
    printf("usage: %s [-a] [-g] [-u] file...\n", tool);
    printf("options:\n");
    printf("  -a, --all       include empty/debug symbols\n");
    printf("  -g, --extern    show only external symbols\n");
    printf("  -u, --undefined show only undefined symbols\n");
    printf("  --help          show this help\n");
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size == 0 ? 1 : size);
    if (p == NULL) {
        fprintf(stderr, "nm: out of memory\n");
        exit(2);
    }
    return p;
}

static int read_file(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    unsigned char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;

    if (fd < 0) {
        return 1;
    }

    for (;;) {
        ssize_t n;

        if (len == cap) {
            size_t next = cap == 0 ? 16384 : cap * 2;
            buf = xrealloc(buf, next);
            cap = next;
        }

        n = read(fd, buf + len, cap - len);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(buf);
            close(fd);
            return 1;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }

    close(fd);
    *data_out = buf == NULL ? xrealloc(NULL, 1) : buf;
    *size_out = len;
    return 0;
}

static char localize_type(char type, unsigned char bind) {
    if (bind == STB_LOCAL && type >= 'A' && type <= 'Z') {
        return (char)(type - 'A' + 'a');
    }
    return type;
}

static char section_type(const ot_elf_file_t *elf, uint16_t shndx,
                         unsigned char info, uint64_t value) {
    unsigned char bind = ELF64_ST_BIND(info);
    unsigned char stype = ELF64_ST_TYPE(info);
    const ot_elf_section_t *s;
    char type;

    if (bind == STB_WEAK) {
        return shndx == SHN_UNDEF ? 'w' : 'W';
    }
    if (bind == STB_GNU_UNIQUE) {
        return 'u';
    }
    if (stype == STT_GNU_IFUNC) {
        return localize_type('I', bind);
    }
    if (shndx == SHN_UNDEF) {
        return 'U';
    }
    if (shndx == SHN_ABS) {
        return localize_type('A', bind);
    }
    if (shndx == SHN_COMMON) {
        return localize_type('C', bind);
    }
    if (shndx >= elf->section_count) {
        return '?';
    }

    s = &elf->sections[shndx];
    if (s->type == SHT_NOBITS) {
        type = 'B';
    } else if ((s->flags & SHF_EXECINSTR) != 0) {
        type = 'T';
    } else if ((s->flags & SHF_ALLOC) == 0) {
        type = 'N';
    } else if ((s->flags & SHF_WRITE) != 0) {
        type = 'D';
    } else {
        type = 'R';
    }

    if (stype == STT_SECTION && value == 0) {
        type = 'N';
    }
    return localize_type(type, bind);
}

static int symbol_cmp(const void *a, const void *b) {
    const nm_symbol_t *sa = a;
    const nm_symbol_t *sb = b;
    int by_name = strcmp(sa->name, sb->name);

    if (by_name != 0) {
        return by_name;
    }
    if (sa->value < sb->value) {
        return -1;
    }
    if (sa->value > sb->value) {
        return 1;
    }
    return (int)sa->type - (int)sb->type;
}

static const char *symbol_name(const unsigned char *strtab, size_t strtab_size,
                               uint32_t off) {
    size_t end;

    if (off >= strtab_size) {
        return "<badstr>";
    }
    end = off;
    while (end < strtab_size && strtab[end] != '\0') {
        end++;
    }
    if (end >= strtab_size) {
        return "<badstr>";
    }
    return (const char *)strtab + off;
}

static int should_print_symbol(const nm_symbol_t *sym, int all, int extern_only,
                               int undefined_only) {
    unsigned char bind = ELF64_ST_BIND(sym->info);
    unsigned char stype = ELF64_ST_TYPE(sym->info);

    if (!all && (sym->name[0] == '\0' || stype == STT_FILE || stype == STT_SECTION)) {
        return 0;
    }
    if (extern_only && bind == STB_LOCAL) {
        return 0;
    }
    if (undefined_only && sym->shndx != SHN_UNDEF) {
        return 0;
    }
    return 1;
}

static int load_symbols(const char *path, const ot_elf_file_t *elf,
                        nm_symbol_t **out_symbols, size_t *out_count,
                        int all, int extern_only, int undefined_only) {
    nm_symbol_t *symbols = NULL;
    size_t count = 0;
    int have_symtab = 0;

    for (size_t i = 0; i < elf->section_count; i++) {
        if (elf->sections[i].type == SHT_SYMTAB) {
            have_symtab = 1;
            break;
        }
    }

    for (size_t sec_i = 0; sec_i < elf->section_count; sec_i++) {
        const ot_elf_section_t *symsec = &elf->sections[sec_i];
        const ot_elf_section_t *strsec;
        const unsigned char *strtab;
        size_t strtab_size;
        size_t sym_count;

        if (symsec->type != SHT_SYMTAB &&
            !(symsec->type == SHT_DYNSYM && !have_symtab)) {
            continue;
        }
        if (symsec->entsize != 24 || symsec->link >= elf->section_count ||
            !ot_elf_range_ok(elf->size, symsec->offset, symsec->size)) {
            fprintf(stderr, "nm: %s: invalid symbol table\n", path);
            free(symbols);
            return 1;
        }

        strsec = &elf->sections[symsec->link];
        if (!ot_elf_range_ok(elf->size, strsec->offset, strsec->size)) {
            fprintf(stderr, "nm: %s: invalid string table\n", path);
            free(symbols);
            return 1;
        }

        strtab = elf->data + strsec->offset;
        strtab_size = (size_t)strsec->size;
        sym_count = (size_t)(symsec->size / symsec->entsize);

        for (size_t i = 0; i < sym_count; i++) {
            const unsigned char *p = elf->data + symsec->offset + i * symsec->entsize;
            uint32_t name_off = ot_elf_get32(p);
            nm_symbol_t sym;

            memset(&sym, 0, sizeof(sym));
            sym.info = p[4];
            sym.other = p[5];
            sym.shndx = ot_elf_get16(p + 6);
            sym.value = ot_elf_get64(p + 8);
            sym.name = symbol_name(strtab, strtab_size, name_off);
            sym.type = section_type(elf, sym.shndx, sym.info, sym.value);

            if (!should_print_symbol(&sym, all, extern_only, undefined_only)) {
                continue;
            }

            symbols = xrealloc(symbols, (count + 1) * sizeof(symbols[0]));
            symbols[count++] = sym;
        }
    }

    *out_symbols = symbols;
    *out_count = count;
    return 0;
}

static int inspect_file(const char *path, int all, int extern_only,
                        int undefined_only, int show_header) {
    unsigned char *data = NULL;
    size_t data_size = 0;
    ot_elf_file_t elf;
    nm_symbol_t *symbols = NULL;
    size_t symbol_count = 0;
    char err[256];
    int rc = 0;

    if (read_file(path, &data, &data_size) != 0) {
        fprintf(stderr, "nm: %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (ot_elf_parse(&elf, path, data, data_size, err, sizeof(err)) != 0) {
        fprintf(stderr, "nm: %s\n", err);
        free(data);
        return 1;
    }

    if (load_symbols(path, &elf, &symbols, &symbol_count, all, extern_only,
                     undefined_only) != 0) {
        ot_elf_free(&elf);
        free(data);
        return 1;
    }

    qsort(symbols, symbol_count, sizeof(symbols[0]), symbol_cmp);
    if (show_header) {
        printf("\n%s:\n", path);
    }
    for (size_t i = 0; i < symbol_count; i++) {
        if (symbols[i].shndx == SHN_UNDEF) {
            printf("%16s %c %s\n", "", symbols[i].type, symbols[i].name);
        } else {
            printf("%016" PRIx64 " %c %s\n",
                   symbols[i].value, symbols[i].type, symbols[i].name);
        }
    }

    free(symbols);
    ot_elf_free(&elf);
    free(data);
    return rc;
}

int main(int argc, char *argv[]) {
    const char *files[128];
    int file_count = 0;
    int all = 0;
    int extern_only = 0;
    int undefined_only = 0;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            help(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-a") == 0 || strcmp(arg, "--all") == 0) {
            all = 1;
            continue;
        }
        if (strcmp(arg, "-g") == 0 || strcmp(arg, "--extern") == 0 ||
            strcmp(arg, "--extern-only") == 0) {
            extern_only = 1;
            continue;
        }
        if (strcmp(arg, "-u") == 0 || strcmp(arg, "--undefined") == 0 ||
            strcmp(arg, "--undefined-only") == 0) {
            undefined_only = 1;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "nm: unknown option %s\n", arg);
            return 1;
        }
        if (file_count >= (int)(sizeof(files) / sizeof(files[0]))) {
            fprintf(stderr, "nm: too many input files\n");
            return 1;
        }
        files[file_count++] = arg;
    }

    if (file_count == 0) {
        fprintf(stderr, "usage: %s [-a] [-g] [-u] file...\n", argv[0]);
        return 1;
    }

    for (int i = 0; i < file_count; i++) {
        if (inspect_file(files[i], all, extern_only, undefined_only,
                         file_count > 1) != 0) {
            rc = 1;
        }
    }
    return rc;
}
