#include "drm_internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int srapi_drm_ioctl(int fd, unsigned long request, void *arg) {
    int rc;

    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

srapi_result_t srapi_drm_create_buffer(int fd, const struct drm_mode_modeinfo *mode, srapi_drm_buffer_t *buffer) {
    struct drm_mode_create_dumb create;
    struct drm_mode_fb_cmd fb_cmd;
    struct drm_mode_map_dumb map;

    memset(buffer, 0, sizeof(*buffer));
    memset(&create, 0, sizeof(create));
    create.width = mode->hdisplay;
    create.height = mode->vdisplay;
    create.bpp = 32;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        srapi_set_error("drm: CREATE_DUMB %ux%u failed: %s",
                        mode->hdisplay, mode->vdisplay, strerror(errno));
        return SRAPI_ERROR;
    }
    srapi_debugf("drm dumb create %ux%u pitch=%u size=%llu handle=%u",
                 create.width, create.height, create.pitch, (unsigned long long)create.size, create.handle);

    memset(&fb_cmd, 0, sizeof(fb_cmd));
    fb_cmd.width = create.width;
    fb_cmd.height = create.height;
    fb_cmd.pitch = create.pitch;
    fb_cmd.bpp = 32;
    fb_cmd.depth = 24;
    fb_cmd.handle = create.handle;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb_cmd) != 0) {
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("drm: ADDFB failed: %s", strerror(errno));
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }
    srapi_debugf("drm addfb id=%u depth=%u bpp=%u", fb_cmd.fb_id, fb_cmd.depth, fb_cmd.bpp);

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        unsigned int fb_id = fb_cmd.fb_id;
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("drm: MAP_DUMB failed: %s", strerror(errno));
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }

    buffer->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
    if (buffer->map == MAP_FAILED) {
        unsigned int fb_id = fb_cmd.fb_id;
        struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
        srapi_set_error("drm: mmap dumb buffer failed: %s", strerror(errno));
        buffer->map = NULL;
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        return SRAPI_ERROR;
    }

    buffer->fb_id = fb_cmd.fb_id;
    buffer->handle = create.handle;
    buffer->size = create.size;
    buffer->fb.width = create.width;
    buffer->fb.height = create.height;
    buffer->fb.pitch = create.pitch;
    buffer->fb.pixels = buffer->map;
    buffer->fb.owns_pixels = 0;
    buffer->fb.backend = SRAPI_BACKEND_GPU;
    srapi_debugf("drm buffer mapped fb=%u ptr=%p size=%llu", buffer->fb_id, buffer->map, (unsigned long long)buffer->size);
    return SRAPI_OK;
}

void srapi_drm_destroy_buffer(int fd, srapi_drm_buffer_t *buffer) {
    if (buffer == NULL) {
        return;
    }
    if (buffer->map != NULL) {
        srapi_debugf("drm munmap fb=%u size=%llu", buffer->fb_id, (unsigned long long)buffer->size);
        munmap(buffer->map, buffer->size);
        buffer->map = NULL;
    }
    if (fd >= 0 && buffer->fb_id != 0) {
        unsigned int fb_id = buffer->fb_id;
        srapi_debugf("drm rmfb id=%u", fb_id);
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
        buffer->fb_id = 0;
    }
    if (fd >= 0 && buffer->handle != 0) {
        struct drm_mode_destroy_dumb destroy = { .handle = buffer->handle };
        srapi_debugf("drm destroy dumb handle=%u", buffer->handle);
        srapi_drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        buffer->handle = 0;
    }
}
