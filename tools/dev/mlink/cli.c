#include "mlink.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void help(const char *tool) {
    printf("mlink - minimalistic ELF64 x86-64 linker\n");
    printf("usage: %s [options] file.o [lib.a ...]\n", tool);
    printf("options:\n");
    printf("  -o file            output executable (default a.out)\n");
    printf("  -e symbol          entry symbol (default _start)\n");
    printf("  --base addr        image base address (default 0x400000)\n");
    printf("  -Map file          write link map\n");
    printf("  --print-map        print link map to stdout\n");
    printf("  --print-symbols    print global symbols to stdout\n");
    printf("  --dry-run          parse, resolve and layout without writing output\n");
    printf("  -v, --verbose      print archive member selection and loader details\n");
    printf("  -h, --help         show this help\n");
    printf("\n");
    printf("scope: static ET_EXEC from ELF64 relocatable objects; supports .o, simple .a,\n");
    printf("       .text/.rodata/.data/.bss/common and common x86-64 relocations.\n");
}

static int parse_args(int argc, char *argv[], ml_context_t *ctx) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            while (++i < argc) {
                ml_add_input_path(ctx, argv[i]);
            }
            break;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            help(argv[0]);
            return 2;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            ctx->verbose = 1;
            continue;
        }
        if (strcmp(arg, "--dry-run") == 0) {
            ctx->dry_run = 1;
            continue;
        }
        if (strcmp(arg, "--print-map") == 0) {
            ctx->print_map = 1;
            continue;
        }
        if (strcmp(arg, "--print-symbols") == 0) {
            ctx->print_symbols = 1;
            continue;
        }
        if (strcmp(arg, "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mlink: missing value for -o\n");
                return 1;
            }
            ctx->output_path = argv[++i];
            continue;
        }
        if (strncmp(arg, "-o=", 3) == 0) {
            ctx->output_path = arg + 3;
            continue;
        }
        if (strncmp(arg, "--output=", 9) == 0) {
            ctx->output_path = arg + 9;
            continue;
        }
        if (strcmp(arg, "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mlink: missing value for -e\n");
                return 1;
            }
            ctx->entry_name = argv[++i];
            continue;
        }
        if (strncmp(arg, "-e=", 3) == 0) {
            ctx->entry_name = arg + 3;
            continue;
        }
        if (strncmp(arg, "--entry=", 8) == 0) {
            ctx->entry_name = arg + 8;
            continue;
        }
        if (strcmp(arg, "--base") == 0) {
            if (i + 1 >= argc || ml_parse_u64(argv[++i], &ctx->base_addr) != 0) {
                fprintf(stderr, "mlink: invalid --base value\n");
                return 1;
            }
            if ((ctx->base_addr & (ML_PAGE_SIZE - 1)) != 0) {
                fprintf(stderr, "mlink: --base must be page aligned\n");
                return 1;
            }
            continue;
        }
        if (strncmp(arg, "--base=", 7) == 0) {
            if (ml_parse_u64(arg + 7, &ctx->base_addr) != 0) {
                fprintf(stderr, "mlink: invalid base value\n");
                return 1;
            }
            if ((ctx->base_addr & (ML_PAGE_SIZE - 1)) != 0) {
                fprintf(stderr, "mlink: --base must be page aligned\n");
                return 1;
            }
            continue;
        }
        if (strcmp(arg, "-Map") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "mlink: missing value for -Map\n");
                return 1;
            }
            ctx->map_path = argv[++i];
            continue;
        }
        if (strncmp(arg, "-Map=", 5) == 0) {
            ctx->map_path = arg + 5;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "mlink: unknown option %s\n", arg);
            return 1;
        }

        ml_add_input_path(ctx, arg);
    }

    if (ctx->input_count == 0) {
        fprintf(stderr, "mlink: input files required\n");
        return 1;
    }
    return 0;
}

static int write_map_file(ml_context_t *ctx) {
    FILE *f;

    if (ctx->map_path == NULL) {
        return 0;
    }
    f = fopen(ctx->map_path, "w");
    if (f == NULL) {
        ml_error(ctx, "%s: %s", ctx->map_path, strerror(errno));
        return 1;
    }
    ml_print_map(ctx, f);
    if (fclose(f) != 0) {
        ml_error(ctx, "%s: %s", ctx->map_path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    ml_context_t ctx;
    int rc;

    ml_context_init(&ctx);
    rc = parse_args(argc, argv, &ctx);
    if (rc == 2) {
        ml_context_free(&ctx);
        return 0;
    }
    if (rc != 0) {
        ml_context_free(&ctx);
        return 1;
    }

    for (size_t i = 0; i < ctx.input_count; i++) {
        ml_load_input(&ctx, ctx.inputs[i]);
    }

    if (ctx.error_count == 0) {
        ml_resolve_symbols(&ctx);
    }
    if (ctx.error_count == 0) {
        ml_layout(&ctx);
    }
    if (ctx.error_count == 0) {
        ml_apply_relocations(&ctx);
    }
    if (ctx.error_count == 0) {
        write_map_file(&ctx);
    }
    if (ctx.error_count == 0 && ctx.print_map) {
        ml_print_map(&ctx, stdout);
    }
    if (ctx.error_count == 0 && ctx.print_symbols) {
        ml_print_symbols(&ctx, stdout);
    }
    if (ctx.error_count == 0) {
        ml_emit_output(&ctx);
    }

    rc = ctx.error_count == 0 ? 0 : 1;
    ml_context_free(&ctx);
    return rc;
}
