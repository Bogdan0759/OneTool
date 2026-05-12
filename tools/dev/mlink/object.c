#include "mlink.h"

#include <ar.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int starts_with(const unsigned char *data, size_t size, const char *magic) {
    size_t len = strlen(magic);
    return size >= len && memcmp(data, magic, len) == 0;
}

void ml_context_init(ml_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->output_path = "a.out";
    ctx->entry_name = "_start";
    ctx->base_addr = 0x400000;
}

void ml_object_free(ml_object_t *obj) {
    if (obj == NULL) {
        return;
    }

    for (size_t i = 0; i < obj->section_count; i++) {
        free(obj->sections[i].name);
        free(obj->sections[i].image);
    }
    for (size_t i = 0; i < obj->symbol_count; i++) {
        free(obj->symbols[i].name);
    }

    free(obj->name);
    free(obj->data);
    free(obj->sections);
    free(obj->symbols);
    free(obj->relocs);
    free(obj);
}

void ml_context_free(ml_context_t *ctx) {
    for (size_t i = 0; i < ctx->input_count; i++) {
        free(ctx->inputs[i]);
    }
    for (size_t i = 0; i < ctx->object_count; i++) {
        ml_object_free(ctx->objects[i]);
    }

    free(ctx->inputs);
    free(ctx->objects);
    free(ctx->globals);
    memset(ctx, 0, sizeof(*ctx));
}

int ml_add_input_path(ml_context_t *ctx, const char *path) {
    if (ctx->input_count == ctx->input_cap) {
        size_t next = ctx->input_cap == 0 ? 8 : ctx->input_cap * 2;
        ctx->inputs = ml_xrealloc(ctx->inputs, next * sizeof(ctx->inputs[0]));
        ctx->input_cap = next;
    }
    ctx->inputs[ctx->input_count++] = ml_xstrdup(path);
    return 0;
}

int ml_add_object(ml_context_t *ctx, ml_object_t *obj) {
    if (ctx->object_count == ctx->object_cap) {
        size_t next = ctx->object_cap == 0 ? 16 : ctx->object_cap * 2;
        ctx->objects = ml_xrealloc(ctx->objects, next * sizeof(ctx->objects[0]));
        ctx->object_cap = next;
    }
    ctx->objects[ctx->object_count++] = obj;
    return 0;
}

static long parse_ar_decimal(const char *p, size_t len) {
    char buf[32];
    char *end = NULL;
    long v;

    if (len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, p, len);
    buf[len] = '\0';
    v = strtol(buf, &end, 10);
    if (end == buf || v < 0) {
        return -1;
    }
    return v;
}

static char *trim_ar_name(const char *raw, size_t len) {
    while (len > 0 && raw[len - 1] == ' ') {
        len--;
    }
    if (len > 0 && raw[len - 1] == '/') {
        len--;
    }
    return ml_xstrndup(raw, len);
}

static char *gnu_long_name(const unsigned char *strtab, size_t strtab_size, long off) {
    size_t pos = (size_t)off;
    size_t end;

    if (off < 0 || pos >= strtab_size) {
        return ml_xstrdup("<bad-long-name>");
    }
    end = pos;
    while (end < strtab_size && strtab[end] != '\n' && strtab[end] != '/') {
        end++;
    }
    return ml_xstrndup((const char *)strtab + pos, end - pos);
}

static int load_archive(ml_context_t *ctx, const char *path,
                        unsigned char *data, size_t size) {
    const unsigned char *strtab = NULL;
    size_t strtab_size = 0;
    size_t off = 8;
    int loaded = 0;

    while (off + 60 <= size) {
        const unsigned char *hdr = data + off;
        const unsigned char *body;
        size_t body_size;
        long member_size;
        char *member_name = NULL;
        size_t content_skip = 0;

        if (hdr[58] != '`' || hdr[59] != '\n') {
            ml_error(ctx, "%s: invalid archive header", path);
            return 1;
        }

        member_size = parse_ar_decimal((const char *)hdr + 48, 10);
        if (member_size < 0 || off + 60 + (size_t)member_size > size) {
            ml_error(ctx, "%s: invalid archive member size", path);
            return 1;
        }

        body = hdr + 60;
        body_size = (size_t)member_size;

        if (memcmp(hdr, "//", 2) == 0) {
            strtab = body;
            strtab_size = body_size;
        } else if (hdr[0] == '/' && hdr[1] == ' ') {
            /* normal ar symbol table */
        } else if (memcmp(hdr, "/SYM64/", 7) == 0) {
            /* BSD 64-bit symbol table */
        } else {
            if (memcmp(hdr, "#1/", 3) == 0) {
                long name_len = parse_ar_decimal((const char *)hdr + 3, 13);
                if (name_len < 0 || (size_t)name_len > body_size) {
                    ml_error(ctx, "%s: invalid BSD archive name", path);
                    return 1;
                }
                member_name = ml_xstrndup((const char *)body, (size_t)name_len);
                content_skip = (size_t)name_len;
            } else if (hdr[0] == '/' && hdr[1] >= '0' && hdr[1] <= '9') {
                long name_off = parse_ar_decimal((const char *)hdr + 1, 15);
                member_name = gnu_long_name(strtab, strtab_size, name_off);
            } else {
                member_name = trim_ar_name((const char *)hdr, 16);
            }

            if (body_size >= content_skip + SELFMAG &&
                memcmp(body + content_skip, ELFMAG, SELFMAG) == 0) {
                size_t obj_size = body_size - content_skip;
                unsigned char *copy = ml_xmalloc(obj_size);
                char full_name[1024];
                ml_object_t *obj = NULL;

                memcpy(copy, body + content_skip, obj_size);
                snprintf(full_name, sizeof(full_name), "%s(%s)", path, member_name);
                if (ml_parse_elf_object(ctx, full_name, copy, obj_size, 0, 1, &obj) == 0) {
                    ml_add_object(ctx, obj);
                    loaded++;
                } else {
                    free(copy);
                    free(member_name);
                    return 1;
                }
            }
            free(member_name);
        }

        off += 60 + (size_t)member_size;
        if ((off & 1) != 0) {
            off++;
        }
    }

    if (loaded == 0) {
        ml_verbose(ctx, "%s: archive has no ELF64 objects", path);
    }
    return 0;
}

int ml_load_input(ml_context_t *ctx, const char *path) {
    unsigned char *data = NULL;
    size_t size = 0;
    ml_object_t *obj = NULL;

    if (ml_read_file(path, &data, &size) != 0) {
        ml_error(ctx, "%s: %s", path, strerror(errno));
        return 1;
    }

    if (starts_with(data, size, ARMAG)) {
        int rc = load_archive(ctx, path, data, size);
        free(data);
        return rc;
    }

    if (size >= SELFMAG && memcmp(data, ELFMAG, SELFMAG) == 0) {
        if (ml_parse_elf_object(ctx, path, data, size, 1, 0, &obj) != 0) {
            free(data);
            return 1;
        }
        ml_add_object(ctx, obj);
        return 0;
    }

    ml_error(ctx, "%s: expected ELF64 object or .a archive", path);
    free(data);
    return 1;
}
