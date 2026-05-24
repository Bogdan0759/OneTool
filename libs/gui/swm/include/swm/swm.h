#ifndef ONETOOL_LIBS_GUI_SWM_INTERNAL_H
#define ONETOOL_LIBS_GUI_SWM_INTERNAL_H

#include <srapi/srapi.h>
#include <sprot/sprot.h>

#include <stdint.h>
#include <sys/types.h>

#define SWM_MAX_CLIENTS   16
#define SWM_MAX_SURFACES  32
#define SWM_TITLEBAR_H    22
#define SWM_BORDER        2
#define SWM_BTN_SIZE      14

typedef struct swm_client swm_client_t;
typedef struct de de_t;
typedef struct swm_output swm_output_t;
typedef struct swm_buffer swm_buffer_t;

typedef enum {
    SWM_INTERACT_NONE = 0,
    SWM_INTERACT_MOVE = 1,
} swm_interact_t;

typedef struct swm_surface {
    int in_use;
    uint32_t id;
    uint32_t client_handle;
    swm_client_t *owner;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t buffer_size;
    uint32_t buffer_kind;
    swm_buffer_t *buffer;
    int has_pending;
    int committed;
    int minimized;
    int maximized;
    uint32_t role;
    uint32_t parent_id;
    int32_t rel_x;
    int32_t rel_y;
    int wants_frame;
    int32_t pos_x;
    int32_t pos_y;
    int32_t saved_pos_x;
    int32_t saved_pos_y;
    int z;
    char title[128];
} swm_surface_t;

struct swm_client {
    int in_use;
    int sock;
    int has_hello;
    swm_surface_t *surfaces[8];
    int surface_count;
};

typedef struct {
    swm_output_t *output;
    srapi_input_context_t *input;
    int listen_fd;
    char socket_path[128];
    uint32_t display_w;
    uint32_t display_h;
    int32_t mouse_x;
    int32_t mouse_y;
    uint32_t current_cursor;
    uint32_t modifiers;
    int mouse_left_down;
    swm_interact_t interaction;
    swm_surface_t *grab_surface;
    swm_surface_t *hovered_surface;
    int32_t grab_offset_x;
    int32_t grab_offset_y;
    int should_quit;
    uint32_t next_surface_id;
    int32_t next_cascade_x;
    int32_t next_cascade_y;
    int next_z;
    uint64_t frame_count;
    uint64_t start_ms;
    swm_client_t clients[SWM_MAX_CLIENTS];
    swm_surface_t surfaces[SWM_MAX_SURFACES];
    de_t *de;

    int32_t dirty_x1, dirty_y1, dirty_x2, dirty_y2;
    int has_dirty_rect;
    int32_t prev_cursor_x, prev_cursor_y;
} swm_state_t;


#endif
