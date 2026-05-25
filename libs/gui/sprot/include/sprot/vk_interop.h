#ifndef ONETOOL_LIBS_GUI_SPROT_VK_INTEROP_H
#define ONETOOL_LIBS_GUI_SPROT_VK_INTEROP_H
#include <sprot/client.h>
#include <sprot/sprot.h>
#include <stdint.h>
int sprot_query_render_node(
    sprot_connection_t       *conn,
    sprot_body_render_node_t *out,
    int                       timeout_ms);
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
    uint32_t         total_size);

#endif
