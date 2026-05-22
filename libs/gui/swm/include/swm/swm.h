#ifndef ONETOOL_LIBS_GUI_SWM_INTERNAL_H
#define ONETOOL_LIBS_GUI_SWM_INTERNAL_H

#include <srapi/srapi.h>
#include <sprot/sprot.h>

#include <stdint.h>
#include <sys/types.h>

#define SWM_MAX_CLIENTS  16
#define SWM_MAX_SURFACES 32

typedef struct swm_client swm_client_t;

typedef struct swm_surface {
    int in_use;
    uint32_t id;
    uint32_t client_handle;
    swm_client_t *owner;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t buffer_size;
    int buf_fd;
    void *buf_map;
    int has_pending;
    int committed;
    int32_t pos_x;
    int32_t pos_y;
} swm_surface_t;

struct swm_client {
    int in_use;
    int sock;
    int has_hello;
    swm_surface_t *surfaces[8];
    int surface_count;
};

typedef struct {
    srapi_drm_display_t *drm;
    srapi_context_t *srctx;
    srapi_cmd_buffer_t *cmd;
    srapi_input_context_t *input;
    int listen_fd;
    char socket_path[128];
    uint32_t display_w;
    uint32_t display_h;
    int32_t mouse_x;
    int32_t mouse_y;
    int should_quit;
    uint32_t next_surface_id;
    int32_t next_cascade_x;
    int32_t next_cascade_y;
    swm_client_t clients[SWM_MAX_CLIENTS];
    swm_surface_t surfaces[SWM_MAX_SURFACES];
} swm_state_t;

#endif
