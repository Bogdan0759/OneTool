#include "internal.h"

#include <drm/drm.h>

#include <fcntl.h>
#include <string.h>

static srapi_result_t export_dmabuf_fd(int drm_fd, uint32_t handle, int *out_fd, const char *what) {
    struct drm_prime_handle prime;

    if (out_fd != NULL) *out_fd = -1;
    if (out_fd == NULL || drm_fd < 0 || handle == 0) {
        srapi_set_error("%s: bad export args", what);
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(&prime, 0, sizeof(prime));
    prime.handle = handle;
    prime.flags = DRM_CLOEXEC;
    if (srapi_drm_ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
        srapi_set_error("%s: PRIME_HANDLE_TO_FD handle=%u failed", what, handle);
        return SRAPI_ERROR;
    }

    *out_fd = prime.fd;
    return SRAPI_OK;
}

srapi_result_t srapi_buffer_export_dmabuf(const srapi_buffer_t *buffer, int *out_fd) {
    if (out_fd != NULL) *out_fd = -1;
    if (buffer == NULL || out_fd == NULL) {
        srapi_set_error("buffer export: bad args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (buffer->device == NULL || buffer->device->fd < 0 || buffer->gpu_handle == 0) {
        srapi_set_error("buffer export: backend has no exportable gpu handle");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    return export_dmabuf_fd(buffer->device->fd, buffer->gpu_handle, out_fd, "buffer export");
}

srapi_result_t srapi_image_export_dmabuf(const srapi_image_t *image, int *out_fd) {
    if (out_fd != NULL) *out_fd = -1;
    if (image == NULL || out_fd == NULL) {
        srapi_set_error("image export: bad args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (image->device == NULL || image->device->fd < 0 || image->gpu_handle == 0) {
        srapi_set_error("image export: backend has no exportable gpu handle");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    return export_dmabuf_fd(image->device->fd, image->gpu_handle, out_fd, "image export");
}

srapi_result_t srapi_framebuffer_export_dmabuf(const srapi_framebuffer_t *fb, int *out_fd) {
    if (out_fd != NULL) *out_fd = -1;
    if (fb == NULL || out_fd == NULL) {
        srapi_set_error("framebuffer export: bad args");
        return SRAPI_ERROR_BAD_ARG;
    }
    if (fb->gpu_fd < 0 || fb->gpu_handle == 0) {
        srapi_set_error("framebuffer export: framebuffer is not GPU-backed");
        return SRAPI_ERROR_UNSUPPORTED;
    }
    return export_dmabuf_fd(fb->gpu_fd, fb->gpu_handle, out_fd, "framebuffer export");
}
