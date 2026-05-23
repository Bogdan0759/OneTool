#ifndef ONETOOL_LIBS_GUI_SPROT_H
#define ONETOOL_LIBS_GUI_SPROT_H

#include <stddef.h>
#include <stdint.h>

#define SPROT_VERSION_MAJOR  0
#define SPROT_VERSION_MINOR  2
#define SPROT_DEFAULT_SOCKET "/tmp/swm.sock"
#define SPROT_MAX_MESSAGE    4096
#define SPROT_PIXEL_FORMAT_BGRA8888  0u
#define SPROT_MAX_TITLE      127

typedef struct {
    uint16_t type;
    uint16_t flags;
    uint32_t length;
    uint32_t object_id;
    uint32_t serial;
} sprot_header_t;

#define SPROT_FLAG_HAS_FD  (1u << 0)

typedef enum {
    SPROT_REQ_HELLO            = 0x0001,
    SPROT_REQ_SURFACE_CREATE   = 0x0010,
    SPROT_REQ_SURFACE_ATTACH   = 0x0011,
    SPROT_REQ_SURFACE_DAMAGE   = 0x0012,
    SPROT_REQ_SURFACE_COMMIT   = 0x0013,
    SPROT_REQ_SURFACE_DESTROY  = 0x0014,
    SPROT_REQ_SURFACE_FRAME    = 0x0015,
    SPROT_REQ_SURFACE_SET_TITLE = 0x0016,
    SPROT_REQ_SURFACE_SET_ROLE = 0x0017,
    SPROT_REQ_SET_CURSOR       = 0x0018,
    SPROT_REQ_PING             = 0x0080,

    SPROT_EVT_WELCOME          = 0x4001,
    SPROT_EVT_SURFACE_CREATED  = 0x4010,
    SPROT_EVT_SURFACE_CONFIGURE = 0x4011,
    SPROT_EVT_SURFACE_CLOSE    = 0x4012,
    SPROT_EVT_SURFACE_FRAME    = 0x4013,
    SPROT_EVT_POINTER_MOTION   = 0x4020,
    SPROT_EVT_POINTER_BUTTON   = 0x4021,
    SPROT_EVT_KEY              = 0x4022,
    SPROT_EVT_POINTER_ENTER    = 0x4023,
    SPROT_EVT_POINTER_LEAVE    = 0x4024,
    SPROT_EVT_POINTER_AXIS     = 0x4025,
    SPROT_EVT_PONG             = 0x4080,
    SPROT_EVT_ERROR            = 0x40FF,
} sprot_msg_type_t;

typedef struct {
    uint32_t version_major;
    uint32_t version_minor;
} sprot_body_hello_t;

typedef struct {
    uint32_t display_width;
    uint32_t display_height;
    uint32_t version_major;
    uint32_t version_minor;
} sprot_body_welcome_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t client_handle;
} sprot_body_surface_create_t;

typedef struct {
    uint32_t surface_id;
    uint32_t client_handle;
} sprot_body_surface_created_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t buffer_size;
} sprot_body_surface_attach_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} sprot_body_surface_damage_t;

typedef struct {
    int32_t x;
    int32_t y;
} sprot_body_pointer_motion_t;

typedef struct {
    uint32_t button;
    uint32_t state;
} sprot_body_pointer_button_t;

typedef struct {
    int32_t dx;
    int32_t dy;
} sprot_body_pointer_axis_t;

typedef struct {
    uint32_t scancode;
    uint32_t state;
    uint32_t modifiers;
} sprot_body_key_t;

typedef struct {
    uint32_t code;
    uint32_t length;
} sprot_body_error_t;

typedef struct {
    uint32_t length;
} sprot_body_set_title_t;

typedef struct {
    uint32_t role;
    uint32_t parent_id;
    int32_t x;
    int32_t y;
} sprot_body_surface_set_role_t;

typedef struct {
    uint32_t cursor_type;
} sprot_body_set_cursor_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t state;
    uint32_t serial;
} sprot_body_configure_t;

typedef struct {
    uint32_t time_ms;
    uint32_t serial;
} sprot_body_frame_t;

#define SPROT_BUTTON_STATE_RELEASED  0u
#define SPROT_BUTTON_STATE_PRESSED   1u

#define SPROT_KEY_STATE_RELEASED  0u
#define SPROT_KEY_STATE_PRESSED   1u

#define SPROT_SURFACE_ROLE_TOPLEVEL 0u
#define SPROT_SURFACE_ROLE_POPUP    1u

#define SPROT_CURSOR_ARROW  0u
#define SPROT_CURSOR_IBEAM  1u
#define SPROT_CURSOR_HAND   2u

#define SPROT_SURFACE_STATE_NORMAL     0u
#define SPROT_SURFACE_STATE_MAXIMIZED  (1u << 0)
#define SPROT_SURFACE_STATE_MINIMIZED  (1u << 1)
#define SPROT_SURFACE_STATE_FOCUSED    (1u << 2)

#define SPROT_ERROR_PROTOCOL       0x0001
#define SPROT_ERROR_INVALID_ARG    0x0002
#define SPROT_ERROR_OUT_OF_MEMORY  0x0003
#define SPROT_ERROR_LIMIT          0x0004

int sprot_send_message(int sock_fd, const sprot_header_t *header, const void *body, size_t body_len, int attach_fd);
int sprot_recv_message(int sock_fd, sprot_header_t *header_out, void *body_out, size_t body_cap, int *received_fd);

const char *sprot_msg_type_name(uint16_t type);

#endif
