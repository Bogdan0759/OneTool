#include <sprot/srapi_bridge.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int wait_surface_ready(sprot_connection_t *conn) {
    sprot_event_t ev;

    for (int tries = 0; tries < 80; tries++) {
        int r = sprot_poll_event(conn, &ev, 50);
        if (r < 0) return -1;
        if (r == 0) continue;
        if (ev.kind == SPROT_EVENT_SURFACE_CREATED) return 0;
        if (ev.kind == SPROT_EVENT_DISCONNECT) break;
    }
    return -1;
}

static int copy_rows(
    uint32_t *dst,
    uint32_t dst_pitch,
    const uint32_t *src,
    uint32_t src_pitch,
    uint32_t width,
    uint32_t height
) {
    uint32_t dst_pitch_px = dst_pitch / 4u;
    uint32_t src_pitch_px = src_pitch / 4u;

    if (dst == NULL || src == NULL || dst_pitch_px < width || src_pitch_px < width) {
        return -1;
    }

    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst + y * dst_pitch_px, src + y * src_pitch_px, (size_t)width * 4u);
    }
    return 0;
}

static int present_dmabuf(
    sprot_surface_t *surface,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t buffer_size,
    int fd
) {
    int rc = sprot_attach_fd(
        surface,
        fd,
        width,
        height,
        stride,
        buffer_size,
        SPROT_BUFFER_DMABUF,
        SPROT_PIXEL_FORMAT_BGRA8888
    );
    close(fd);
    if (rc != 0) return -1;
    return sprot_commit(surface);
}

sprot_surface_t *sprot_create_surface_for_framebuffer(
    sprot_connection_t *conn,
    srapi_framebuffer_t *fb,
    const char *title
) {
    sprot_surface_t *surface;

    if (conn == NULL || fb == NULL) {
        return NULL;
    }

    surface = sprot_create_surface(conn, srapi_framebuffer_width(fb), srapi_framebuffer_height(fb));
    if (surface == NULL) return NULL;
    if (wait_surface_ready(conn) != 0) {
        sprot_destroy_surface(surface);
        return NULL;
    }
    if (title != NULL && *title != '\0') {
        sprot_set_title(surface, title);
    }
    return surface;
}

int sprot_present_framebuffer(sprot_surface_t *surface, srapi_framebuffer_t *fb) {
    int fd = -1;
    uint32_t *dst;
    uint32_t *src;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;

    if (surface == NULL || fb == NULL) {
        return -1;
    }

    width = srapi_framebuffer_width(fb);
    height = srapi_framebuffer_height(fb);
    pitch = srapi_framebuffer_pitch(fb);
    if (width != sprot_surface_width(surface) || height != sprot_surface_height(surface)) {
        return -1;
    }

    if (srapi_framebuffer_export_dmabuf(fb, &fd) == SRAPI_OK && fd >= 0) {
        return present_dmabuf(surface, width, height, pitch, pitch * height, fd);
    }

    dst = sprot_surface_pixels(surface);
    src = srapi_framebuffer_pixels(fb);
    if (copy_rows(dst, sprot_surface_stride(surface), src, pitch, width, height) != 0) {
        return -1;
    }
    return sprot_commit(surface);
}

sprot_surface_t *sprot_create_surface_for_image(
    sprot_connection_t *conn,
    srapi_image_t *image,
    const char *title
) {
    sprot_surface_t *surface;

    if (conn == NULL || image == NULL) {
        return NULL;
    }

    surface = sprot_create_surface(conn, srapi_image_width(image), srapi_image_height(image));
    if (surface == NULL) return NULL;
    if (wait_surface_ready(conn) != 0) {
        sprot_destroy_surface(surface);
        return NULL;
    }
    if (title != NULL && *title != '\0') {
        sprot_set_title(surface, title);
    }
    return surface;
}

int sprot_present_image(sprot_surface_t *surface, srapi_image_t *image) {
    int fd = -1;
    void *map = NULL;
    uint32_t pitch = 0;
    uint32_t *dst;
    uint32_t width;
    uint32_t height;
    int rc;

    if (surface == NULL || image == NULL) {
        return -1;
    }

    width = srapi_image_width(image);
    height = srapi_image_height(image);
    if (width != sprot_surface_width(surface) || height != sprot_surface_height(surface)) {
        return -1;
    }

    if (srapi_image_export_dmabuf(image, &fd) == SRAPI_OK && fd >= 0) {
        return present_dmabuf(surface, width, height, srapi_image_pitch(image), srapi_image_pitch(image) * height, fd);
    }

    rc = srapi_image_map(image, &map, &pitch);
    if (rc != SRAPI_OK) {
        errno = EINVAL;
        return -1;
    }

    dst = sprot_surface_pixels(surface);
    rc = copy_rows(dst, sprot_surface_stride(surface), (const uint32_t *)map, pitch, width, height);
    srapi_image_unmap(image);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }
    return sprot_commit(surface);
}
