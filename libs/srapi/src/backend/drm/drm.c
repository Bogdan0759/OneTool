#include "drm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#define SRAPI_DRM_CONNECTED 1

static int open_card(const char *device_path, char *resolved, size_t resolved_size) {
    char path[64];
    int fd;

    if (device_path != NULL) {
        if (resolved != NULL) {
            snprintf(resolved, resolved_size, "%s", device_path);
        }
        srapi_debugf("drm open card %s", device_path);
        fd = open(device_path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            srapi_set_error("drm: open %s failed: %s", device_path, strerror(errno));
        }
        return fd;
    }

    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        srapi_debugf("drm probe card %s", path);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            if (resolved != NULL) {
                snprintf(resolved, resolved_size, "%s", path);
            }
            srapi_debugf("drm card opened %s", path);
            return fd;
        }
    }
    srapi_set_error("drm: no /dev/dri/card0..7 could be opened: %s", strerror(errno));
    return -1;
}

static uint32_t mode_refresh_millihz(const struct drm_mode_modeinfo *mode) {
    uint64_t denom;
    uint64_t refresh;

    if (mode == NULL || mode->htotal == 0 || mode->vtotal == 0) {
        return 0;
    }

    denom = (uint64_t)mode->htotal * mode->vtotal;
    refresh = (uint64_t)mode->clock * 1000000u / denom;
    return refresh > UINT32_MAX ? UINT32_MAX : (uint32_t)refresh;
}

static void fill_public_mode(const struct drm_mode_modeinfo *mode, srapi_display_mode_t *out) {
    if (mode == NULL || out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->width = mode->hdisplay;
    out->height = mode->vdisplay;
    out->refresh_millihz = mode_refresh_millihz(mode);
    out->flags = mode->flags;
    out->type = mode->type;
    snprintf(out->name, sizeof(out->name), "%s", mode->name);
}

static void free_resources(uint32_t *connectors, uint32_t *crtcs, uint32_t *encoders) {
    free(connectors);
    free(crtcs);
    free(encoders);
}

static int load_resources(int fd, struct drm_mode_card_res *res, uint32_t **connectors, uint32_t **crtcs, uint32_t **encoders) {
    memset(res, 0, sizeof(*res));
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) != 0) {
        srapi_set_error("drm: GETRESOURCES failed: %s", strerror(errno));
        return 0;
    }
    srapi_debugf("drm resources connectors=%u crtcs=%u encoders=%u",
                 res->count_connectors, res->count_crtcs, res->count_encoders);

    *connectors = calloc(res->count_connectors ? res->count_connectors : 1, sizeof(uint32_t));
    *crtcs = calloc(res->count_crtcs ? res->count_crtcs : 1, sizeof(uint32_t));
    *encoders = calloc(res->count_encoders ? res->count_encoders : 1, sizeof(uint32_t));
    if (*connectors == NULL || *crtcs == NULL || *encoders == NULL) {
        srapi_set_error("drm: out of memory while loading resources");
        return 0;
    }

    res->connector_id_ptr = (uint64_t)(uintptr_t)*connectors;
    res->crtc_id_ptr = (uint64_t)(uintptr_t)*crtcs;
    res->encoder_id_ptr = (uint64_t)(uintptr_t)*encoders;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, res) != 0) {
        srapi_set_error("drm: GETRESOURCES fill failed: %s", strerror(errno));
        return 0;
    }
    return 1;
}

static int get_connector(int fd, uint32_t connector_id, struct drm_mode_get_connector *conn,
                         struct drm_mode_modeinfo **modes, uint32_t **encoders) {
    struct drm_mode_modeinfo probe_mode;
    uint32_t *props = NULL;
    uint64_t *prop_values = NULL;
    int ok = 0;

    memset(conn, 0, sizeof(*conn));
    conn->connector_id = connector_id;
    conn->count_modes = 1;
    conn->modes_ptr = (uint64_t)(uintptr_t)&probe_mode;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
        srapi_set_error("drm: connector %u probe failed: %s", connector_id, strerror(errno));
        return 0;
    }
    if (conn->count_modes == 0 || conn->count_encoders == 0) {
        srapi_debugf("drm connector %u skipped modes=%u encoders=%u",
                     connector_id, conn->count_modes, conn->count_encoders);
        return 0;
    }

    *modes = calloc(conn->count_modes ? conn->count_modes : 1, sizeof(**modes));
    *encoders = calloc(conn->count_encoders ? conn->count_encoders : 1, sizeof(**encoders));
    props = calloc(conn->count_props ? conn->count_props : 1, sizeof(*props));
    prop_values = calloc(conn->count_props ? conn->count_props : 1, sizeof(*prop_values));
    if (*modes == NULL || *encoders == NULL || props == NULL || prop_values == NULL) {
        srapi_set_error("drm: out of memory while loading connector %u", connector_id);
        goto done;
    }

    conn->modes_ptr = (uint64_t)(uintptr_t)*modes;
    conn->encoders_ptr = (uint64_t)(uintptr_t)*encoders;
    conn->props_ptr = (uint64_t)(uintptr_t)props;
    conn->prop_values_ptr = (uint64_t)(uintptr_t)prop_values;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, conn) != 0) {
        srapi_set_error("drm: connector %u fill failed: %s", connector_id, strerror(errno));
        goto done;
    }
    srapi_debugf("drm connector %u connection=%u modes=%u encoders=%u current_encoder=%u",
                 connector_id, conn->connection, conn->count_modes, conn->count_encoders, conn->encoder_id);
    ok = 1;

done:
    free(props);
    free(prop_values);
    return ok;
}

static int choose_crtc(int fd, const struct drm_mode_card_res *res, const uint32_t *crtcs,
                       const struct drm_mode_get_connector *conn, const uint32_t *encoders,
                       uint32_t *out_crtc) {
    struct drm_mode_get_encoder enc;

    if (res == NULL || crtcs == NULL || conn == NULL || encoders == NULL ||
        out_crtc == NULL || res->count_crtcs == 0) {
        srapi_set_error("drm: bad crtc selection args");
        return 0;
    }

    if (conn->encoder_id != 0) {
        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = conn->encoder_id;
        if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) == 0) {
            srapi_debugf("drm current encoder=%u crtc=%u possible=0x%x",
                         enc.encoder_id, enc.crtc_id, enc.possible_crtcs);
            if (enc.crtc_id != 0) {
                for (uint32_t i = 0; i < res->count_crtcs; i++) {
                    if (crtcs[i] == enc.crtc_id) {
                        *out_crtc = enc.crtc_id;
                        srapi_debugf("drm choose current crtc=%u from encoder=%u",
                                     *out_crtc, enc.encoder_id);
                        return 1;
                    }
                }
                srapi_debugf("drm current crtc=%u is not in resource crtc list", enc.crtc_id);
            }
        } else {
            srapi_debugf("drm current encoder %u get failed: %s",
                         conn->encoder_id, strerror(errno));
        }
    }

    for (uint32_t i = 0; i < conn->count_encoders; i++) {
        memset(&enc, 0, sizeof(enc));
        enc.encoder_id = encoders[i];
        if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &enc) != 0) {
            srapi_debugf("drm encoder %u get failed: %s", encoders[i], strerror(errno));
            continue;
        }
        srapi_debugf("drm encoder[%u]=%u crtc=%u possible=0x%x",
                     i, enc.encoder_id, enc.crtc_id, enc.possible_crtcs);
        for (uint32_t crtc_index = 0; crtc_index < res->count_crtcs; crtc_index++) {
            uint32_t bit = crtc_index < 32 ? (1u << crtc_index) : 0;

            srapi_debugf("drm crtc[%u]=%u bit=0x%x allowed=%u",
                         crtc_index, crtcs[crtc_index], bit,
                         bit != 0 && (enc.possible_crtcs & bit) != 0);
            if (bit != 0 && (enc.possible_crtcs & bit) != 0) {
                *out_crtc = crtcs[crtc_index];
                srapi_debugf("drm choose crtc=%u from encoder=%u possible=0x%x",
                             *out_crtc, enc.encoder_id, enc.possible_crtcs);
                return 1;
            }
        }
    }

    if (res->count_crtcs > 0) {
        *out_crtc = crtcs[0];
        srapi_debugf("drm fallback choose first resource crtc=%u", *out_crtc);
        return 1;
    }

    srapi_set_error("drm: no usable crtc found for connector=%u encoders=%u",
                    conn->connector_id, conn->count_encoders);
    return 0;
}

static int mode_matches(
    const struct drm_mode_modeinfo *mode,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_millihz
) {
    uint32_t mode_refresh;

    if (mode == NULL) {
        return 0;
    }
    if (width != 0 && mode->hdisplay != width) {
        return 0;
    }
    if (height != 0 && mode->vdisplay != height) {
        return 0;
    }
    if (refresh_millihz == 0) {
        return 1;
    }

    mode_refresh = mode_refresh_millihz(mode);
    return mode_refresh + 500 >= refresh_millihz && refresh_millihz + 500 >= mode_refresh;
}

static const struct drm_mode_modeinfo *choose_mode(
    const struct drm_mode_modeinfo *modes,
    uint32_t mode_count,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_millihz
) {
    const struct drm_mode_modeinfo *preferred = NULL;

    if (modes == NULL || mode_count == 0) {
        return NULL;
    }

    for (uint32_t i = 0; i < mode_count; i++) {
        if (mode_matches(&modes[i], width, height, refresh_millihz)) {
            return &modes[i];
        }
        if (preferred == NULL && (modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0) {
            preferred = &modes[i];
        }
    }

    if (width == 0 && height == 0 && refresh_millihz == 0) {
        return preferred != NULL ? preferred : &modes[0];
    }
    return NULL;
}

static int find_output(
    int fd,
    uint32_t wanted_connector_id,
    uint32_t wanted_width,
    uint32_t wanted_height,
    uint32_t wanted_refresh,
    uint32_t *connector_id,
    uint32_t *crtc_id,
    struct drm_mode_modeinfo *mode
) {
    struct drm_mode_card_res res;
    uint32_t *connectors = NULL;
    uint32_t *crtcs = NULL;
    uint32_t *res_encoders = NULL;
    int ok = 0;

    if (!load_resources(fd, &res, &connectors, &crtcs, &res_encoders)) {
        free_resources(connectors, crtcs, res_encoders);
        return 0;
    }

    for (uint32_t i = 0; i < res.count_connectors && !ok; i++) {
        struct drm_mode_get_connector conn;
        struct drm_mode_modeinfo *modes = NULL;
        uint32_t *encoders = NULL;

        if (!get_connector(fd, connectors[i], &conn, &modes, &encoders)) {
            free(modes);
            free(encoders);
            continue;
        }
        const struct drm_mode_modeinfo *chosen = choose_mode(
            modes,
            conn.count_modes,
            wanted_width,
            wanted_height,
            wanted_refresh
        );

        if (wanted_connector_id != 0 && conn.connector_id != wanted_connector_id) {
            free(modes);
            free(encoders);
            continue;
        }
        if (conn.connection != SRAPI_DRM_CONNECTED) {
            srapi_debugf("drm connector=%u skipped: not connected connection=%u",
                         conn.connector_id, conn.connection);
        } else if (chosen == NULL) {
            srapi_debugf("drm connector=%u skipped: no mode matching %ux%u refresh=%u",
                         conn.connector_id, wanted_width, wanted_height, wanted_refresh);
        }
        if (conn.connection == SRAPI_DRM_CONNECTED && chosen != NULL &&
            choose_crtc(fd, &res, crtcs, &conn, encoders, crtc_id)) {
            *connector_id = conn.connector_id;
            *mode = *chosen;
            srapi_debugf("drm output connector=%u crtc=%u mode=%s %ux%u refresh=%u",
                         *connector_id, *crtc_id, mode->name, mode->hdisplay,
                         mode->vdisplay, mode_refresh_millihz(mode));
            ok = 1;
        }
        free(modes);
        free(encoders);
    }

    free_resources(connectors, crtcs, res_encoders);
    if (!ok) {
        srapi_set_error("drm: no connected connector with usable CRTC found");
    }
    return ok;
}

srapi_result_t srapi_drm_open(const char *device_path, srapi_drm_display_t **out) {
    srapi_drm_display_desc_t desc;

    memset(&desc, 0, sizeof(desc));
    desc.device_path = device_path;
    return srapi_drm_open_display(&desc, out);
}

static srapi_result_t drm_set_crtc(
    srapi_drm_display_t *display,
    const struct drm_mode_modeinfo *mode,
    uint32_t fb_id
) {
    uint64_t connector_ptr;
    struct drm_mode_crtc set;

    connector_ptr = (uint64_t)(uintptr_t)&display->connector_id;
    memset(&set, 0, sizeof(set));
    set.set_connectors_ptr = connector_ptr;
    set.count_connectors = 1;
    set.crtc_id = display->crtc_id;
    set.fb_id = fb_id;
    set.mode_valid = 1;
    set.mode = *mode;

    if (srapi_drm_ioctl(display->fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
        srapi_set_error("drm: SETCRTC crtc=%u connector=%u fb=%u mode=%ux%u failed: %s",
                        display->crtc_id, display->connector_id, fb_id,
                        mode->hdisplay, mode->vdisplay, strerror(errno));
        return SRAPI_ERROR;
    }
    return SRAPI_OK;
}

srapi_result_t srapi_drm_open_display(
    const srapi_drm_display_desc_t *desc,
    srapi_drm_display_t **out
) {
    struct srapi_drm_display *display;
    struct drm_mode_modeinfo mode;
    char resolved[64];
    int fd;

    if (out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    if (desc == NULL) {
        static const srapi_drm_display_desc_t default_desc;
        desc = &default_desc;
    }

    fd = open_card(desc->device_path, resolved, sizeof(resolved));
    if (fd < 0) {
        return SRAPI_ERROR;
    }

    display = calloc(1, sizeof(*display));
    if (display == NULL) {
        srapi_set_error("drm: out of memory while creating display");
        close(fd);
        return SRAPI_ERROR_OOM;
    }
    display->fd = fd;
    snprintf(display->device_path, sizeof(display->device_path), "%s", resolved);

    if (!find_output(fd, desc->connector_id, desc->width, desc->height, desc->refresh_millihz,
                     &display->connector_id, &display->crtc_id, &mode)) {
        srapi_drm_close(display);
        return SRAPI_ERROR;
    }
    display->mode = mode;

    if (srapi_drm_create_buffer(fd, &display->mode, &display->buffers[0]) != SRAPI_OK ||
        srapi_drm_create_buffer(fd, &display->mode, &display->buffers[1]) != SRAPI_OK) {
        srapi_drm_close(display);
        return SRAPI_ERROR;
    }
    display->front = 0;

    memset(&display->old_crtc, 0, sizeof(display->old_crtc));
    display->old_crtc.crtc_id = display->crtc_id;
    if (srapi_drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &display->old_crtc) == 0) {
        display->has_old_crtc = 1;
        srapi_debugf("drm saved old crtc=%u fb=%u", display->old_crtc.crtc_id, display->old_crtc.fb_id);
    }

    if (drm_set_crtc(display, &mode, display->buffers[display->front].fb_id) != SRAPI_OK) {
        srapi_drm_close(display);
        return SRAPI_ERROR;
    }

    srapi_debugf("drm setcrtc ok crtc=%u connector=%u fb=%u", display->crtc_id, display->connector_id, display->buffers[display->front].fb_id);
    *out = display;
    return SRAPI_OK;
}

void srapi_drm_close(srapi_drm_display_t *display) {
    if (display == NULL) {
        return;
    }

    if (display->fd >= 0 && display->has_old_crtc) {
        srapi_debugf("drm restore old crtc=%u fb=%u", display->old_crtc.crtc_id, display->old_crtc.fb_id);
        srapi_drm_ioctl(display->fd, DRM_IOCTL_MODE_SETCRTC, &display->old_crtc);
    }
    srapi_drm_destroy_buffer(display->fd, &display->buffers[0]);
    srapi_drm_destroy_buffer(display->fd, &display->buffers[1]);
    if (display->fd >= 0) {
        srapi_debugf("drm close fd");
        close(display->fd);
    }
    free(display);
}

srapi_framebuffer_t *srapi_drm_framebuffer(srapi_drm_display_t *display) {
    return srapi_drm_backbuffer(display);
}

srapi_framebuffer_t *srapi_drm_backbuffer(srapi_drm_display_t *display) {
    if (display == NULL) {
        return NULL;
    }
    return &display->buffers[display->front ^ 1].fb;
}

static int wait_flip_event(int fd) {
    struct pollfd pfd;
    unsigned char buf[256];

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int rc = poll(&pfd, 1, 3000);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc <= 0) {
            srapi_set_error("drm: page flip wait timed out");
            return 0;
        }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < (ssize_t)sizeof(struct drm_event)) {
            srapi_set_error("drm: page flip event read failed: %s", n < 0 ? strerror(errno) : "short event");
            return 0;
        }

        size_t off = 0;
        while (off + sizeof(struct drm_event) <= (size_t)n) {
            struct drm_event *ev = (struct drm_event *)(void *)(buf + off);
            if (ev->length == 0 || off + ev->length > (size_t)n) {
                srapi_set_error("drm: bad event length");
                return 0;
            }
            if (ev->type == DRM_EVENT_FLIP_COMPLETE) {
                return 1;
            }
            off += ev->length;
        }
    }
}

srapi_result_t srapi_drm_present(srapi_drm_display_t *display) {
    uint64_t connector_ptr;
    int next;

    if (display == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    next = display->front ^ 1;
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = display->crtc_id,
        .fb_id = display->buffers[next].fb_id,
        .flags = DRM_MODE_PAGE_FLIP_EVENT,
        .user_data = 0,
    };

    if (srapi_drm_ioctl(display->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
        if (!wait_flip_event(display->fd)) {
            return SRAPI_ERROR;
        }
        display->front = next;
        srapi_debugf("drm page_flip present fb=%u front=%d", display->buffers[display->front].fb_id, display->front);
        return SRAPI_OK;
    }

    srapi_debugf("drm page_flip failed, fallback SETCRTC: %s", strerror(errno));

    connector_ptr = (uint64_t)(uintptr_t)&display->connector_id;
    struct drm_mode_crtc set = {
        .set_connectors_ptr = connector_ptr,
        .count_connectors = 1,
        .crtc_id = display->crtc_id,
        .fb_id = display->buffers[next].fb_id,
        .mode_valid = 1,
        .mode = display->mode,
    };

    if (srapi_drm_ioctl(display->fd, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
        srapi_set_error("drm: present SETCRTC crtc=%u connector=%u fb=%u failed: %s",
                        display->crtc_id, display->connector_id, display->buffers[next].fb_id, strerror(errno));
        return SRAPI_ERROR;
    }

    display->front = next;
    srapi_debugf("drm present fb=%u front=%d", display->buffers[display->front].fb_id, display->front);
    return SRAPI_OK;
}

uint32_t srapi_drm_width(const srapi_drm_display_t *display) {
    return display ? display->buffers[display->front].fb.width : 0;
}

uint32_t srapi_drm_height(const srapi_drm_display_t *display) {
    return display ? display->buffers[display->front].fb.height : 0;
}

const char *srapi_drm_device_path(const srapi_drm_display_t *display) {
    return display != NULL ? display->device_path : "";
}

srapi_result_t srapi_drm_current_mode(
    const srapi_drm_display_t *display,
    srapi_display_mode_t *out
) {
    if (display == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    fill_public_mode(&display->mode, out);
    return SRAPI_OK;
}

srapi_result_t srapi_drm_set_mode(
    srapi_drm_display_t *display,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_millihz
) {
    struct drm_mode_modeinfo mode;
    uint32_t connector_id;
    uint32_t crtc_id;
    srapi_drm_buffer_t new_buffers[2];

    if (display == NULL || width == 0 || height == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    if (!find_output(display->fd, display->connector_id, width, height, refresh_millihz,
                     &connector_id, &crtc_id, &mode)) {
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memset(new_buffers, 0, sizeof(new_buffers));
    if (srapi_drm_create_buffer(display->fd, &mode, &new_buffers[0]) != SRAPI_OK ||
        srapi_drm_create_buffer(display->fd, &mode, &new_buffers[1]) != SRAPI_OK) {
        srapi_drm_destroy_buffer(display->fd, &new_buffers[0]);
        srapi_drm_destroy_buffer(display->fd, &new_buffers[1]);
        return SRAPI_ERROR;
    }

    display->crtc_id = crtc_id;
    if (drm_set_crtc(display, &mode, new_buffers[0].fb_id) != SRAPI_OK) {
        srapi_drm_destroy_buffer(display->fd, &new_buffers[0]);
        srapi_drm_destroy_buffer(display->fd, &new_buffers[1]);
        return SRAPI_ERROR;
    }

    srapi_drm_destroy_buffer(display->fd, &display->buffers[0]);
    srapi_drm_destroy_buffer(display->fd, &display->buffers[1]);
    display->buffers[0] = new_buffers[0];
    display->buffers[1] = new_buffers[1];
    display->mode = mode;
    display->front = 0;
    srapi_debugf("drm mode set connector=%u crtc=%u mode=%s %ux%u refresh=%u",
                 display->connector_id, display->crtc_id, display->mode.name,
                 display->mode.hdisplay, display->mode.vdisplay,
                 mode_refresh_millihz(&display->mode));
    return SRAPI_OK;
}
