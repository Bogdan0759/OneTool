#include "memory.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void oom(void) {
    fprintf(stderr, "onetool: out of memory\n");
    exit(2);
}

static size_t nonzero_size(size_t size) {
    return size == 0 ? 1 : size;
}

static size_t checked_mul(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) {
        oom();
    }
    return count * size;
}

void *ot_xmalloc(size_t size) {
    void *p = malloc(nonzero_size(size));
    if (p == NULL) {
        oom();
    }
    return p;
}

void *ot_xcalloc(size_t count, size_t size) {
    size_t total = checked_mul(count, size);
    void *p;

    p = calloc(1, nonzero_size(total));
    if (p == NULL) {
        oom();
    }
    return p;
}

void *ot_xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, nonzero_size(size));
    if (p == NULL) {
        oom();
    }
    return p;
}

void *ot_xreallocarray(void *ptr, size_t count, size_t size) {
    return ot_xrealloc(ptr, checked_mul(count, size));
}

char *ot_xstrdup(const char *s) {
    size_t len = strlen(s);
    char *copy = ot_xmalloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *ot_xstrndup(const char *s, size_t len) {
    char *copy = ot_xmalloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}
