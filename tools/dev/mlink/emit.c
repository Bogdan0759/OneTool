#include "mlink.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void put_ehdr(unsigned char *p, ml_context_t *ctx) {
    memset(p, 0, sizeof(Elf64_Ehdr));
    p[0] = 0x7f;
    p[1] = 'E';
    p[2] = 'L';
    p[3] = 'F';
    p[EI_CLASS] = ELFCLASS64;
    p[EI_DATA] = ELFDATA2LSB;
    p[EI_VERSION] = EV_CURRENT;
    p[EI_OSABI] = ELFOSABI_SYSV;

    ml_put16(p + 16, ET_EXEC);
    ml_put16(p + 18, EM_X86_64);
    ml_put32(p + 20, EV_CURRENT);
    ml_put64(p + 24, ctx->entry_addr);
    ml_put64(p + 32, sizeof(Elf64_Ehdr));
    ml_put64(p + 40, 0);
    ml_put32(p + 48, 0);
    ml_put16(p + 52, sizeof(Elf64_Ehdr));
    ml_put16(p + 54, sizeof(Elf64_Phdr));
    ml_put16(p + 56, ctx->phnum);
    ml_put16(p + 58, 0);
    ml_put16(p + 60, 0);
    ml_put16(p + 62, SHN_UNDEF);
}

static void put_phdr(unsigned char *p, uint32_t flags, uint64_t offset,
                     uint64_t vaddr, uint64_t filesz, uint64_t memsz) {
    memset(p, 0, sizeof(Elf64_Phdr));
    ml_put32(p, PT_LOAD);
    ml_put32(p + 4, flags);
    ml_put64(p + 8, offset);
    ml_put64(p + 16, vaddr);
    ml_put64(p + 24, vaddr);
    ml_put64(p + 32, filesz);
    ml_put64(p + 40, memsz);
    ml_put64(p + 48, ML_PAGE_SIZE);
}

static int copy_sections(ml_context_t *ctx, unsigned char *out, size_t out_size) {
    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->section_count; j++) {
            ml_section_t *s = &obj->sections[j];
            if (s->kind == ML_SEC_SKIP || s->type == SHT_NOBITS || s->file_size == 0) {
                continue;
            }
            if (s->out_offset > out_size || s->file_size > out_size - s->out_offset) {
                ml_error(ctx, "%s: section %s overflows output image", obj->name, s->name);
                return 1;
            }
            memcpy(out + s->out_offset, s->image, (size_t)s->file_size);
        }
    }
    return 0;
}

int ml_emit_output(ml_context_t *ctx) {
    unsigned char *out;
    unsigned char *ph;
    size_t out_size = (size_t)ctx->final_file_size;

    if (ctx->dry_run) {
        return 0;
    }
    if ((uint64_t)out_size != ctx->final_file_size) {
        ml_error(ctx, "output is too large for this host");
        return 1;
    }

    out = ml_xcalloc(1, out_size == 0 ? 1 : out_size);
    put_ehdr(out, ctx);
    ph = out + sizeof(Elf64_Ehdr);

    put_phdr(ph, PF_R | PF_X, 0, ctx->base_addr, ctx->rx_filesz, ctx->rx_filesz);
    ph += sizeof(Elf64_Phdr);

    if (ctx->has_rodata_segment) {
        put_phdr(ph, PF_R, ctx->ro_offset, ctx->base_addr + ctx->ro_offset,
                 ctx->ro_filesz, ctx->ro_filesz);
        ph += sizeof(Elf64_Phdr);
    }

    if (ctx->has_rw_segment) {
        put_phdr(ph, PF_R | PF_W, ctx->data_offset, ctx->base_addr + ctx->data_offset,
                 ctx->data_filesz, ctx->data_memsz);
    }

    if (copy_sections(ctx, out, out_size) != 0) {
        free(out);
        return 1;
    }

    if (ml_write_file_mode(ctx->output_path, out, out_size, 0755) != 0) {
        ml_error(ctx, "%s: %s", ctx->output_path, strerror(errno));
        free(out);
        return 1;
    }

    free(out);
    return 0;
}

void ml_print_map(ml_context_t *ctx, FILE *out) {
    fprintf(out, "mlink map\n");
    fprintf(out, "output: %s%s\n", ctx->output_path, ctx->dry_run ? " (dry-run)" : "");
    fprintf(out, "base:   0x%llx\n", (unsigned long long)ctx->base_addr);
    fprintf(out, "entry:  %s = 0x%llx\n", ctx->entry_name,
            (unsigned long long)ctx->entry_addr);
    fprintf(out, "\nsegments:\n");
    fprintf(out, "  RX off 0x%06llx vaddr 0x%llx filesz 0x%llx\n",
            0ULL, (unsigned long long)ctx->base_addr,
            (unsigned long long)ctx->rx_filesz);
    if (ctx->has_rodata_segment) {
        fprintf(out, "  R  off 0x%06llx vaddr 0x%llx filesz 0x%llx\n",
                (unsigned long long)ctx->ro_offset,
                (unsigned long long)(ctx->base_addr + ctx->ro_offset),
                (unsigned long long)ctx->ro_filesz);
    }
    if (ctx->has_rw_segment) {
        fprintf(out, "  RW off 0x%06llx vaddr 0x%llx filesz 0x%llx memsz 0x%llx\n",
                (unsigned long long)ctx->data_offset,
                (unsigned long long)(ctx->base_addr + ctx->data_offset),
                (unsigned long long)ctx->data_filesz,
                (unsigned long long)ctx->data_memsz);
    }

    fprintf(out, "\nsections:\n");
    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_t *obj = ctx->objects[i];
        if (!obj->selected) {
            continue;
        }
        for (size_t j = 0; j < obj->section_count; j++) {
            ml_section_t *s = &obj->sections[j];
            if (s->kind == ML_SEC_SKIP || s->size == 0) {
                continue;
            }
            fprintf(out, "  0x%llx 0x%06llx %-6s %-18s %s\n",
                    (unsigned long long)s->out_addr,
                    (unsigned long long)s->size,
                    ml_section_kind_name(s->kind),
                    s->name,
                    obj->name);
        }
    }
}

void ml_print_symbols(ml_context_t *ctx, FILE *out) {
    fprintf(out, "symbols:\n");
    for (size_t i = 0; i < ctx->global_count; i++) {
        ml_global_t *g = &ctx->globals[i];
        if (!g->defined && !g->common) {
            continue;
        }
        fprintf(out, "  0x%016llx %s%s\n",
                (unsigned long long)g->value,
                g->common ? "COMMON " : "",
                g->name);
    }
}
