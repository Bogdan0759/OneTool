#ifndef ONETOOL_LIBS_GUI_SPROT_INTERNAL_H
#define ONETOOL_LIBS_GUI_SPROT_INTERNAL_H

#include <sprot/client.h>
#include <sprot/sprot.h>
#include <stdint.h>

void sprot_internal_set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int      sprot_internal_conn_fd(const sprot_connection_t *conn);
uint32_t sprot_internal_conn_next_serial(sprot_connection_t *conn);
sprot_connection_t *sprot_internal_surface_conn(sprot_surface_t *surface);
uint32_t            sprot_internal_surface_id(const sprot_surface_t *surface);
void                sprot_internal_surface_mark_attached(sprot_surface_t *surface,uint32_t kind, uint32_t format);
int                 sprot_internal_surface_dmabuf_matches(
    sprot_surface_t *surface,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t drm_format,
    uint64_t modifier,
    uint32_t num_planes,
    const uint32_t *plane_offsets,
    const uint32_t *plane_strides,
    uint32_t total_size);
void                sprot_internal_surface_mark_dmabuf_attached(
    sprot_surface_t *surface,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t drm_format,
    uint64_t modifier,
    uint32_t num_planes,
    const uint32_t *plane_offsets,
    const uint32_t *plane_strides,
    uint32_t total_size);
int  sprot_internal_pending_push(sprot_connection_t *conn, const sprot_event_t *ev);
int  sprot_internal_pending_pop(sprot_connection_t *conn, sprot_event_t *out);

#endif
