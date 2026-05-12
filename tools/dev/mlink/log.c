#include "mlink.h"

#include <stdarg.h>

void ml_error(ml_context_t *ctx, const char *fmt, ...) {
    va_list ap;

    if (ctx != NULL) {
        ctx->error_count++;
    }

    fprintf(stderr, "mlink: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void ml_verbose(ml_context_t *ctx, const char *fmt, ...) {
    va_list ap;

    if (ctx == NULL || !ctx->verbose) {
        return;
    }

    fprintf(stderr, "mlink: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

const char *ml_section_kind_name(ml_section_kind_t kind) {
    switch (kind) {
    case ML_SEC_TEXT:
        return "text";
    case ML_SEC_RODATA:
        return "rodata";
    case ML_SEC_DATA:
        return "data";
    case ML_SEC_BSS:
        return "bss";
    default:
        return "skip";
    }
}

const char *ml_reloc_name(uint32_t type) {
    switch (type) {
    case R_X86_64_64:
        return "R_X86_64_64";
    case R_X86_64_PC32:
        return "R_X86_64_PC32";
    case R_X86_64_32:
        return "R_X86_64_32";
    case R_X86_64_32S:
        return "R_X86_64_32S";
    case R_X86_64_PLT32:
        return "R_X86_64_PLT32";
    case R_X86_64_RELATIVE:
        return "R_X86_64_RELATIVE";
    default:
        return "R_X86_64_UNKNOWN";
    }
}
