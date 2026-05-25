#define _GNU_SOURCE
#include <sprot/vk_interop.h>

#include "internal.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>
static int await_event(
    sprot_connection_t *conn,
    sprot_event_kind_t  want,
    sprot_event_t      *out,
    int                 timeout_ms)
{
    struct timespec ts0, ts1;
    int budget = timeout_ms > 0 ? timeout_ms : 1000;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        int elapsed = (int)((ts1.tv_sec - ts0.tv_sec) * 1000 +
                            (ts1.tv_nsec - ts0.tv_nsec) / 1000000);
        int remaining = budget - elapsed;
        if (remaining <= 0) {
            sprot_internal_set_error("sprot_vk: await %d timed out", (int)want);
            return -1;
        }

        sprot_event_t ev;
        int r = sprot_poll_event(conn, &ev, remaining);
        if (r < 0) return -1;
        if (r == 0) continue;

        if (ev.kind == want) {
            *out = ev;
            return 0;
        }
        if (ev.kind == SPROT_EVENT_DISCONNECT) {
            sprot_internal_set_error("sprot_vk: compositor closed connection");
            return -1;
        }
        if (ev.kind == SPROT_EVENT_ERROR) {
            sprot_internal_set_error("sprot_vk: compositor returned error %u: %s",
                                     ev.u.error.code, ev.u.error.message);
            return -1;
        }
        
        if (sprot_internal_pending_push(conn, &ev) != 0) {
            sprot_event_t drop;
            (void)sprot_internal_pending_pop(conn, &drop);
            (void)sprot_internal_pending_push(conn, &ev);
        }
    }
}

int sprot_query_render_node(
    sprot_connection_t       *conn,
    sprot_body_render_node_t *out,
    int                       timeout_ms)
{
    sprot_header_t hdr;
    sprot_event_t  ev;

    if (conn == NULL || out == NULL) {
        sprot_internal_set_error("sprot_vk: bad args to sprot_query_render_node");
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.type      = SPROT_REQ_QUERY_RENDER_NODE;
    hdr.serial    = sprot_internal_conn_next_serial(conn);
    if (sprot_send_message(sprot_internal_conn_fd(conn), &hdr, NULL, 0, -1) != 0) {
        sprot_internal_set_error("sprot_vk: send QUERY_RENDER_NODE: %s",
                                 strerror(errno));
        return -1;
    }

    if (await_event(conn, SPROT_EVENT_RENDER_NODE, &ev, timeout_ms) != 0) {
        return -1;
    }
    *out = ev.u.render_node;
    return 0;
}

int sprot_surface_attach_dmabuf(
    sprot_surface_t *surface,
    int              fd,
    uint32_t         width,
    uint32_t         height,
    uint32_t         drm_format,
    uint64_t         modifier,
    uint32_t         num_planes,
    const uint32_t  *plane_offsets,
    const uint32_t  *plane_strides,
    uint32_t         total_size)
{
    sprot_header_t                      hdr;
    sprot_body_surface_attach_dmabuf_t  body;
    sprot_connection_t                 *conn;
    uint32_t                            sid;

    if (surface == NULL || fd < 0 || width == 0 || height == 0 ||
        plane_offsets == NULL || plane_strides == NULL ||
        num_planes == 0 || num_planes > SPROT_MAX_DMABUF_PLANES) {
        sprot_internal_set_error("sprot_vk: bad args to sprot_surface_attach_dmabuf");
        return -1;
    }
    conn = sprot_internal_surface_conn(surface);
    sid  = sprot_internal_surface_id(surface);
    if (conn == NULL || sid == 0) {
        sprot_internal_set_error("sprot_vk: surface not yet acknowledged by compositor "
                                 "(wait for SURFACE_CREATED first)");
        return -1;
    }

    memset(&body, 0, sizeof(body));
    body.width      = width;
    body.height     = height;
    body.drm_format = drm_format;
    body.num_planes = num_planes;
    body.modifier   = modifier;
    body.total_size = total_size;
    for (uint32_t i = 0; i < num_planes; i++) {
        body.plane_offsets[i] = plane_offsets[i];
        body.plane_strides[i] = plane_strides[i];
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.type      = SPROT_REQ_SURFACE_ATTACH_DMABUF;
    hdr.object_id = sid;
    hdr.serial    = sprot_internal_conn_next_serial(conn);

    if (sprot_send_message(sprot_internal_conn_fd(conn), &hdr, &body, sizeof(body), fd) != 0) {
        sprot_internal_set_error("sprot_vk: send SURFACE_ATTACH_DMABUF: %s",
                                 strerror(errno));
        return -1;
    }

    sprot_internal_surface_mark_attached(surface,
                                         SPROT_BUFFER_DMABUF,
                                         SPROT_PIXEL_FORMAT_BGRA8888);
    return 0;
}
