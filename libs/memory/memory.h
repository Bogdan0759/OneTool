#ifndef ONETOOL_MEMORY_H
#define ONETOOL_MEMORY_H

#include <stddef.h>

void *ot_xmalloc(size_t size);
void *ot_xcalloc(size_t count, size_t size);
void *ot_xrealloc(void *ptr, size_t size);
void *ot_xreallocarray(void *ptr, size_t count, size_t size);
char *ot_xstrdup(const char *s);
char *ot_xstrndup(const char *s, size_t len);

#endif
