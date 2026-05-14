// size tool: print ELF section sizes
#include "../../libs/elf/elf.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    uint64_t text;
    uint64_t data;
    uint64_t bss;
} size_totals_t;

static void help(const char *tool) {
    printf("size - print ELF section sizes\n");
    printf("usage: %s [-A|-B] file...\n", tool);
    printf("options:\n");
    printf("  -A, --format=sysv       print per-section sizes\n");
    printf("  -B, --format=berkeley   print text/data/bss summary (default)\n");
    printf("  --help                  show this help\n");
}

static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size == 0 ? 1 : size);
    if (p == NULL) {
        fprintf(stderr, "size: out of memory\n");
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

static void add_alloc_section(size_totals_t *totals, const ot_elf_section_t *s) {
    if ((s->flags & SHF_ALLOC) == 0) {
        return;
    }
    if (s->type == SHT_NOBITS) {
        totals->bss += s->size;
    } else if ((s->flags & SHF_WRITE) != 0) {
        totals->data += s->size;
    } else {
        totals->text += s->size;
    }
}

static int print_berkeley(const char *path, const ot_elf_file_t *elf) {
    size_totals_t totals = {0};

    for (size_t i = 0; i < elf->section_count; i++) {
        add_alloc_section(&totals, &elf->sections[i]);
    }

    uint64_t total = totals.text + totals.data + totals.bss;
    printf("%7" PRIu64 " %7" PRIu64 " %7" PRIu64 " %7" PRIu64 " %7" PRIx64 " %s\n",
           totals.text, totals.data, totals.bss, total, total, path);
    return 0;
}

static int print_sysv(const char *path, const ot_elf_file_t *elf) {
    uint64_t total = 0;

    printf("%s  :\n", path);
    printf("%-24s %12s %18s\n", "section", "size", "addr");
    for (size_t i = 0; i < elf->section_count; i++) {
        const ot_elf_section_t *s = &elf->sections[i];

        if (s->size == 0 || s->name[0] == '\0') {
            continue;
        }
        printf("%-24s %12" PRIu64 " %#18" PRIx64 "\n",
               s->name, s->size, s->addr);
        total += s->size;
    }
    printf("%-24s %12" PRIu64 "\n", "Total", total);
    return 0;
}

static int inspect_file(const char *path, int sysv) {
    unsigned char *data = NULL;
    size_t data_size = 0;
    ot_elf_file_t elf;
    char err[256];
    int rc;

    if (read_file(path, &data, &data_size) != 0) {
        fprintf(stderr, "size: %s: %s\n", path, strerror(errno));
        return 1;
    }

    if (ot_elf_parse(&elf, path, data, data_size, err, sizeof(err)) != 0) {
        fprintf(stderr, "size: %s\n", err);
        free(data);
        return 1;
    }

    rc = sysv ? print_sysv(path, &elf) : print_berkeley(path, &elf);
    ot_elf_free(&elf);
    free(data);
    return rc;
}

int main(int argc, char *argv[]) {
    int sysv = 0;
    int first_file = 1;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            help(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-A") == 0 || strcmp(arg, "--format=sysv") == 0 ||
            strcmp(arg, "--sections") == 0) {
            sysv = 1;
            continue;
        }
        if (strcmp(arg, "-B") == 0 || strcmp(arg, "--format=berkeley") == 0) {
            sysv = 0;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "size: unknown option %s\n", arg);
            return 1;
        }

        if (first_file && !sysv) {
            printf("%7s %7s %7s %7s %7s %s\n",
                   "text", "data", "bss", "dec", "hex", "filename");
        }
        if (!first_file && sysv) {
            printf("\n");
        }
        if (inspect_file(arg, sysv) != 0) {
            rc = 1;
        }
        first_file = 0;
    }

    if (first_file) {
        fprintf(stderr, "usage: %s [-A|-B] file...\n", argv[0]);
        return 1;
    }
    return rc;
}
