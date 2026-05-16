#ifndef ONETOOL_LIBS_SRAPI_DRM_INTERNAL_H
#define ONETOOL_LIBS_SRAPI_DRM_INTERNAL_H

#include "internal.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <stdint.h>

typedef struct {
    uint32_t fb_id;
    uint32_t handle;
    uint64_t size;
    void *map;
    srapi_framebuffer_t fb;
} srapi_drm_buffer_t;

struct srapi_drm_display {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    struct drm_mode_modeinfo mode;
    struct drm_mode_crtc old_crtc;
    int has_old_crtc;
    int front;
    srapi_drm_buffer_t buffers[2];
};

int srapi_drm_ioctl(int fd, unsigned long request, void *arg);
srapi_result_t srapi_drm_create_buffer(int fd, const struct drm_mode_modeinfo *mode, srapi_drm_buffer_t *buffer);
void srapi_drm_destroy_buffer(int fd, srapi_drm_buffer_t *buffer);

#endif
