#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static const char *default_fbdev_path(void) {
    return "/dev/fb0";
}

static int fbdev_ioctl(int fd, unsigned long request, void *arg) {
    int rc;

    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static int fbdev_load_info(int fd, struct fb_fix_screeninfo *fix, struct fb_var_screeninfo *var) {
    if (fbdev_ioctl(fd, FBIOGET_FSCREENINFO, fix) != 0) {
        srapi_set_error("fbdev: FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        return 0;
    }
    if (fbdev_ioctl(fd, FBIOGET_VSCREENINFO, var) != 0) {
        srapi_set_error("fbdev: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        return 0;
    }
    if (var->bits_per_pixel != 32) {
        srapi_set_error("fbdev: only 32bpp framebuffers are supported, got %u", var->bits_per_pixel);
        return 0;
    }
    if (fix->line_length == 0 || var->xres == 0 || var->yres == 0 || fix->smem_len == 0) {
        srapi_set_error("fbdev: bad screen info %ux%u pitch=%u size=%u",
                        var->xres, var->yres, fix->line_length, fix->smem_len);
        return 0;
    }
    return 1;
}

srapi_result_t srapi_fbdev_probe(srapi_device_info_t *out) {
    const char *path = default_fbdev_path();
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    int fd;

    if (out != NULL) {
        out->backend = SRAPI_BACKEND_FBDEV;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (out != NULL) {
            snprintf(out->message, sizeof(out->message), "no usable /dev/fb0");
        }
        srapi_set_error("fbdev: open %s failed: %s", path, strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    if (!fbdev_load_info(fd, &fix, &var)) {
        close(fd);
        return SRAPI_ERROR_UNSUPPORTED;
    }

    close(fd);
    if (out != NULL) {
        out->available = 1;
        snprintf(out->path, sizeof(out->path), "%s", path);
        snprintf(out->message, sizeof(out->message), "fbdev %ux%u pitch=%u",
                 var.xres, var.yres, fix.line_length);
    }
    srapi_debugf("fbdev probe ok path=%s %ux%u pitch=%u",
                 path, var.xres, var.yres, fix.line_length);
    return SRAPI_OK;
}

srapi_result_t srapi_fbdev_open(const char *device_path, srapi_fbdev_display_t **out) {
    srapi_fbdev_display_desc_t desc;

    desc.device_path = device_path;
    return srapi_fbdev_open_display(&desc, out);
}

srapi_result_t srapi_fbdev_open_display(
    const srapi_fbdev_display_desc_t *desc,
    srapi_fbdev_display_t **out
) {
    const char *path;
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    srapi_fbdev_display_t *display;
    int fd;

    if (out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    path = (desc != NULL && desc->device_path != NULL && desc->device_path[0] != '\0')
        ? desc->device_path
        : default_fbdev_path();

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        srapi_set_error("fbdev: open %s failed: %s", path, strerror(errno));
        return SRAPI_ERROR;
    }

    if (!fbdev_load_info(fd, &fix, &var)) {
        close(fd);
        return SRAPI_ERROR_UNSUPPORTED;
    }

    display = calloc(1, sizeof(*display));
    if (display == NULL) {
        close(fd);
        return SRAPI_ERROR_OOM;
    }

    display->map = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (display->map == MAP_FAILED) {
        srapi_set_error("fbdev: mmap %s failed: %s", path, strerror(errno));
        display->map = NULL;
        free(display);
        close(fd);
        return SRAPI_ERROR;
    }

    display->fd = fd;
    display->map_size = fix.smem_len;
    snprintf(display->device_path, sizeof(display->device_path), "%s", path);
    display->fb.width = var.xres;
    display->fb.height = var.yres;
    display->fb.pitch = fix.line_length;
    display->fb.pixels = display->map;
    display->fb.owns_pixels = 0;
    display->fb.backend = SRAPI_BACKEND_FBDEV;

    display->device = calloc(1, sizeof(srapi_device_t));
    if (display->device == NULL) {
        munmap(display->map, fix.smem_len);
        free(display);
        close(fd);
        return SRAPI_ERROR_OOM;
    }
    display->device->backend = SRAPI_BACKEND_FBDEV;
    snprintf(display->device->path, sizeof(display->device->path), "%s", path);
    display->device->fd = fd;
    display->device->tile_cache_enabled = 0;
    memset(display->device->tile_hashes, 0, sizeof(display->device->tile_hashes));

    display->fb.device = display->device;

    *out = display;
    srapi_debugf("fbdev open path=%s %ux%u pitch=%u size=%llu",
                 display->device_path, display->fb.width, display->fb.height,
                 display->fb.pitch, (unsigned long long)display->map_size);
    return SRAPI_OK;
}

void srapi_fbdev_close(srapi_fbdev_display_t *display) {
    if (display == NULL) {
        return;
    }
    srapi_debugf("fbdev close path=%s", display->device_path);
    if (display->map != NULL && display->map_size > 0) {
        munmap(display->map, (size_t)display->map_size);
    }
    if (display->fd >= 0) {
        close(display->fd);
    }
    if (display->device != NULL) {
        free(display->device);
    }
    free(display);
}

srapi_framebuffer_t *srapi_fbdev_framebuffer(srapi_fbdev_display_t *display) {
    return display != NULL ? &display->fb : NULL;
}

srapi_result_t srapi_fbdev_present(srapi_fbdev_display_t *display) {
    if (display == NULL || display->map == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (msync(display->map, (size_t)display->map_size, MS_ASYNC) != 0) {
        srapi_set_error("fbdev: msync failed: %s", strerror(errno));
        return SRAPI_ERROR;
    }
    return SRAPI_OK;
}

uint32_t srapi_fbdev_width(const srapi_fbdev_display_t *display) {
    return display != NULL ? display->fb.width : 0;
}

uint32_t srapi_fbdev_height(const srapi_fbdev_display_t *display) {
    return display != NULL ? display->fb.height : 0;
}

srapi_result_t srapi_fbdev_set_tile_cache_enabled(srapi_fbdev_display_t *display, uint32_t enabled) {
    if (display == NULL || display->device == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    return srapi_device_set_tile_cache_enabled(display->device, enabled);
}

uint32_t srapi_fbdev_tile_cache_enabled(const srapi_fbdev_display_t *display) {
    if (display == NULL || display->device == NULL) {
        return 0;
    }
    return srapi_device_tile_cache_enabled(display->device);
}
