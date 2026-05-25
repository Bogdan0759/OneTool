#define _GNU_SOURCE
#include "render_node.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static void copy_path(char *dst, size_t cap, const char *src) {
    if (cap == 0) return;
    if (src == NULL) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int derive_render_node(const char *primary_path, char *out_path, size_t cap,
                              uint32_t *out_major, uint32_t *out_minor) {
    const char *base;
    int idx;
    char buf[SPROT_MAX_DEVICE_PATH];
    struct stat st;

    if (primary_path == NULL) return 0;
    base = strrchr(primary_path, '/');
    base = (base != NULL) ? base + 1 : primary_path;
    if (sscanf(base, "card%d", &idx) != 1 || idx < 0 || idx > 63) return 0;
    if ((size_t)snprintf(buf, sizeof(buf), "/dev/dri/renderD%d", 128 + idx) >= sizeof(buf)) return 0;
    if (stat(buf, &st) != 0) return 0;
    if (!S_ISCHR(st.st_mode)) return 0;
    copy_path(out_path, cap, buf);
    *out_major = (uint32_t)major(st.st_rdev);
    *out_minor = (uint32_t)minor(st.st_rdev);
    return 1;
}

void swm_probe_render_node(const char *device_path, sprot_body_render_node_t *out) {
    struct stat st;

    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    if (device_path == NULL || device_path[0] == '\0') return;

    copy_path(out->device_path, sizeof(out->device_path), device_path);
    if (stat(device_path, &st) == 0 && S_ISCHR(st.st_mode)) {
        out->primary_major = (uint32_t)major(st.st_rdev);
        out->primary_minor = (uint32_t)minor(st.st_rdev);
        out->has_drm = 1;
    }
    derive_render_node(device_path,
                       out->render_node_path, sizeof(out->render_node_path),
                       &out->render_major, &out->render_minor);
}
