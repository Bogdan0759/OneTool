#include "internal.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define SRAPI_DRM_CONNECTED 1

static int drm_ioctl(int fd, unsigned long request, void *arg) {
    int rc;

    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static int open_card(const char *device_path, char *resolved, size_t resolved_size) {
    char path[64];
    int fd;

    if (device_path != NULL) {
        snprintf(resolved, resolved_size, "%s", device_path);
        return open(device_path, O_RDWR | O_CLOEXEC);
    }

    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            snprintf(resolved, resolved_size, "%s", path);
            return fd;
        }
    }
    snprintf(resolved, resolved_size, "%s", "/dev/dri/card?");
    return -1;
}

srapi_result_t srapi_display_probe_drm(
    const char *device_path,
    srapi_display_info_t *out,
    size_t out_count,
    size_t *written
) {
    struct drm_mode_card_res res;
    uint32_t *connectors = NULL;
    uint32_t *crtcs = NULL;
    uint32_t *encoders_res = NULL;
    uint32_t *fbs = NULL;
    char device[64];
    int fd;
    size_t count = 0;

    if (written != NULL) {
        *written = 0;
    }
    if (out == NULL && out_count != 0) {
        return SRAPI_ERROR_BAD_ARG;
    }

    fd = open_card(device_path, device, sizeof(device));
    if (fd < 0) {
        srapi_set_error("display: open drm card failed: %s", strerror(errno));
        return SRAPI_ERROR;
    }
    srapi_debugf("display: drm probe device=%s fd=%d", device, fd);

    memset(&res, 0, sizeof(res));
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        srapi_set_error("display: GETRESOURCES failed: %s", strerror(errno));
        close(fd);
        return SRAPI_ERROR;
    }
    srapi_debugf("display: resources counts fbs=%u crtcs=%u connectors=%u encoders=%u min=%ux%u max=%ux%u",
                 res.count_fbs,
                 res.count_crtcs,
                 res.count_connectors,
                 res.count_encoders,
                 res.min_width,
                 res.min_height,
                 res.max_width,
                 res.max_height);

    connectors = calloc(res.count_connectors ? res.count_connectors : 1, sizeof(*connectors));
    crtcs = calloc(res.count_crtcs ? res.count_crtcs : 1, sizeof(*crtcs));
    encoders_res = calloc(res.count_encoders ? res.count_encoders : 1, sizeof(*encoders_res));
    fbs = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(*fbs));
    if (connectors == NULL || crtcs == NULL || encoders_res == NULL || fbs == NULL) {
        srapi_set_error("display: out of memory");
        free(connectors);
        free(crtcs);
        free(encoders_res);
        free(fbs);
        close(fd);
        return SRAPI_ERROR_OOM;
    }

    res.fb_id_ptr = 0;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encoders_res;
    res.count_fbs = 0;
    srapi_debugf("display: resources ptrs fbs=%p crtcs=%p connectors=%p encoders=%p",
                 NULL,
                 (void *)crtcs,
                 (void *)connectors,
                 (void *)encoders_res);
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        srapi_debugf("display: GETRESOURCES full fill failed: %s", strerror(errno));
        memset(&res, 0, sizeof(res));
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
            srapi_set_error("display: GETRESOURCES retry failed: %s", strerror(errno));
            free(connectors);
            free(crtcs);
            free(encoders_res);
            free(fbs);
            close(fd);
            return SRAPI_ERROR;
        }
        res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
        res.count_crtcs = 0;
        res.count_encoders = 0;
        res.count_fbs = 0;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
            srapi_set_error("display: GETRESOURCES connector-only fill failed: %s", strerror(errno));
            free(connectors);
            free(crtcs);
            free(encoders_res);
            free(fbs);
            close(fd);
            return SRAPI_ERROR;
        }
        srapi_debugf("display: fallback connector-only fill connectors=%u", res.count_connectors);
    }

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        struct drm_mode_modeinfo probe_mode;
        struct drm_mode_modeinfo *modes = NULL;
        uint32_t *encoders = NULL;
        uint32_t *props = NULL;
        uint64_t *prop_values = NULL;

        memset(&conn, 0, sizeof(conn));
        conn.connector_id = connectors[i];
        conn.count_modes = 1;
        conn.modes_ptr = (uint64_t)(uintptr_t)&probe_mode;
        srapi_debugf("display: connector[%u] id=%u probe", i, connectors[i]);
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0) {
            srapi_debugf("display: connector %u probe failed: %s", connectors[i], strerror(errno));
            continue;
        }
        srapi_debugf("display: connector %u counts modes=%u props=%u encoders=%u connection=%u",
                     connectors[i],
                     conn.count_modes,
                     conn.count_props,
                     conn.count_encoders,
                     conn.connection);

        modes = calloc(conn.count_modes ? conn.count_modes : 1, sizeof(*modes));
        encoders = calloc(conn.count_encoders ? conn.count_encoders : 1, sizeof(*encoders));
        props = calloc(conn.count_props ? conn.count_props : 1, sizeof(*props));
        prop_values = calloc(conn.count_props ? conn.count_props : 1, sizeof(*prop_values));
        if (modes == NULL || encoders == NULL || props == NULL || prop_values == NULL) {
            free(modes);
            free(encoders);
            free(props);
            free(prop_values);
            continue;
        }

        conn.modes_ptr = (uint64_t)(uintptr_t)modes;
        conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        conn.props_ptr = (uint64_t)(uintptr_t)props;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)prop_values;
        srapi_debugf("display: connector %u ptrs modes=%p encoders=%p props=%p prop_values=%p",
                     connectors[i],
                     (void *)modes,
                     (void *)encoders,
                     (void *)props,
                     (void *)prop_values);
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) == 0) {
            if (count < out_count) {
                srapi_display_info_t *info = &out[count];

                memset(info, 0, sizeof(*info));
                snprintf(info->device, sizeof(info->device), "%s", device);
                info->connector_id = conn.connector_id;
                info->connector_type = conn.connector_type;
                info->connected = conn.connection == SRAPI_DRM_CONNECTED;
                info->mode_count = conn.count_modes;
                if (conn.count_modes > 0) {
                    info->preferred_width = modes[0].hdisplay;
                    info->preferred_height = modes[0].vdisplay;
                    snprintf(info->preferred_mode, sizeof(info->preferred_mode), "%s", modes[0].name);
                }
            }
            count++;
        } else {
            srapi_debugf("display: connector %u fill failed: %s", connectors[i], strerror(errno));
        }

        free(modes);
        free(encoders);
        free(props);
        free(prop_values);
    }

    free(connectors);
    free(crtcs);
    free(encoders_res);
    free(fbs);
    close(fd);
    if (written != NULL) {
        *written = count;
    }
    return SRAPI_OK;
}
