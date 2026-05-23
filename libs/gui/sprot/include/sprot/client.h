#ifndef ONETOOL_LIBS_GUI_SPROT_CLIENT_H
#define ONETOOL_LIBS_GUI_SPROT_CLIENT_H

#include <sprot/sprot.h>

#include <stdint.h>

typedef struct sprot_connection sprot_connection_t;
typedef struct sprot_surface sprot_surface_t;

typedef enum {
    SPROT_EVENT_NONE = 0,
    SPROT_EVENT_WELCOME,
    SPROT_EVENT_SURFACE_CREATED,
    SPROT_EVENT_SURFACE_CONFIGURE,
    SPROT_EVENT_SURFACE_CLOSE,
    SPROT_EVENT_SURFACE_FRAME,
    SPROT_EVENT_POINTER_MOTION,
    SPROT_EVENT_POINTER_BUTTON,
    SPROT_EVENT_POINTER_ENTER,
    SPROT_EVENT_POINTER_LEAVE,
    SPROT_EVENT_POINTER_AXIS,
    SPROT_EVENT_KEY,
    SPROT_EVENT_PONG,
    SPROT_EVENT_ERROR,
    SPROT_EVENT_DISCONNECT,
} sprot_event_kind_t;

typedef struct {
    sprot_event_kind_t kind;
    uint32_t serial;
    uint32_t object_id;
    union {
        sprot_body_welcome_t welcome;
        sprot_body_surface_created_t surface_created;
        sprot_body_configure_t configure;
        sprot_body_frame_t frame;
        sprot_body_pointer_motion_t pointer_motion;
        sprot_body_pointer_button_t pointer_button;
        sprot_body_pointer_axis_t pointer_axis;
        sprot_body_key_t key;
        struct {
            uint32_t code;
            char message[128];
        } error;
    } u;
} sprot_event_t;

sprot_connection_t *sprot_connect(const char *socket_path);
void sprot_disconnect(sprot_connection_t *conn);
int sprot_connection_fd(const sprot_connection_t *conn);
uint32_t sprot_display_width(const sprot_connection_t *conn);
uint32_t sprot_display_height(const sprot_connection_t *conn);
const char *sprot_last_error(void);

sprot_surface_t *sprot_create_surface(sprot_connection_t *conn, uint32_t width, uint32_t height);
void sprot_destroy_surface(sprot_surface_t *surface);
uint32_t *sprot_surface_pixels(sprot_surface_t *surface);
uint32_t sprot_surface_stride(const sprot_surface_t *surface);
uint32_t sprot_surface_id(const sprot_surface_t *surface);
int sprot_commit(sprot_surface_t *surface);
int sprot_damage(sprot_surface_t *surface, int32_t x, int32_t y, uint32_t w, uint32_t h);
int sprot_request_frame(sprot_surface_t *surface);
int sprot_set_title(sprot_surface_t *surface, const char *title);
int sprot_set_role(sprot_surface_t *surface, uint32_t role, uint32_t parent_id, int32_t x, int32_t y);
int sprot_set_cursor(sprot_surface_t *surface, uint32_t cursor_type);

int sprot_poll_event(sprot_connection_t *conn, sprot_event_t *out_event, int timeout_ms);
int sprot_ping(sprot_connection_t *conn, uint32_t serial);

#endif
