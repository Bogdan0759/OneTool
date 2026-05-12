#include "mlink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *ml_xmalloc(size_t size) {
    void *p = malloc(size == 0 ? 1 : size);
    if (p == NULL) {
        fprintf(stderr, "mlink: out of memory\n");
        exit(2);
    }
    return p;
}

void *ml_xcalloc(size_t count, size_t size) {
    void *p = calloc(count == 0 ? 1 : count, size == 0 ? 1 : size);
    if (p == NULL) {
        fprintf(stderr, "mlink: out of memory\n");
        exit(2);
    }
    return p;
}

void *ml_xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size == 0 ? 1 : size);
    if (p == NULL) {
        fprintf(stderr, "mlink: out of memory\n");
        exit(2);
    }
    return p;
}

char *ml_xstrdup(const char *s) {
    size_t len = strlen(s);
    char *copy = ml_xmalloc(len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *ml_xstrndup(const char *s, size_t len) {
    char *copy = ml_xmalloc(len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

uint64_t ml_align_up(uint64_t value, uint64_t align) {
    if (align <= 1) {
        return value;
    }
    return (value + align - 1) / align * align;
}

int ml_parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long v;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0') {
        return 1;
    }
    *out = (uint64_t)v;
    return 0;
}

int ml_read_file(const char *path, unsigned char **data_out, size_t *size_out) {
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
            buf = ml_xrealloc(buf, next);
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
    *data_out = buf == NULL ? ml_xmalloc(1) : buf;
    *size_out = len;
    return 0;
}

int ml_write_file_mode(const char *path, const unsigned char *data, size_t size, int mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    size_t off = 0;

    if (fd < 0) {
        return 1;
    }

    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return 1;
        }
        off += (size_t)n;
    }

    if (close(fd) != 0) {
        return 1;
    }
    return 0;
}

uint16_t ml_get16(const unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t ml_get32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t ml_get64(const unsigned char *p) {
    return (uint64_t)ml_get32(p) | ((uint64_t)ml_get32(p + 4) << 32);
}

void ml_put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

void ml_put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

void ml_put64(unsigned char *p, uint64_t v) {
    ml_put32(p, (uint32_t)(v & 0xffffffffU));
    ml_put32(p + 4, (uint32_t)(v >> 32));
}
