#ifndef ONETOOL_LIBS_SRAPI_DRM_INTERNAL_H
#define ONETOOL_LIBS_SRAPI_DRM_INTERNAL_H

#include "internal.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t fb_id;
    uint32_t handle;
    uint64_t size;
    void *map;
    srapi_framebuffer_t fb;
} srapi_drm_buffer_t;

struct srapi_drm_record;

struct srapi_drm_display {
    int fd;
    uint32_t connector_id;
    uint32_t crtc_id;
    struct drm_mode_modeinfo mode;
    struct drm_mode_crtc old_crtc;
    int has_old_crtc;
    int front;
    srapi_drm_buffer_t buffers[2];
    char device_path[64];
    int tty_fd;
    int vt_nr;
    int has_saved_vt_mode;
    struct vt_mode saved_vt_mode;
    int has_saved_kd_mode;
    long saved_kd_mode;
    volatile sig_atomic_t paused;
    volatile sig_atomic_t need_resume;
    struct srapi_drm_record *recorder;
};

int srapi_drm_ioctl(int fd, unsigned long request, void *arg);
srapi_result_t srapi_drm_create_buffer(int fd, const struct drm_mode_modeinfo *mode, srapi_drm_buffer_t *buffer);
void srapi_drm_destroy_buffer(int fd, srapi_drm_buffer_t *buffer);
void srapi_drm_record_capture(srapi_drm_display_t *display);
void srapi_drm_record_close(srapi_drm_display_t *display);

#endif
