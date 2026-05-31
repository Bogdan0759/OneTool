#define _GNU_SOURCE
#include "wayland_server.h"
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "viewporter-protocol.h"
#include "linux-dmabuf-protocol.h"
#include <sprot/client.h>
#include <sprot/vk_interop.h>
#include <srapi/srapi.h>

#include <wayland-server.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <linux/input-event-codes.h>

#define MAX_SURFACES 64

struct bridge_client;

struct bridge_surface;

struct bridge_surface {
    struct wl_resource *resource;
    struct bridge_client *client;
    sprot_surface_t *sprot_surface;
    uint32_t sprot_id;
    uint32_t client_handle;
    struct wl_resource *xdg_surface_resource;
    struct wl_resource *xdg_toplevel_resource;
    struct wl_resource *xdg_popup_resource;
    struct wl_resource *buffer_resource;
    int32_t width, height;
    int memfd;
    void *memfd_map;
    size_t memfd_size;
    struct wl_list frame_callbacks;
    struct wl_list link;
    int configured;
    int pending_commit;
    int pending_width;
    int pending_height;
    int pending_stride;
    size_t pending_size;
    struct dmabuf_buffer *pending_dmabuf;
    int is_subsurface;
    struct wl_resource *subsurface_resource;
    struct bridge_surface *subsurface_parent;
    int is_cursor;
    int is_popup;
    int popup_grabbed;
    struct bridge_surface *popup_parent;
    int32_t popup_x;
    int32_t popup_y;
    int32_t popup_width;
    int32_t popup_height;
    int window_geometry_set;
    int32_t window_geometry_x;
    int32_t window_geometry_y;
    int32_t window_geometry_width;
    int32_t window_geometry_height;
    int32_t subsurface_x;
    int32_t subsurface_y;
    int32_t cursor_hotspot_x;
    int32_t cursor_hotspot_y;
};

struct seat_resource {
    struct wl_resource *resource;
    struct bridge_client *client;
    struct wl_list link;
};

struct bridge_client {
    struct wl_client *wl_client;
    sprot_connection_t *sprot_conn;
    struct wl_event_source *sprot_source;
    struct wl_listener destroy_listener;
    struct wl_list surfaces;
    struct wl_list seat_pointers;
    struct wl_list seat_keyboards;
    struct wl_list seat_resources;
    struct bridge_surface *keyboard_focus;
    uint32_t keyboard_serial;
    uint32_t keyboard_mods_depressed;
    uint32_t keyboard_mods_latched;
    uint32_t keyboard_mods_locked;
    uint32_t keyboard_group;
    uint32_t pressed_keys[32];
    size_t pressed_key_count;
    struct wl_list link;
};

struct bridge_server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    const char *swm_socket;
    struct wl_list clients;
    uint32_t next_client_handle;
};

struct dmabuf_buffer {
    struct wl_resource *resource;
    int is_dmabuf;

    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t modifier;
    uint32_t num_planes;
    uint32_t offsets[4];
    uint32_t strides[4];
    uint32_t flags;
};

struct dmabuf_params {
    struct wl_resource *resource;
    struct bridge_client *client;

    int fds[4];
    uint32_t offsets[4];
    uint32_t strides[4];
    uint64_t modifier;
    uint32_t num_planes;
    uint8_t plane_set[4];
    int used;
};

struct dmabuf_feedback {
    struct wl_resource *resource;
    struct wl_resource *surface_resource;
};

struct xdg_positioner_data {
    int has_size;
    int32_t width;
    int32_t height;
    int has_anchor_rect;
    int32_t anchor_rect_x;
    int32_t anchor_rect_y;
    int32_t anchor_rect_width;
    int32_t anchor_rect_height;
    uint32_t anchor;
    uint32_t gravity;
    uint32_t constraint_adjustment;
    int32_t offset_x;
    int32_t offset_y;
    int reactive;
    int has_parent_size;
    int32_t parent_width;
    int32_t parent_height;
    int has_parent_configure;
    uint32_t parent_configure_serial;
};

static struct bridge_server g_server;
static struct {
    char render_node_path[256];
    uint32_t render_major;
    uint32_t render_minor;
    int has_drm;
    int queried;
} g_render_node_info = {0};
static FILE *g_debug_file = NULL;
static int g_keymap_fd = -1;
static uint32_t g_keymap_size = 0;
static const char g_default_keymap[] =
    "xkb_keymap {\n"
    "xkb_keycodes \"(unnamed)\" {\n"
    "minimum = 8;\n"
    "maximum = 255;\n"
    "include \"evdev+aliases(qwerty)\"\n"
    "};\n"
    "xkb_types \"(unnamed)\" {\n"
    "include \"complete\"\n"
    "};\n"
    "xkb_compatibility \"(unnamed)\" {\n"
    "include \"complete\"\n"
    "};\n"
    "xkb_symbols \"(unnamed)\" {\n"
    "include \"pc+us+inet(evdev)\"\n"
    "};\n"
    "xkb_geometry \"(unnamed)\" {\n"
    "include \"pc(pc105)\"\n"
    "};\n"
    "};\n";

#include <stdarg.h>
#include <time.h>

static void debug_log(const char *fmt, ...) {
    if (!g_debug_file) return;
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    char time_buf[26];
    strftime(time_buf, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(g_debug_file, "[%s] ", time_buf);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_debug_file, fmt, args);
    va_end(args);
    fprintf(g_debug_file, "\n");
    fflush(g_debug_file);
}

static uint32_t wlbridge_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint32_t wayland_button_from_srapi(uint32_t button) {
    switch (button) {
        case SRAPI_MOUSE_BUTTON_LEFT:   return BTN_LEFT;
        case SRAPI_MOUSE_BUTTON_RIGHT:  return BTN_RIGHT;
        case SRAPI_MOUSE_BUTTON_MIDDLE: return BTN_MIDDLE;
        case SRAPI_MOUSE_BUTTON_X1:     return BTN_SIDE;
        case SRAPI_MOUSE_BUTTON_X2:     return BTN_EXTRA;
        default: return button;
    }
}

static void on_client_created(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wl_client *client = (struct wl_client *)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("[CLIENT_CONNECT] new wl_client pid=%d uid=%d ptr=%p", (int)pid, (int)uid, (void*)client);
}

static void on_protocol_log(void *ud, enum wl_protocol_logger_type dir,
                             const struct wl_protocol_logger_message *msg) {
    (void)ud;
    if (!g_debug_file) return;
    const char *dir_str = (dir == WL_PROTOCOL_LOGGER_REQUEST) ? "REQ" : "EVT";
    const char *iface  = msg->resource ? wl_resource_get_class(msg->resource) : "?";
    uint32_t res_id    = msg->resource ? wl_resource_get_id(msg->resource)    : 0;
    const char *method = msg->message  ? msg->message->name                   : "?";
    struct wl_client *client = msg->resource ? wl_resource_get_client(msg->resource) : NULL;
    pid_t pid = 0;
    if (client) { uid_t u; gid_t g; wl_client_get_credentials(client, &pid, &u, &g); }

    if (strcmp(iface, "wl_registry") == 0 && strcmp(method, "global") == 0 && msg->arguments_count >= 3) {
        debug_log("[PROTO %s] pid=%d %s@%u.%s name=%u interface=%s version=%u",
                  dir_str, (int)pid, iface, res_id, method,
                  msg->arguments[0].u, msg->arguments[1].s, msg->arguments[2].u);
        return;
    }
    if (strcmp(iface, "wl_registry") == 0 && strcmp(method, "bind") == 0 && msg->arguments_count >= 4) {
        debug_log("[PROTO %s] pid=%d %s@%u.%s name=%u interface=%s version=%u new_id=%u",
                  dir_str, (int)pid, iface, res_id, method,
                  msg->arguments[0].u, msg->arguments[1].s, msg->arguments[2].u, msg->arguments[3].n);
        return;
    }
    if (msg->arguments_count > 0) {
        debug_log("[PROTO %s] pid=%d %s@%u.%s argc=%d", dir_str, (int)pid, iface, res_id, method, msg->arguments_count);
        return;
    }
    debug_log("[PROTO %s] pid=%d %s@%u.%s", dir_str, (int)pid, iface, res_id, method);
}

static uint32_t next_keyboard_serial(struct bridge_client *c) {
    c->keyboard_serial++;
    if (c->keyboard_serial == 0) {
        c->keyboard_serial = 1;
    }
    return c->keyboard_serial;
}

static int32_t bridge_surface_window_x(const struct bridge_surface *s) {
    return s && s->window_geometry_set ? s->window_geometry_x : 0;
}

static int32_t bridge_surface_window_y(const struct bridge_surface *s) {
    return s && s->window_geometry_set ? s->window_geometry_y : 0;
}

static int32_t bridge_surface_window_width(const struct bridge_surface *s) {
    if (!s) return 0;
    if (s->window_geometry_set && s->window_geometry_width > 0) return s->window_geometry_width;
    return s->width;
}

static int32_t bridge_surface_window_height(const struct bridge_surface *s) {
    if (!s) return 0;
    if (s->window_geometry_set && s->window_geometry_height > 0) return s->window_geometry_height;
    return s->height;
}

static int positioner_anchor_left(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
}

static int positioner_anchor_right(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_RIGHT ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_RIGHT ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}

static int positioner_anchor_top(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_TOP ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_RIGHT;
}

static int positioner_anchor_bottom(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_BOTTOM ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}

static int positioner_gravity_left(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
}

static int positioner_gravity_right(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_RIGHT ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_RIGHT ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
}

static int positioner_gravity_top(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_TOP ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_RIGHT;
}

static int positioner_gravity_bottom(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_BOTTOM ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
}

static uint32_t positioner_flip_anchor_x(uint32_t anchor) {
    switch (anchor) {
        case XDG_POSITIONER_ANCHOR_LEFT: return XDG_POSITIONER_ANCHOR_RIGHT;
        case XDG_POSITIONER_ANCHOR_RIGHT: return XDG_POSITIONER_ANCHOR_LEFT;
        case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
        default: return anchor;
    }
}

static uint32_t positioner_flip_anchor_y(uint32_t anchor) {
    switch (anchor) {
        case XDG_POSITIONER_ANCHOR_TOP: return XDG_POSITIONER_ANCHOR_BOTTOM;
        case XDG_POSITIONER_ANCHOR_BOTTOM: return XDG_POSITIONER_ANCHOR_TOP;
        case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
        default: return anchor;
    }
}

static uint32_t positioner_flip_gravity_x(uint32_t gravity) {
    switch (gravity) {
        case XDG_POSITIONER_GRAVITY_LEFT: return XDG_POSITIONER_GRAVITY_RIGHT;
        case XDG_POSITIONER_GRAVITY_RIGHT: return XDG_POSITIONER_GRAVITY_LEFT;
        case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
        case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
        default: return gravity;
    }
}

static uint32_t positioner_flip_gravity_y(uint32_t gravity) {
    switch (gravity) {
        case XDG_POSITIONER_GRAVITY_TOP: return XDG_POSITIONER_GRAVITY_BOTTOM;
        case XDG_POSITIONER_GRAVITY_BOTTOM: return XDG_POSITIONER_GRAVITY_TOP;
        case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
        case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
        default: return gravity;
    }
}

static void positioner_calc_xy(const struct xdg_positioner_data *pos,
                               uint32_t anchor, uint32_t gravity,
                               int32_t width, int32_t height,
                               int32_t *out_x, int32_t *out_y) {
    int32_t ax = pos->anchor_rect_x;
    int32_t ay = pos->anchor_rect_y;

    if (positioner_anchor_right(anchor)) ax += pos->anchor_rect_width;
    else if (!positioner_anchor_left(anchor)) ax += pos->anchor_rect_width / 2;

    if (positioner_anchor_bottom(anchor)) ay += pos->anchor_rect_height;
    else if (!positioner_anchor_top(anchor)) ay += pos->anchor_rect_height / 2;

    if (positioner_gravity_left(gravity)) *out_x = ax - width;
    else if (positioner_gravity_right(gravity)) *out_x = ax;
    else *out_x = ax - width / 2;

    if (positioner_gravity_top(gravity)) *out_y = ay - height;
    else if (positioner_gravity_bottom(gravity)) *out_y = ay;
    else *out_y = ay - height / 2;

    *out_x += pos->offset_x;
    *out_y += pos->offset_y;
}

static int positioner_axis_fits(int32_t value, int32_t size, int32_t bounds) {
    return bounds <= 0 || (value >= 0 && value + size <= bounds);
}

static void positioner_apply_constraints(const struct xdg_positioner_data *pos,
                                         const struct bridge_surface *parent,
                                         int32_t *x, int32_t *y,
                                         int32_t *width, int32_t *height) {
    int32_t bounds_w = pos->has_parent_size ? pos->parent_width : bridge_surface_window_width(parent);
    int32_t bounds_h = pos->has_parent_size ? pos->parent_height : bridge_surface_window_height(parent);

    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;

    if (bounds_w > 0 && !positioner_axis_fits(*x, *width, bounds_w)) {
        if (pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) {
            int32_t fx, fy;
            positioner_calc_xy(pos, positioner_flip_anchor_x(pos->anchor),
                               positioner_flip_gravity_x(pos->gravity),
                               *width, *height, &fx, &fy);
            if (positioner_axis_fits(fx, *width, bounds_w)) *x = fx;
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) &&
            !positioner_axis_fits(*x, *width, bounds_w)) {
            if (*width <= bounds_w) {
                if (*x < 0) *x = 0;
                if (*x + *width > bounds_w) *x = bounds_w - *width;
            }
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X) &&
            !positioner_axis_fits(*x, *width, bounds_w)) {
            if (*x < 0) *x = 0;
            if (*x >= bounds_w) *x = bounds_w - 1;
            if (*x + *width > bounds_w) *width = bounds_w - *x;
            if (*width < 1) *width = 1;
        }
    }

    if (bounds_h > 0 && !positioner_axis_fits(*y, *height, bounds_h)) {
        if (pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) {
            int32_t fx, fy;
            positioner_calc_xy(pos, positioner_flip_anchor_y(pos->anchor),
                               positioner_flip_gravity_y(pos->gravity),
                               *width, *height, &fx, &fy);
            if (positioner_axis_fits(fy, *height, bounds_h)) *y = fy;
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) &&
            !positioner_axis_fits(*y, *height, bounds_h)) {
            if (*height <= bounds_h) {
                if (*y < 0) *y = 0;
                if (*y + *height > bounds_h) *y = bounds_h - *height;
            }
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y) &&
            !positioner_axis_fits(*y, *height, bounds_h)) {
            if (*y < 0) *y = 0;
            if (*y >= bounds_h) *y = bounds_h - 1;
            if (*y + *height > bounds_h) *height = bounds_h - *y;
            if (*height < 1) *height = 1;
        }
    }
}

static void compute_popup_geometry(struct bridge_surface *popup,
                                   struct bridge_surface *parent,
                                   const struct xdg_positioner_data *pos) {
    int32_t x, y;
    int32_t width = pos->width;
    int32_t height = pos->height;

    positioner_calc_xy(pos, pos->anchor, pos->gravity, width, height, &x, &y);
    positioner_apply_constraints(pos, parent, &x, &y, &width, &height);

    popup->popup_x = x;
    popup->popup_y = y;
    popup->popup_width = width;
    popup->popup_height = height;
}

static int32_t popup_sprot_x(const struct bridge_surface *s) {
    return s ? s->popup_x + bridge_surface_window_x(s->popup_parent) : 0;
}

static int32_t popup_sprot_y(const struct bridge_surface *s) {
    return s ? s->popup_y + bridge_surface_window_y(s->popup_parent) : 0;
}

static int send_sprot_role_for_surface(struct bridge_surface *s) {
    if (!s || !s->sprot_surface || s->sprot_id == 0) return -1;

    if (s->is_popup) {
        if (!s->popup_parent || s->popup_parent->sprot_id == 0) return -2;
        uint32_t parent_id = s->popup_parent ? s->popup_parent->sprot_id : 0;
        return sprot_set_role(s->sprot_surface, SPROT_SURFACE_ROLE_POPUP,
                              parent_id, popup_sprot_x(s), popup_sprot_y(s));
    }
    if (s->is_subsurface) {
        if (!s->subsurface_parent || s->subsurface_parent->sprot_id == 0) return -2;
        uint32_t parent_id = s->subsurface_parent ? s->subsurface_parent->sprot_id : 0;
        return sprot_set_role(s->sprot_surface, SPROT_SURFACE_ROLE_SUBSURFACE,
                              parent_id, s->subsurface_x, s->subsurface_y);
    }
    return sprot_set_role(s->sprot_surface, SPROT_SURFACE_ROLE_TOPLEVEL, 0, 0, 0);
}

static void configure_popup_surface(struct bridge_surface *s, uint32_t token, int repositioned) {
    if (!s || !s->xdg_popup_resource || !s->xdg_surface_resource) return;
    uint32_t version = wl_resource_get_version(s->xdg_popup_resource);
    if (repositioned && version >= XDG_POPUP_REPOSITIONED_SINCE_VERSION) {
        xdg_popup_send_repositioned(s->xdg_popup_resource, token);
    }
    xdg_popup_send_configure(s->xdg_popup_resource, s->popup_x, s->popup_y,
                             s->popup_width, s->popup_height);
    xdg_surface_send_configure(s->xdg_surface_resource, next_keyboard_serial(s->client));
    s->configured = 1;
}

static uint32_t evdev_to_xkb_mod(uint32_t scancode) {
    switch (scancode) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
            return 1u << 0;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
            return 1u << 2;
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            return 1u << 3;
        case KEY_CAPSLOCK:
            return 1u << 1;
        default:
            return 0;
    }
}

static uint32_t srapi_scancode_to_evdev(uint32_t scancode) {
    switch ((srapi_scancode_t)scancode) {
        case SRAPI_SCANCODE_A: return KEY_A;
        case SRAPI_SCANCODE_B: return KEY_B;
        case SRAPI_SCANCODE_C: return KEY_C;
        case SRAPI_SCANCODE_D: return KEY_D;
        case SRAPI_SCANCODE_E: return KEY_E;
        case SRAPI_SCANCODE_F: return KEY_F;
        case SRAPI_SCANCODE_G: return KEY_G;
        case SRAPI_SCANCODE_H: return KEY_H;
        case SRAPI_SCANCODE_I: return KEY_I;
        case SRAPI_SCANCODE_J: return KEY_J;
        case SRAPI_SCANCODE_K: return KEY_K;
        case SRAPI_SCANCODE_L: return KEY_L;
        case SRAPI_SCANCODE_M: return KEY_M;
        case SRAPI_SCANCODE_N: return KEY_N;
        case SRAPI_SCANCODE_O: return KEY_O;
        case SRAPI_SCANCODE_P: return KEY_P;
        case SRAPI_SCANCODE_Q: return KEY_Q;
        case SRAPI_SCANCODE_R: return KEY_R;
        case SRAPI_SCANCODE_S: return KEY_S;
        case SRAPI_SCANCODE_T: return KEY_T;
        case SRAPI_SCANCODE_U: return KEY_U;
        case SRAPI_SCANCODE_V: return KEY_V;
        case SRAPI_SCANCODE_W: return KEY_W;
        case SRAPI_SCANCODE_X: return KEY_X;
        case SRAPI_SCANCODE_Y: return KEY_Y;
        case SRAPI_SCANCODE_Z: return KEY_Z;
        case SRAPI_SCANCODE_1: return KEY_1;
        case SRAPI_SCANCODE_2: return KEY_2;
        case SRAPI_SCANCODE_3: return KEY_3;
        case SRAPI_SCANCODE_4: return KEY_4;
        case SRAPI_SCANCODE_5: return KEY_5;
        case SRAPI_SCANCODE_6: return KEY_6;
        case SRAPI_SCANCODE_7: return KEY_7;
        case SRAPI_SCANCODE_8: return KEY_8;
        case SRAPI_SCANCODE_9: return KEY_9;
        case SRAPI_SCANCODE_0: return KEY_0;
        case SRAPI_SCANCODE_RETURN: return KEY_ENTER;
        case SRAPI_SCANCODE_ESCAPE: return KEY_ESC;
        case SRAPI_SCANCODE_BACKSPACE: return KEY_BACKSPACE;
        case SRAPI_SCANCODE_TAB: return KEY_TAB;
        case SRAPI_SCANCODE_SPACE: return KEY_SPACE;
        case SRAPI_SCANCODE_MINUS: return KEY_MINUS;
        case SRAPI_SCANCODE_EQUALS: return KEY_EQUAL;
        case SRAPI_SCANCODE_LEFTBRACKET: return KEY_LEFTBRACE;
        case SRAPI_SCANCODE_RIGHTBRACKET: return KEY_RIGHTBRACE;
        case SRAPI_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
        case SRAPI_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
        case SRAPI_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
        case SRAPI_SCANCODE_GRAVE: return KEY_GRAVE;
        case SRAPI_SCANCODE_COMMA: return KEY_COMMA;
        case SRAPI_SCANCODE_PERIOD: return KEY_DOT;
        case SRAPI_SCANCODE_SLASH: return KEY_SLASH;
        case SRAPI_SCANCODE_CAPSLOCK: return KEY_CAPSLOCK;
        case SRAPI_SCANCODE_F1: return KEY_F1;
        case SRAPI_SCANCODE_F2: return KEY_F2;
        case SRAPI_SCANCODE_F3: return KEY_F3;
        case SRAPI_SCANCODE_F4: return KEY_F4;
        case SRAPI_SCANCODE_F5: return KEY_F5;
        case SRAPI_SCANCODE_F6: return KEY_F6;
        case SRAPI_SCANCODE_F7: return KEY_F7;
        case SRAPI_SCANCODE_F8: return KEY_F8;
        case SRAPI_SCANCODE_F9: return KEY_F9;
        case SRAPI_SCANCODE_F10: return KEY_F10;
        case SRAPI_SCANCODE_F11: return KEY_F11;
        case SRAPI_SCANCODE_F12: return KEY_F12;
        case SRAPI_SCANCODE_PRINTSCREEN: return KEY_SYSRQ;
        case SRAPI_SCANCODE_SCROLLLOCK: return KEY_SCROLLLOCK;
        case SRAPI_SCANCODE_PAUSE: return KEY_PAUSE;
        case SRAPI_SCANCODE_INSERT: return KEY_INSERT;
        case SRAPI_SCANCODE_HOME: return KEY_HOME;
        case SRAPI_SCANCODE_PAGEUP: return KEY_PAGEUP;
        case SRAPI_SCANCODE_DELETE: return KEY_DELETE;
        case SRAPI_SCANCODE_END: return KEY_END;
        case SRAPI_SCANCODE_PAGEDOWN: return KEY_PAGEDOWN;
        case SRAPI_SCANCODE_RIGHT: return KEY_RIGHT;
        case SRAPI_SCANCODE_LEFT: return KEY_LEFT;
        case SRAPI_SCANCODE_DOWN: return KEY_DOWN;
        case SRAPI_SCANCODE_UP: return KEY_UP;
        case SRAPI_SCANCODE_NUMLOCK: return KEY_NUMLOCK;
        case SRAPI_SCANCODE_KP_DIVIDE: return KEY_KPSLASH;
        case SRAPI_SCANCODE_KP_MULTIPLY: return KEY_KPASTERISK;
        case SRAPI_SCANCODE_KP_MINUS: return KEY_KPMINUS;
        case SRAPI_SCANCODE_KP_PLUS: return KEY_KPPLUS;
        case SRAPI_SCANCODE_KP_ENTER: return KEY_KPENTER;
        case SRAPI_SCANCODE_KP_1: return KEY_KP1;
        case SRAPI_SCANCODE_KP_2: return KEY_KP2;
        case SRAPI_SCANCODE_KP_3: return KEY_KP3;
        case SRAPI_SCANCODE_KP_4: return KEY_KP4;
        case SRAPI_SCANCODE_KP_5: return KEY_KP5;
        case SRAPI_SCANCODE_KP_6: return KEY_KP6;
        case SRAPI_SCANCODE_KP_7: return KEY_KP7;
        case SRAPI_SCANCODE_KP_8: return KEY_KP8;
        case SRAPI_SCANCODE_KP_9: return KEY_KP9;
        case SRAPI_SCANCODE_KP_0: return KEY_KP0;
        case SRAPI_SCANCODE_KP_PERIOD: return KEY_KPDOT;
        case SRAPI_SCANCODE_LCTRL: return KEY_LEFTCTRL;
        case SRAPI_SCANCODE_LSHIFT: return KEY_LEFTSHIFT;
        case SRAPI_SCANCODE_LALT: return KEY_LEFTALT;
        case SRAPI_SCANCODE_LGUI: return KEY_LEFTMETA;
        case SRAPI_SCANCODE_RCTRL: return KEY_RIGHTCTRL;
        case SRAPI_SCANCODE_RSHIFT: return KEY_RIGHTSHIFT;
        case SRAPI_SCANCODE_RALT: return KEY_RIGHTALT;
        case SRAPI_SCANCODE_RGUI: return KEY_RIGHTMETA;
        default: return 0;
    }
}

static void send_keyboard_modifiers(struct bridge_client *c, uint32_t serial) {
    struct seat_resource *sr;
    wl_list_for_each(sr, &c->seat_keyboards, link) {
        wl_keyboard_send_modifiers(sr->resource, serial,
            c->keyboard_mods_depressed,
            c->keyboard_mods_latched,
            c->keyboard_mods_locked,
            c->keyboard_group);
    }
}

static void clear_pressed_keys(struct bridge_client *c) {
    c->pressed_key_count = 0;
    c->keyboard_mods_depressed = 0;
}

static void set_keyboard_focus(struct bridge_client *c, struct bridge_surface *surface, uint32_t serial) {
    if (c->keyboard_focus == surface) {
        return;
    }

    struct wl_array keys;
    wl_array_init(&keys);

    if (c->keyboard_focus != NULL) {
        struct seat_resource *sr;
        wl_list_for_each(sr, &c->seat_keyboards, link) {
            wl_keyboard_send_leave(sr->resource, serial, c->keyboard_focus->resource);
        }
    }

    clear_pressed_keys(c);
    c->keyboard_focus = surface;

    if (surface != NULL) {
        struct seat_resource *sr;
        wl_list_for_each(sr, &c->seat_keyboards, link) {
            wl_keyboard_send_enter(sr->resource, serial, surface->resource, &keys);
        }
        send_keyboard_modifiers(c, serial);
    }

    wl_array_release(&keys);
}

static void update_pressed_keys(struct bridge_client *c, uint32_t scancode, uint32_t pressed) {
    size_t i;
    for (i = 0; i < c->pressed_key_count; i++) {
        if (c->pressed_keys[i] == scancode) {
            break;
        }
    }

    if (pressed) {
        if (i == c->pressed_key_count && c->pressed_key_count < sizeof(c->pressed_keys) / sizeof(c->pressed_keys[0])) {
            c->pressed_keys[c->pressed_key_count++] = scancode;
        }
    } else if (i < c->pressed_key_count) {
        memmove(&c->pressed_keys[i], &c->pressed_keys[i + 1],
            (c->pressed_key_count - i - 1) * sizeof(c->pressed_keys[0]));
        c->pressed_key_count--;
    }

    uint32_t mod = evdev_to_xkb_mod(scancode);
    if (mod == 0) {
        return;
    }

    if (scancode == KEY_CAPSLOCK && pressed) {
        c->keyboard_mods_locked ^= mod;
        return;
    }

    if (pressed) {
        c->keyboard_mods_depressed |= mod;
    } else {
        c->keyboard_mods_depressed &= ~mod;
    }
}

static int ensure_keymap(void) {
    if (g_keymap_fd >= 0) {
        return 0;
    }

    g_keymap_size = (uint32_t)strlen(g_default_keymap);
    g_keymap_fd = memfd_create("wlbridge-keymap", MFD_CLOEXEC);
    if (g_keymap_fd < 0) {
        debug_log("Error: keymap memfd_create failed: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(g_keymap_fd, g_keymap_size) != 0) {
        debug_log("Error: keymap ftruncate failed: %s", strerror(errno));
        close(g_keymap_fd);
        g_keymap_fd = -1;
        return -1;
    }
    ssize_t written = pwrite(g_keymap_fd, g_default_keymap, g_keymap_size, 0);
    if (written < 0 || (size_t)written != g_keymap_size) {
        debug_log("Error: keymap write failed: %s", strerror(errno));
        close(g_keymap_fd);
        g_keymap_fd = -1;
        return -1;
    }
    return 0;
}

// Forward declarations
static const struct wl_surface_interface surface_implementation;
static const struct xdg_surface_interface xdg_surface_implementation;
static const struct xdg_toplevel_interface xdg_toplevel_implementation;
static const struct xdg_positioner_interface xdg_positioner_implementation;
static const struct xdg_popup_interface xdg_popup_implementation;

static struct bridge_surface *find_surface_by_sprot_id(struct bridge_client *c, uint32_t sprot_id) {
    struct bridge_surface *s;
    wl_list_for_each(s, &c->surfaces, link) {
        if (s->sprot_id == sprot_id ||
            (s->sprot_surface && sprot_surface_id(s->sprot_surface) == sprot_id)) {
            return s;
        }
    }
    return NULL;
}

static void destroy_bridge_surface(struct bridge_surface *s) {
    if (!s) return;

    if (s->client && s->client->keyboard_focus == s) {
        s->client->keyboard_focus = NULL;
    }
    if (s->client) {
        struct bridge_surface *other;
        wl_list_for_each(other, &s->client->surfaces, link) {
            if (other->popup_parent == s) other->popup_parent = NULL;
            if (other->subsurface_parent == s) other->subsurface_parent = NULL;
        }
    }
    if (s->resource) {
        wl_resource_set_user_data(s->resource, NULL);
    }
    if (s->xdg_surface_resource) {
        wl_resource_set_user_data(s->xdg_surface_resource, NULL);
    }
    if (s->xdg_toplevel_resource) {
        wl_resource_set_user_data(s->xdg_toplevel_resource, NULL);
    }
    if (s->xdg_popup_resource) {
        wl_resource_set_user_data(s->xdg_popup_resource, NULL);
    }
    if (s->subsurface_resource) {
        wl_resource_set_user_data(s->subsurface_resource, NULL);
    }

    if (s->sprot_surface) {
        sprot_destroy_surface(s->sprot_surface);
    }
    if (s->memfd_map && s->memfd_map != MAP_FAILED) {
        munmap(s->memfd_map, s->memfd_size);
    }
    if (s->memfd >= 0) {
        close(s->memfd);
    }

    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
        wl_resource_destroy(cb);
    }

    wl_list_remove(&s->link);
    free(s);
}

static void free_seat_resource(struct seat_resource *sr) {
    if (!sr) return;
    if (sr->resource) {
        wl_resource_set_user_data(sr->resource, NULL);
    }
    wl_list_remove(&sr->link);
    free(sr);
}

static int surface_parent_ready(struct bridge_surface *s) {
    if (!s) return 0;
    if (s->is_popup) {
        return s->popup_parent != NULL && s->popup_parent->sprot_id != 0;
    }
    if (s->is_subsurface) {
        return s->subsurface_parent != NULL && s->subsurface_parent->sprot_id != 0;
    }
    return 1;
}

static void flush_pending_surface_commit(struct bridge_surface *s) {
    if (!s || !s->sprot_surface || s->sprot_id == 0) return;

    if (!surface_parent_ready(s)) {
        debug_log("Deferring role/commit for child handle=%u until parent gets a Sprot ID", s->client_handle);
        return;
    }

    int role_res = send_sprot_role_for_surface(s);
    if (s->is_popup || s->is_subsurface) {
        uint32_t parent_id = s->is_popup
            ? (s->popup_parent ? s->popup_parent->sprot_id : 0)
            : (s->subsurface_parent ? s->subsurface_parent->sprot_id : 0);
        int32_t x = s->is_popup ? popup_sprot_x(s) : s->subsurface_x;
        int32_t y = s->is_popup ? popup_sprot_y(s) : s->subsurface_y;
        debug_log("Set Sprot role to %s for Sprot ID=%u parent=%u pos=%d,%d (res=%d)",
            s->is_popup ? "SPROT_SURFACE_ROLE_POPUP" : "SPROT_SURFACE_ROLE_SUBSURFACE",
            s->sprot_id, parent_id, x, y, role_res);
    } else {
        debug_log("Set Sprot role to SPROT_SURFACE_ROLE_TOPLEVEL for Sprot ID=%u (res=%d)",
                  s->sprot_id, role_res);
    }
    if (role_res != 0) return;

    if (!s->pending_commit) return;

    if (s->pending_dmabuf) {
        debug_log("Executing deferred DMA-BUF commit for surface Sprot ID=%u", s->sprot_id);
        int dup_fd = dup(s->pending_dmabuf->fd);
        if (dup_fd >= 0) {
            int attach_res = sprot_surface_attach_dmabuf(
                s->sprot_surface,
                dup_fd,
                s->pending_dmabuf->width,
                s->pending_dmabuf->height,
                s->pending_dmabuf->format,
                s->pending_dmabuf->modifier,
                s->pending_dmabuf->num_planes,
                s->pending_dmabuf->offsets,
                s->pending_dmabuf->strides,
                s->pending_dmabuf->strides[0] * s->pending_dmabuf->height
            );
            close(dup_fd);
            int commit_res = sprot_commit(s->sprot_surface);
            sprot_request_frame(s->sprot_surface);
            debug_log("Deferred DMA-BUF commit results: attach=%d, commit=%d", attach_res, commit_res);
        } else {
            debug_log("Error: deferred dup failed for dmabuf: %s", strerror(errno));
        }
        s->pending_dmabuf = NULL;
    } else {
        debug_log("Executing deferred SHM commit for surface Sprot ID=%u", s->sprot_id);
        int dup_fd = dup(s->memfd);
        if (dup_fd >= 0) {
            int attach_res = sprot_attach_fd(s->sprot_surface, dup_fd, s->pending_width,
                                             s->pending_height, s->pending_stride,
                                             (uint32_t)s->pending_size, SPROT_BUFFER_SHM,
                                             SPROT_PIXEL_FORMAT_BGRA8888);
            close(dup_fd);
            int commit_res = sprot_commit(s->sprot_surface);
            sprot_request_frame(s->sprot_surface);
            debug_log("Deferred SHM commit results: attach=%d, commit=%d", attach_res, commit_res);
        } else {
            debug_log("Error: deferred dup failed: %s", strerror(errno));
        }
    }
    s->pending_commit = 0;

    if (s->buffer_resource) {
        wl_buffer_send_release(s->buffer_resource);
        s->buffer_resource = NULL;
    }
}

static void flush_children_waiting_for_parent(struct bridge_surface *parent) {
    if (!parent || !parent->client || parent->sprot_id == 0) return;
    struct bridge_surface *child;
    wl_list_for_each(child, &parent->client->surfaces, link) {
        if ((child->is_popup && child->popup_parent == parent) ||
            (child->is_subsurface && child->subsurface_parent == parent)) {
            flush_pending_surface_commit(child);
        }
    }
}

// Sprot FD event handler
static int client_sprot_fd_handler(int fd, uint32_t mask, void *data) {
    struct bridge_client *c = data;
    (void)fd;
    (void)mask;

    sprot_event_t ev;
    while (sprot_poll_event(c->sprot_conn, &ev, 0) > 0) {
        switch (ev.kind) {
            case SPROT_EVENT_WELCOME:
                debug_log("Sprot event: Welcome received.");
                break;

            case SPROT_EVENT_SURFACE_CREATED: {
                struct bridge_surface *s = find_surface_by_sprot_id(c, ev.u.surface_created.surface_id);
                if (s) {
                    s->sprot_id = ev.u.surface_created.surface_id;
                    debug_log("Sprot event: Surface created. Sprot ID=%u assigned to handle=%u (sprot_handle=%u)",
                        s->sprot_id, s->client_handle, ev.u.surface_created.client_handle);
                    flush_pending_surface_commit(s);
                    flush_children_waiting_for_parent(s);
                }
                break;
            }

            case SPROT_EVENT_SURFACE_CONFIGURE: {
                struct bridge_surface *s = find_surface_by_sprot_id(c, ev.object_id);
                if (s) {
                    s->width = ev.u.configure.width;
                    s->height = ev.u.configure.height;
                    debug_log("Sprot event: Configure surface handle=%u (Sprot ID=%u) to size: %dx%d", s->client_handle, s->sprot_id, s->width, s->height);

                    if (s->xdg_toplevel_resource) {
                        struct wl_array states;
                        wl_array_init(&states);
                        if (ev.u.configure.state & SPROT_SURFACE_STATE_MAXIMIZED) {
                            uint32_t *p = wl_array_add(&states, sizeof(uint32_t));
                            if (p != NULL) *p = XDG_TOPLEVEL_STATE_MAXIMIZED;
                        }
                        if (ev.u.configure.state & SPROT_SURFACE_STATE_FOCUSED) {
                            uint32_t *p = wl_array_add(&states, sizeof(uint32_t));
                            if (p != NULL) *p = XDG_TOPLEVEL_STATE_ACTIVATED;
                        }
                        xdg_toplevel_send_configure(s->xdg_toplevel_resource, s->width, s->height, &states);
                        wl_array_release(&states);
                    }
                    if (s->xdg_surface_resource) {
                        xdg_surface_send_configure(s->xdg_surface_resource, ev.serial);
                    }
                }
                break;
            }

            case SPROT_EVENT_SURFACE_CLOSE: {
                struct bridge_surface *s = find_surface_by_sprot_id(c, ev.object_id);
                debug_log("Sprot event: Close surface Sprot ID=%u", ev.object_id);
                if (s && s->xdg_popup_resource) {
                    xdg_popup_send_popup_done(s->xdg_popup_resource);
                } else if (s && s->xdg_toplevel_resource) {
                    xdg_toplevel_send_close(s->xdg_toplevel_resource);
                }
                break;
            }

            case SPROT_EVENT_SURFACE_FRAME: {
                struct bridge_surface *s = find_surface_by_sprot_id(c, ev.object_id);
                if (s) {
                    struct wl_resource *cb, *tmp;
                    int cb_count = 0;
                    wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
                        wl_callback_send_done(cb, ev.u.frame.time_ms);
                        wl_resource_destroy(cb);
                        cb_count++;
                    }
                    if (cb_count > 0) {
                        debug_log("Sprot event: Frame update for surface Sprot ID=%u. Serviced %d Wayland callbacks.", ev.object_id, cb_count);
                    }
                }
                break;
            }

            case SPROT_EVENT_POINTER_ENTER: {
                struct bridge_surface *s = find_surface_by_sprot_id(c, ev.object_id);
                if (s) {
                    uint32_t serial = ev.serial ? ev.serial : next_keyboard_serial(c);
                    debug_log("Sprot event: Pointer entered surface handle=%u at (%d, %d)", s->client_handle, ev.u.pointer_motion.x, ev.u.pointer_motion.y);
                    struct seat_resource *sr;
                    wl_list_for_each(sr, &c->seat_pointers, link) {
                        wl_pointer_send_enter(sr->resource, serial, s->resource,
                            wl_fixed_from_int(ev.u.pointer_motion.x),
                            wl_fixed_from_int(ev.u.pointer_motion.y));
                    }
                    set_keyboard_focus(c, s, serial);
                }
                break;
            }

            case SPROT_EVENT_POINTER_LEAVE: {
                struct bridge_surface *s = find_surface_by_sprot_id(c, ev.object_id);
                if (s) {
                    uint32_t serial = ev.serial ? ev.serial : next_keyboard_serial(c);
                    debug_log("Sprot event: Pointer left surface handle=%u", s->client_handle);
                    struct seat_resource *sr;
                    wl_list_for_each(sr, &c->seat_pointers, link) {
                        wl_pointer_send_leave(sr->resource, serial, s->resource);
                    }
                    if (c->keyboard_focus == s) {
                        set_keyboard_focus(c, NULL, serial);
                    }
                }
                break;
            }

            case SPROT_EVENT_POINTER_MOTION: {
                struct seat_resource *sr;
                uint32_t time_ms = wlbridge_now_ms();
                wl_list_for_each(sr, &c->seat_pointers, link) {
                    wl_pointer_send_motion(sr->resource, time_ms,
                        wl_fixed_from_int(ev.u.pointer_motion.x),
                        wl_fixed_from_int(ev.u.pointer_motion.y));
                }
                break;
            }

            case SPROT_EVENT_POINTER_BUTTON: {
                struct seat_resource *sr;
                uint32_t wl_button = wayland_button_from_srapi(ev.u.pointer_button.button);
                debug_log("Sprot event: Pointer button button=%u wl_button=%u state=%u",
                    ev.u.pointer_button.button, wl_button, ev.u.pointer_button.state);
                uint32_t serial = ev.serial ? ev.serial : next_keyboard_serial(c);
                uint32_t time_ms = wlbridge_now_ms();
                wl_list_for_each(sr, &c->seat_pointers, link) {
                    // Translate button state: pressed = 1, released = 0
                    uint32_t state = ev.u.pointer_button.state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
                    wl_pointer_send_button(sr->resource, serial, time_ms, wl_button, state);
                }
                break;
            }

            case SPROT_EVENT_KEY: {
                debug_log("Sprot event: Keyboard key scancode=%u state=%u", ev.u.key.scancode, ev.u.key.state);
                if (c->keyboard_focus != NULL) {
                    uint32_t evdev_key = srapi_scancode_to_evdev(ev.u.key.scancode);
                    if (evdev_key == 0) {
                        debug_log("Sprot event: unmapped SRAPI scancode=%u", ev.u.key.scancode);
                        break;
                    }
                    uint32_t serial = ev.serial ? ev.serial : next_keyboard_serial(c);
                    uint32_t state = ev.u.key.state ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
                    update_pressed_keys(c, evdev_key, ev.u.key.state);

                    struct seat_resource *sr;
                    wl_list_for_each(sr, &c->seat_keyboards, link) {
                        wl_keyboard_send_key(sr->resource, serial, 0, evdev_key, state);
                    }
                    send_keyboard_modifiers(c, serial);
                }
                break;
            }

            case SPROT_EVENT_ERROR:
                debug_log("Sprot event error: code=%u, message=%s. Terminating client connection.", ev.u.error.code, ev.u.error.message);
                wl_client_destroy(c->wl_client);
                return 0;

            case SPROT_EVENT_DISCONNECT:
                debug_log("Sprot event: Disconnect received. Terminating client connection.");
                wl_client_destroy(c->wl_client);
                return 0;

            default:
                break;
        }
    }
    return 1;
}

static void client_destroy_listener(struct wl_listener *listener, void *data) {
    struct bridge_client *c = wl_container_of(listener, c, destroy_listener);
    (void)data;

    debug_log("Wayland client disconnected. Cleaning up resources.");

    if (c->sprot_source) {
        wl_event_source_remove(c->sprot_source);
    }
    struct bridge_surface *s, *stmp;
    wl_list_for_each_safe(s, stmp, &c->surfaces, link) {
        destroy_bridge_surface(s);
    }

    if (c->sprot_conn) {
        sprot_disconnect(c->sprot_conn);
    }

    struct seat_resource *sr, *srtmp;
    wl_list_for_each_safe(sr, srtmp, &c->seat_pointers, link) {
        free_seat_resource(sr);
    }
    wl_list_for_each_safe(sr, srtmp, &c->seat_keyboards, link) {
        free_seat_resource(sr);
    }
    wl_list_for_each_safe(sr, srtmp, &c->seat_resources, link) {
        free_seat_resource(sr);
    }

    wl_list_remove(&c->link);
    free(c);
}

// Client registration
static struct bridge_client *get_or_create_client(struct wl_client *wl_client) {
    struct bridge_client *c;
    wl_list_for_each(c, &g_server.clients, link) {
        if (c->wl_client == wl_client) {
            return c;
        }
    }

    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(wl_client, &pid, &uid, &gid);
    debug_log("New Wayland client connected. pid=%d Initializing Sprot connection...", (int)pid);

    c = calloc(1, sizeof(*c));
    c->wl_client = wl_client;
    wl_list_init(&c->surfaces);
    wl_list_init(&c->seat_pointers);
    wl_list_init(&c->seat_keyboards);
    wl_list_init(&c->seat_resources);

    c->sprot_conn = sprot_connect(g_server.swm_socket);
    if (!c->sprot_conn) {
        fprintf(stderr, "[wlbridge] Failed to connect to SWM socket: %s\n", sprot_last_error());
        debug_log("Failed to connect to SWM socket: %s", sprot_last_error());
        free(c);
        return NULL;
    }

    int sprot_fd = sprot_connection_fd(c->sprot_conn);
    c->sprot_source = wl_event_loop_add_fd(g_server.loop, sprot_fd, WL_EVENT_READABLE, client_sprot_fd_handler, c);

    c->destroy_listener.notify = client_destroy_listener;
    wl_client_add_destroy_listener(wl_client, &c->destroy_listener);

    wl_list_insert(&g_server.clients, &c->link);
    debug_log("Client registration complete.");
    return c;
}

// Surface interface implementation
static void surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void surface_attach(struct wl_client *client, struct wl_resource *resource, struct wl_resource *buffer, int32_t x, int32_t y) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    (void)client;
    (void)x;
    (void)y;
    debug_log("surface_attach: handle=%u buffer=%s", s->client_handle, buffer ? "set" : "null");
    s->buffer_resource = buffer;
}

static void surface_damage(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static void send_cursor_image(struct bridge_client *c, struct bridge_surface *cursor_surface) {
    if (!c || !c->keyboard_focus || !c->keyboard_focus->sprot_surface) {
        return;
    }

    if (!cursor_surface->buffer_resource) {
        int res = sprot_set_cursor_image(c->keyboard_focus->sprot_surface, -1, 0, 0, 0, 0, 0, 0, 0);
        debug_log("cursor_image: hide res=%d", res);
        return;
    }

    struct wl_shm_buffer *shm_buf = wl_shm_buffer_get(cursor_surface->buffer_resource);
    if (!shm_buf) {
        debug_log("cursor_image: cursor buffer is not SHM");
        return;
    }

    int32_t width = wl_shm_buffer_get_width(shm_buf);
    int32_t height = wl_shm_buffer_get_height(shm_buf);
    int32_t stride = wl_shm_buffer_get_stride(shm_buf);
    size_t size = (size_t)stride * height;
    if (width <= 0 || height <= 0 || stride < width * 4 || size == 0) {
        return;
    }

    int fd = memfd_create("wlbridge-cursor", MFD_CLOEXEC);
    if (fd < 0) {
        debug_log("cursor_image: memfd_create failed: %s", strerror(errno));
        return;
    }
    if (ftruncate(fd, size) != 0) {
        debug_log("cursor_image: ftruncate failed: %s", strerror(errno));
        close(fd);
        return;
    }
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        debug_log("cursor_image: mmap failed: %s", strerror(errno));
        close(fd);
        return;
    }

    wl_shm_buffer_begin_access(shm_buf);
    void *src = wl_shm_buffer_get_data(shm_buf);
    if (src) {
        memcpy(map, src, size);
    }
    wl_shm_buffer_end_access(shm_buf);
    munmap(map, size);

    int res = sprot_set_cursor_image(c->keyboard_focus->sprot_surface, fd,
        (uint32_t)width, (uint32_t)height, (uint32_t)stride, (uint32_t)size,
        cursor_surface->cursor_hotspot_x, cursor_surface->cursor_hotspot_y, 1);
    debug_log("cursor_image: sent %dx%d hotspot=%d,%d res=%d",
        width, height, cursor_surface->cursor_hotspot_x, cursor_surface->cursor_hotspot_y, res);
    close(fd);
}

static void callback_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void surface_frame(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    debug_log("surface_frame: handle=%u id=%u", s->client_handle, id);
    struct wl_resource *callback = wl_resource_create(client, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(callback, NULL, NULL, callback_resource_destroy);
    wl_list_insert(&s->frame_callbacks, wl_resource_get_link(callback));
}

static void finish_frame_callbacks(struct bridge_surface *s) {
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
        wl_callback_send_done(cb, 0);
        wl_resource_destroy(cb);
    }
}

static void release_attached_buffer(struct bridge_surface *s) {
    if (s->buffer_resource) {
        wl_buffer_send_release(s->buffer_resource);
        s->buffer_resource = NULL;
    }
}

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void handle_dmabuf_buffer(struct bridge_surface *s, struct dmabuf_buffer *dmabuf) {
    if (!dmabuf || !dmabuf->is_dmabuf) {
        debug_log("handle_dmabuf_buffer: invalid dmabuf buffer");
        return;
    }

    debug_log("handle_dmabuf_buffer: handle=%u dmabuf %dx%d format=0x%x modifier=0x%lx stride=%u",
              s->client_handle, dmabuf->width, dmabuf->height, dmabuf->format,
              (unsigned long)dmabuf->modifier, dmabuf->strides[0]);

    if (!s->sprot_surface) {
        debug_log("Creating new Sprot surface for dmabuf client...");
        s->sprot_surface = sprot_create_surface(s->client->sprot_conn, dmabuf->width, dmabuf->height);
        if (!s->sprot_surface) {
            debug_log("Error: Failed to create Sprot surface for dmabuf.");
            return;
        }
    }

    if (s->sprot_id == 0 || !surface_parent_ready(s)) {
        debug_log("Sprot surface/parent is not ready. Deferring dmabuf attach.");
        s->pending_dmabuf = dmabuf;
        s->pending_commit = 1;
        return;
    }

    int dup_fd = dup(dmabuf->fd);
    if (dup_fd < 0) {
        debug_log("Error: dup failed for dmabuf fd: %s", strerror(errno));
        return;
    }

    debug_log("Forwarding dmabuf buffer to SWM (Sprot ID=%u)", s->sprot_id);
    int attach_res = sprot_surface_attach_dmabuf(
        s->sprot_surface,
        dup_fd,
        dmabuf->width,
        dmabuf->height,
        dmabuf->format,
        dmabuf->modifier,
        dmabuf->num_planes,
        dmabuf->offsets,
        dmabuf->strides,
        dmabuf->strides[0] * dmabuf->height
    );
    close(dup_fd);

    if (attach_res != 0) {
        debug_log("Error: sprot_surface_attach_dmabuf failed: %d", attach_res);
        return;
    }

    int commit_res = sprot_commit(s->sprot_surface);
    sprot_request_frame(s->sprot_surface);
    debug_log("DMA-BUF commit results: attach=%d, commit=%d", attach_res, commit_res);
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    (void)client;

    debug_log("surface_commit: handle=%u buffer=%s configured=%d sprot_id=%u",
        s->client_handle, s->buffer_resource ? "set" : "null", s->configured, s->sprot_id);

    if (s->is_cursor) {
        send_cursor_image(s->client, s);
        debug_log("surface_commit: skipping cursor handle=%u pos=%d,%d",
            s->client_handle, s->subsurface_x, s->subsurface_y);
        release_attached_buffer(s);
        finish_frame_callbacks(s);
        return;
    }

    if (!s->xdg_toplevel_resource && !s->xdg_popup_resource && !s->is_subsurface) {
        debug_log("surface_commit: skipping unroled surface handle=%u", s->client_handle);
        finish_frame_callbacks(s);
        return;
    }

    if (!s->buffer_resource) {
        if (!s->configured) {
            debug_log("surface_commit: initial commit (no buffer) for handle=%u. Sending XDG configure.", s->client_handle);
            if (s->is_subsurface) {
                debug_log("surface_commit: subsurface handle=%u has no buffer", s->client_handle);
                finish_frame_callbacks(s);
                return;
            }
            if (s->xdg_toplevel_resource) {
                struct wl_array states;
                wl_array_init(&states);
                xdg_toplevel_send_configure(s->xdg_toplevel_resource, 0, 0, &states);
                wl_array_release(&states);
            } else if (s->xdg_popup_resource) {
                configure_popup_surface(s, 0, 0);
            }
            if (s->xdg_surface_resource && !s->xdg_popup_resource) {
                xdg_surface_send_configure(s->xdg_surface_resource, 1);
            }
            s->configured = 1;
        } else {
            debug_log("surface_commit: no buffer, already configured. Skipping.");
        }
        return;
    }

    struct wl_shm_buffer *shm_buf = wl_shm_buffer_get(s->buffer_resource);
    if (shm_buf) {
        int32_t width = wl_shm_buffer_get_width(shm_buf);
        int32_t height = wl_shm_buffer_get_height(shm_buf);
        int32_t stride = wl_shm_buffer_get_stride(shm_buf);
        size_t size = (size_t)stride * height;

        if (width <= 0 || height <= 0) return;

        debug_log("Surface commit for surface handle=%u. Attached SHM buffer size: %dx%d, stride: %d, size: %zu", s->client_handle, width, height, stride, size);

        if (!s->sprot_surface) {
            debug_log("Creating new Sprot surface for client connection...");
            s->sprot_surface = sprot_create_surface(s->client->sprot_conn, width, height);
            if (!s->sprot_surface) {
                debug_log("Error: Failed to create Sprot surface.");
                return;
            }
        }

        if (s->memfd < 0 || s->memfd_size < size) {
            if (s->memfd_map && s->memfd_map != MAP_FAILED) {
                munmap(s->memfd_map, s->memfd_size);
            }
            if (s->memfd >= 0) {
                close(s->memfd);
            }

            s->memfd = memfd_create("wlbridge-shm", MFD_CLOEXEC);
            if (s->memfd < 0) {
                debug_log("Error: memfd_create failed: %s", strerror(errno));
                return;
            }
            if (ftruncate(s->memfd, size) != 0) {
                close(s->memfd);
                s->memfd = -1;
                debug_log("Error: ftruncate failed: %s", strerror(errno));
                return;
            }
            s->memfd_size = size;
            s->memfd_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->memfd, 0);
            if (s->memfd_map == MAP_FAILED) {
                close(s->memfd);
                s->memfd = -1;
                debug_log("Error: mmap failed: %s", strerror(errno));
                return;
            }
            debug_log("Created new shared memory mapping (memfd) of size %zu for surface handle=%u", size, s->client_handle);
        }

        wl_shm_buffer_begin_access(shm_buf);
        void *src_pixels = wl_shm_buffer_get_data(shm_buf);
        if (src_pixels && s->memfd_map) {
            memcpy(s->memfd_map, src_pixels, size);
        }
        wl_shm_buffer_end_access(shm_buf);

        if (s->sprot_id == 0 || !surface_parent_ready(s)) {
            debug_log("Sprot surface/parent is not ready. Deferring attach and commit.");
            s->pending_commit = 1;
            s->pending_width = width;
            s->pending_height = height;
            s->pending_stride = stride;
            s->pending_size = size;
        } else {
            int dup_fd = dup(s->memfd);
            if (dup_fd >= 0) {
                debug_log("Forwarding SHM buffer attach to SWM (Sprot ID=%u)", s->sprot_id);
                int attach_res = sprot_attach_fd(s->sprot_surface, dup_fd, width, height, stride, (uint32_t)size, SPROT_BUFFER_SHM, SPROT_PIXEL_FORMAT_BGRA8888);
                close(dup_fd);
                int commit_res = sprot_commit(s->sprot_surface);
                sprot_request_frame(s->sprot_surface);
                debug_log("SHM commit results: attach=%d, commit=%d", attach_res, commit_res);
            } else {
                debug_log("Error: dup failed: %s", strerror(errno));
            }
        }
    } else {
        struct dmabuf_buffer *dmabuf = wl_resource_get_user_data(s->buffer_resource);
        if (dmabuf && dmabuf->is_dmabuf) {
            debug_log("surface_commit: handle=%u using DMA-BUF buffer", s->client_handle);
            handle_dmabuf_buffer(s, dmabuf);
        } else {
            debug_log("surface_commit: WARNING handle=%u buffer is neither SHM nor DMA-BUF", s->client_handle);
            return;
        }
    }

    if (!s->pending_commit) {
        wl_buffer_send_release(s->buffer_resource);
        s->buffer_resource = NULL;
    }
}

static void surface_set_buffer_transform(struct wl_client *client, struct wl_resource *resource, int32_t transform) {
    (void)client; (void)resource; (void)transform;
}

static void surface_set_buffer_scale(struct wl_client *client, struct wl_resource *resource, int32_t scale) {
    (void)client; (void)resource; (void)scale;
}

static void surface_damage_buffer(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
}

static const struct wl_surface_interface surface_implementation = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
};

static void surface_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        debug_log("Surface resource destroyed. handle=%u", s->client_handle);
        destroy_bridge_surface(s);
    }
}

// Region implementation
static void region_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    debug_log("region_destroy: id=%u", wl_resource_get_id(resource));
    wl_resource_destroy(resource);
}

static void region_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    debug_log("region_add: id=%u rect=%d,%d %dx%d",
              wl_resource_get_id(resource), x, y, width, height);
}

static void region_subtract(struct wl_client *client, struct wl_resource *resource,
                            int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    debug_log("region_subtract: id=%u rect=%d,%d %dx%d",
              wl_resource_get_id(resource), x, y, width, height);
}

static const struct wl_region_interface region_implementation = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

// Compositor interface implementation
static void compositor_create_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_client *c = get_or_create_client(client);
    if (!c) {
        return;
    }

    struct bridge_surface *s = calloc(1, sizeof(*s));
    s->client = c;
    s->memfd = -1;
    s->client_handle = ++g_server.next_client_handle;
    wl_list_init(&s->frame_callbacks);

    debug_log("Wayland surface created. Assigned internal handle=%u", s->client_handle);

    s->resource = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(s->resource, &surface_implementation, s, surface_resource_destroy);

    wl_list_insert(&c->surfaces, &s->link);
}

static void compositor_create_region(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    debug_log("compositor_create_region: id=%u", id);
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, wl_resource_get_version(resource), id);
    if (!region) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(region, &region_implementation, NULL, NULL);
}

static const struct wl_compositor_interface compositor_implementation = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("compositor_bind: version=%u id=%u pid=%d", version, id, (int)pid);
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &compositor_implementation, NULL, NULL);
}

// XDG WM Base (xdg_wm_base) implementation
static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_positioner_resource_destroy(struct wl_resource *resource) {
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    free(pos);
}

static void xdg_positioner_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_positioner_set_size(struct wl_client *client, struct wl_resource *resource,
                                    int32_t width, int32_t height) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad positioner size");
        return;
    }
    pos->has_size = 1;
    pos->width = width;
    pos->height = height;
}

static void xdg_positioner_set_anchor_rect(struct wl_client *client, struct wl_resource *resource,
                                           int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad anchor rect");
        return;
    }
    pos->has_anchor_rect = 1;
    pos->anchor_rect_x = x;
    pos->anchor_rect_y = y;
    pos->anchor_rect_width = width;
    pos->anchor_rect_height = height;
}

static void xdg_positioner_set_anchor(struct wl_client *client, struct wl_resource *resource, uint32_t anchor) {
    (void)client;
    if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad anchor");
        return;
    }
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->anchor = anchor;
}

static void xdg_positioner_set_gravity(struct wl_client *client, struct wl_resource *resource, uint32_t gravity) {
    (void)client;
    if (!xdg_positioner_gravity_is_valid(gravity, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad gravity");
        return;
    }
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->gravity = gravity;
}

static void xdg_positioner_set_constraint_adjustment(struct wl_client *client, struct wl_resource *resource,
                                                     uint32_t constraint_adjustment) {
    (void)client;
    if (!xdg_positioner_constraint_adjustment_is_valid(constraint_adjustment, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad constraint adjustment");
        return;
    }
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->constraint_adjustment = constraint_adjustment;
}

static void xdg_positioner_set_offset(struct wl_client *client, struct wl_resource *resource,
                                      int32_t x, int32_t y) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->offset_x = x;
    pos->offset_y = y;
}

static void xdg_positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->reactive = 1;
}

static void xdg_positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource,
                                           int32_t parent_width, int32_t parent_height) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->has_parent_size = parent_width > 0 && parent_height > 0;
    pos->parent_width = parent_width;
    pos->parent_height = parent_height;
}

static void xdg_positioner_set_parent_configure(struct wl_client *client, struct wl_resource *resource,
                                                uint32_t serial) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->has_parent_configure = 1;
    pos->parent_configure_serial = serial;
}

static const struct xdg_positioner_interface xdg_positioner_implementation = {
    .destroy = xdg_positioner_destroy,
    .set_size = xdg_positioner_set_size,
    .set_anchor_rect = xdg_positioner_set_anchor_rect,
    .set_anchor = xdg_positioner_set_anchor,
    .set_gravity = xdg_positioner_set_gravity,
    .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
    .set_offset = xdg_positioner_set_offset,
    .set_reactive = xdg_positioner_set_reactive,
    .set_parent_size = xdg_positioner_set_parent_size,
    .set_parent_configure = xdg_positioner_set_parent_configure,
};

static void xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    debug_log("xdg_wm_base_create_positioner: id=%u", id);
    struct xdg_positioner_data *pos = calloc(1, sizeof(*pos));
    if (!pos) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *positioner = wl_resource_create(client, &xdg_positioner_interface,
                                                       wl_resource_get_version(resource), id);
    if (!positioner) {
        free(pos);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(positioner, &xdg_positioner_implementation,
                                   pos, xdg_positioner_resource_destroy);
}

static void xdg_surface_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        s->xdg_surface_resource = NULL;
    }
}

static void xdg_surface_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_toplevel_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s && s->xdg_toplevel_resource == resource) {
        s->xdg_toplevel_resource = NULL;
    }
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    debug_log("Wayland request: xdg_surface.get_toplevel for surface handle=%u", s->client_handle);

    if (s->xdg_toplevel_resource || s->xdg_popup_resource) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "xdg_surface already has a role");
        return;
    }

    s->xdg_toplevel_resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(s->xdg_toplevel_resource, &xdg_toplevel_implementation, s, xdg_toplevel_resource_destroy);
}

static void xdg_popup_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (!s || s->xdg_popup_resource != resource) return;

    debug_log("xdg_popup destroyed for surface handle=%u", s->client_handle);
    s->xdg_popup_resource = NULL;
    s->is_popup = 0;
    s->popup_grabbed = 0;
    s->popup_parent = NULL;
    s->configured = 0;
    if (s->sprot_surface) {
        sprot_destroy_surface(s->sprot_surface);
        s->sprot_surface = NULL;
        s->sprot_id = 0;
    }
}

static void xdg_popup_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_popup_grab(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)seat;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        s->popup_grabbed = 1;
        debug_log("xdg_popup.grab: handle=%u serial=%u", s->client_handle, serial);
    }
}

static void xdg_popup_reposition(struct wl_client *client, struct wl_resource *resource,
                                 struct wl_resource *positioner, uint32_t token) {
    (void)client;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    struct xdg_positioner_data *pos = positioner ? wl_resource_get_user_data(positioner) : NULL;
    if (!s || !pos || !pos->has_size || !pos->has_anchor_rect) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "bad popup repositioner");
        return;
    }

    compute_popup_geometry(s, s->popup_parent, pos);
    if (s->sprot_surface && s->sprot_id != 0) {
        send_sprot_role_for_surface(s);
    }
    debug_log("xdg_popup.reposition: handle=%u pos=%d,%d size=%dx%d token=%u",
              s->client_handle, s->popup_x, s->popup_y,
              s->popup_width, s->popup_height, token);
    configure_popup_surface(s, token, 1);
}

static const struct xdg_popup_interface xdg_popup_implementation = {
    .destroy = xdg_popup_destroy,
    .grab = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    struct bridge_surface *parent_s = parent ? wl_resource_get_user_data(parent) : NULL;
    struct xdg_positioner_data *pos = positioner ? wl_resource_get_user_data(positioner) : NULL;
    debug_log("Wayland request: xdg_surface.get_popup for surface handle=%u", s ? s->client_handle : 0);

    if (!s || !parent_s || (!parent_s->xdg_toplevel_resource && !parent_s->xdg_popup_resource)) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, "bad popup parent");
        return;
    }
    if (!pos || !pos->has_size || !pos->has_anchor_rect) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "bad popup positioner");
        return;
    }
    if (s->xdg_toplevel_resource || s->xdg_popup_resource) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "xdg_surface already has a role");
        return;
    }

    s->xdg_popup_resource = wl_resource_create(client, &xdg_popup_interface,
                                               wl_resource_get_version(resource), id);
    if (!s->xdg_popup_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    s->is_popup = 1;
    s->popup_parent = parent_s;
    compute_popup_geometry(s, parent_s, pos);

    wl_resource_set_implementation(s->xdg_popup_resource, &xdg_popup_implementation,
                                   s, xdg_popup_resource_destroy);
    debug_log("xdg_surface.get_popup: handle=%u parent_handle=%u pos=%d,%d size=%dx%d",
              s->client_handle, parent_s->client_handle, s->popup_x, s->popup_y,
              s->popup_width, s->popup_height);
    configure_popup_surface(s, 0, 0);
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s && width > 0 && height > 0) {
        s->window_geometry_set = 1;
        s->window_geometry_x = x;
        s->window_geometry_y = y;
        s->window_geometry_width = width;
        s->window_geometry_height = height;
    }
    debug_log("Wayland request: xdg_surface.set_window_geometry to %d,%d %dx%d", x, y, width, height);
}

static void xdg_surface_ack_configure(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    (void)client;
    debug_log("xdg_surface_ack_configure: handle=%u serial=%u", s ? s->client_handle : 0, serial);
}

static const struct xdg_surface_interface xdg_surface_implementation = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_wm_base_get_xdg_surface(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
    struct bridge_surface *s = wl_resource_get_user_data(surface_resource);
    debug_log("Wayland request: xdg_wm_base.get_xdg_surface for surface handle=%u", s->client_handle);

    s->xdg_surface_resource = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(s->xdg_surface_resource, &xdg_surface_implementation, s, xdg_surface_resource_destroy);
}

static void xdg_wm_base_pong(struct wl_client *client, struct wl_resource *resource, uint32_t serial) {
    (void)client; (void)resource; (void)serial;
    debug_log("Wayland request: xdg_wm_base.pong serial=%u", serial);
}

static const struct xdg_wm_base_interface xdg_wm_base_implementation = {
    .destroy = xdg_wm_base_destroy,
    .create_positioner = xdg_wm_base_create_positioner,
    .get_xdg_surface = xdg_wm_base_get_xdg_surface,
    .pong = xdg_wm_base_pong,
};

static void xdg_wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("xdg_wm_base_bind: version=%u id=%u pid=%d", version, id, (int)pid);
    struct wl_resource *resource = wl_resource_create(client, &xdg_wm_base_interface, version, id);
    wl_resource_set_implementation(resource, &xdg_wm_base_implementation, NULL, NULL);
}

// XDG Toplevel operations
static void xdg_toplevel_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_toplevel_set_parent(struct wl_client *client, struct wl_resource *resource, struct wl_resource *parent) {
    (void)client; (void)resource; (void)parent;
}

static void xdg_toplevel_set_title(struct wl_client *client, struct wl_resource *resource, const char *title) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    (void)client;
    if (s->sprot_surface) {
        sprot_set_title(s->sprot_surface, title);
    }
}

static void xdg_toplevel_set_app_id(struct wl_client *client, struct wl_resource *resource, const char *app_id) {
    (void)client; (void)resource; (void)app_id;
}

static void xdg_toplevel_show_window_menu(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y) {
    (void)client; (void)resource; (void)seat; (void)serial; (void)x; (void)y;
}

static void xdg_toplevel_move(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)resource; (void)seat; (void)serial;
}

static void xdg_toplevel_resize(struct wl_client *client, struct wl_resource *resource, struct wl_resource *seat, uint32_t serial, uint32_t edges) {
    (void)client; (void)resource; (void)seat; (void)serial; (void)edges;
}

static void xdg_toplevel_set_max_size(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static void xdg_toplevel_set_min_size(struct wl_client *client, struct wl_resource *resource, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)width; (void)height;
}

static void xdg_toplevel_set_maximized(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_toplevel_unset_maximized(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_toplevel_set_fullscreen(struct wl_client *client, struct wl_resource *resource, struct wl_resource *output) {
    (void)client; (void)resource; (void)output;
}

static void xdg_toplevel_unset_fullscreen(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void xdg_toplevel_set_minimized(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static const struct xdg_toplevel_interface xdg_toplevel_implementation = {
    .destroy = xdg_toplevel_destroy,
    .set_parent = xdg_toplevel_set_parent,
    .set_title = xdg_toplevel_set_title,
    .set_app_id = xdg_toplevel_set_app_id,
    .show_window_menu = xdg_toplevel_show_window_menu,
    .move = xdg_toplevel_move,
    .resize = xdg_toplevel_resize,
    .set_max_size = xdg_toplevel_set_max_size,
    .set_min_size = xdg_toplevel_set_min_size,
    .set_maximized = xdg_toplevel_set_maximized,
    .unset_maximized = xdg_toplevel_unset_maximized,
    .set_fullscreen = xdg_toplevel_set_fullscreen,
    .unset_fullscreen = xdg_toplevel_unset_fullscreen,
    .set_minimized = xdg_toplevel_set_minimized,
};

// Data device (clipboard/DnD) stubs
static void data_source_offer(struct wl_client *client, struct wl_resource *resource, const char *mime_type) {
    (void)client; (void)resource;
    debug_log("data_source_offer: %s", mime_type ? mime_type : "(null)");
}

static void data_source_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void data_source_set_actions(struct wl_client *client, struct wl_resource *resource, uint32_t dnd_actions) {
    (void)client; (void)resource;
    debug_log("data_source_set_actions: 0x%x", dnd_actions);
}

static const struct wl_data_source_interface data_source_implementation = {
    .offer = data_source_offer,
    .destroy = data_source_destroy,
    .set_actions = data_source_set_actions,
};

static void data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *source, struct wl_resource *origin,
                                   struct wl_resource *icon, uint32_t serial) {
    (void)client; (void)resource; (void)source; (void)origin; (void)icon;
    debug_log("data_device_start_drag: serial=%u (ignored)", serial);
}

static void data_device_set_selection(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *source, uint32_t serial) {
    (void)client; (void)resource; (void)source;
    debug_log("data_device_set_selection: serial=%u (ignored)", serial);
}

static void data_device_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_data_device_interface data_device_implementation = {
    .start_drag = data_device_start_drag,
    .set_selection = data_device_set_selection,
    .release = data_device_release,
};

static void data_device_manager_create_data_source(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    uint32_t version = wl_resource_get_version(resource);
    debug_log("data_device_manager_create_data_source: id=%u version=%u", id, version);
    struct wl_resource *source = wl_resource_create(client, &wl_data_source_interface, version, id);
    if (!source) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(source, &data_source_implementation, NULL, NULL);
}

static void data_device_manager_get_data_device(struct wl_client *client, struct wl_resource *resource,
                                                uint32_t id, struct wl_resource *seat) {
    uint32_t version = wl_resource_get_version(resource);
    debug_log("data_device_manager_get_data_device: id=%u seat=%u version=%u",
              id, seat ? wl_resource_get_id(seat) : 0, version);
    struct wl_resource *device = wl_resource_create(client, &wl_data_device_interface, version, id);
    if (!device) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(device, &data_device_implementation, NULL, NULL);
    wl_data_device_send_selection(device, NULL);
}

static const struct wl_data_device_manager_interface data_device_manager_implementation = {
    .create_data_source = data_device_manager_create_data_source,
    .get_data_device = data_device_manager_get_data_device,
};

static void data_device_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("data_device_manager_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &data_device_manager_implementation, NULL, NULL);
}

// Subcompositor
static void subsurface_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (!s || s->subsurface_resource != resource) return;

    debug_log("subsurface destroyed for surface handle=%u", s->client_handle);
    s->subsurface_resource = NULL;
    s->is_subsurface = 0;
    s->subsurface_parent = NULL;
    if (s->sprot_surface) {
        sprot_destroy_surface(s->sprot_surface);
        s->sprot_surface = NULL;
        s->sprot_id = 0;
    }
}

static void subsurface_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subsurface_set_position(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y) {
    (void)client;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        s->subsurface_x = x;
        s->subsurface_y = y;
        send_sprot_role_for_surface(s);
    }
    debug_log("subsurface_set_position: %d,%d", x, y);
}

static void subsurface_place_above(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling) {
    (void)client; (void)sibling;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        debug_log("subsurface_place_above: handle=%u", s->client_handle);
        send_sprot_role_for_surface(s);
    }
}

static void subsurface_place_below(struct wl_client *client, struct wl_resource *resource, struct wl_resource *sibling) {
    (void)client; (void)sibling;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        debug_log("subsurface_place_below: handle=%u", s->client_handle);
        send_sprot_role_for_surface(s);
    }
}

static void subsurface_set_sync(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static void subsurface_set_desync(struct wl_client *client, struct wl_resource *resource) {
    (void)client; (void)resource;
}

static const struct wl_subsurface_interface subsurface_implementation = {
    .destroy = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

static void subcompositor_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void subcompositor_get_subsurface(struct wl_client *client, struct wl_resource *resource,
                                         uint32_t id, struct wl_resource *surface,
                                         struct wl_resource *parent) {
    uint32_t version = wl_resource_get_version(resource);
    struct bridge_surface *s = wl_resource_get_user_data(surface);
    struct bridge_surface *parent_s = wl_resource_get_user_data(parent);
    if (s) {
        s->is_subsurface = 1;
        s->subsurface_parent = parent_s;
    }
    debug_log("subcompositor_get_subsurface: id=%u version=%u surface_handle=%u parent_handle=%u",
        id, version, s ? s->client_handle : 0, parent_s ? parent_s->client_handle : 0);
    struct wl_resource *subsurface = wl_resource_create(client, &wl_subsurface_interface, version, id);
    if (!subsurface) {
        wl_client_post_no_memory(client);
        return;
    }
    if (s) s->subsurface_resource = subsurface;
    wl_resource_set_implementation(subsurface, &subsurface_implementation, s, subsurface_resource_destroy);
}

static const struct wl_subcompositor_interface subcompositor_implementation = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("subcompositor_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_implementation, NULL, NULL);
}

// Viewporter stubs
static void viewport_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void viewport_set_source(struct wl_client *client, struct wl_resource *resource,
                                wl_fixed_t x, wl_fixed_t y, wl_fixed_t width, wl_fixed_t height) {
    (void)client; (void)resource;
    debug_log("viewport_set_source: %d,%d %dx%d", wl_fixed_to_int(x), wl_fixed_to_int(y),
              wl_fixed_to_int(width), wl_fixed_to_int(height));
}

static void viewport_set_destination(struct wl_client *client, struct wl_resource *resource,
                                     int32_t width, int32_t height) {
    (void)client; (void)resource;
    debug_log("viewport_set_destination: %dx%d", width, height);
}

static const struct wp_viewport_interface viewport_implementation = {
    .destroy = viewport_destroy,
    .set_source = viewport_set_source,
    .set_destination = viewport_set_destination,
};

static void viewporter_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void viewporter_get_viewport(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *surface) {
    (void)surface;
    uint32_t version = wl_resource_get_version(resource);
    debug_log("viewporter_get_viewport: id=%u version=%u", id, version);
    struct wl_resource *viewport = wl_resource_create(client, &wp_viewport_interface, version, id);
    if (!viewport) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(viewport, &viewport_implementation, NULL, NULL);
}

static const struct wp_viewporter_interface viewporter_implementation = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

static void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("viewporter_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wp_viewporter_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &viewporter_implementation, NULL, NULL);
}

// xdg-decoration stubs
#define WLBRIDGE_DECORATION_MODE ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE

static void toplevel_decoration_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void toplevel_decoration_set_mode(struct wl_client *client, struct wl_resource *resource, uint32_t mode) {
    (void)client;
    debug_log("toplevel_decoration_set_mode: mode=%u", mode);
    zxdg_toplevel_decoration_v1_send_configure(resource, WLBRIDGE_DECORATION_MODE);
}

static void toplevel_decoration_unset_mode(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    debug_log("toplevel_decoration_unset_mode");
    zxdg_toplevel_decoration_v1_send_configure(resource, WLBRIDGE_DECORATION_MODE);
}

static const struct zxdg_toplevel_decoration_v1_interface toplevel_decoration_implementation = {
    .destroy = toplevel_decoration_destroy,
    .set_mode = toplevel_decoration_set_mode,
    .unset_mode = toplevel_decoration_unset_mode,
};

static void decoration_manager_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void decoration_manager_get_toplevel_decoration(struct wl_client *client, struct wl_resource *resource,
                                                       uint32_t id, struct wl_resource *toplevel) {
    (void)toplevel;
    uint32_t version = wl_resource_get_version(resource);
    debug_log("decoration_manager_get_toplevel_decoration: id=%u version=%u", id, version);
    struct wl_resource *decoration = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, version, id);
    if (!decoration) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(decoration, &toplevel_decoration_implementation, NULL, NULL);
    zxdg_toplevel_decoration_v1_send_configure(decoration, WLBRIDGE_DECORATION_MODE);
}

static const struct zxdg_decoration_manager_v1_interface decoration_manager_implementation = {
    .destroy = decoration_manager_destroy,
    .get_toplevel_decoration = decoration_manager_get_toplevel_decoration,
};

static void decoration_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("decoration_manager_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &decoration_manager_implementation, NULL, NULL);
}

// Seat resources
static void seat_pointer_destroy(struct wl_resource *resource) {
    struct seat_resource *sr = wl_resource_get_user_data(resource);
    free_seat_resource(sr);
}

static void seat_keyboard_destroy(struct wl_resource *resource) {
    struct seat_resource *sr = wl_resource_get_user_data(resource);
    free_seat_resource(sr);
}

static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
                               uint32_t serial, struct wl_resource *surface,
                               int32_t hotspot_x, int32_t hotspot_y) {
    (void)client;
    struct seat_resource *sr = wl_resource_get_user_data(resource);
    struct bridge_client *c = sr ? sr->client : NULL;
    struct bridge_surface *s = surface ? wl_resource_get_user_data(surface) : NULL;
    if (s) {
        s->is_cursor = 1;
        s->cursor_hotspot_x = hotspot_x;
        s->cursor_hotspot_y = hotspot_y;
        if (s->buffer_resource) {
            send_cursor_image(c, s);
            release_attached_buffer(s);
            finish_frame_callbacks(s);
        }
    }
    debug_log("pointer_set_cursor: serial=%u surface_handle=%u hotspot=%d,%d",
        serial, s ? s->client_handle : 0, hotspot_x, hotspot_y);

    if (!surface && c && c->keyboard_focus && c->keyboard_focus->sprot_surface) {
        int res = sprot_set_cursor_image(c->keyboard_focus->sprot_surface, -1, 0, 0, 0, 0, 0, 0, 0);
        debug_log("pointer_set_cursor: hide cursor res=%d", res);
    }
}

static void pointer_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_implementation = {
    .set_cursor = pointer_set_cursor,
    .release = pointer_release,
};

static void keyboard_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
    .release = keyboard_release,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct seat_resource *seat = wl_resource_get_user_data(resource);
    struct bridge_client *c = seat ? seat->client : NULL;
    (void)client;
    debug_log("seat_get_pointer: id=%u", id);
    if (!c) return;

    struct seat_resource *sr = calloc(1, sizeof(*sr));
    if (!sr) {
        wl_client_post_no_memory(client);
        return;
    }
    sr->client = c;
    sr->resource = wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    if (!sr->resource) {
        free(sr);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(sr->resource, &pointer_implementation, sr, seat_pointer_destroy);
    wl_list_insert(&c->seat_pointers, &sr->link);
    debug_log("seat_get_pointer: done, pointer resource created");
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct seat_resource *seat = wl_resource_get_user_data(resource);
    struct bridge_client *c = seat ? seat->client : NULL;
    (void)client;
    debug_log("seat_get_keyboard: id=%u seat_version=%u", id, wl_resource_get_version(resource));
    if (!c) return;

    struct seat_resource *sr = calloc(1, sizeof(*sr));
    if (!sr) {
        wl_client_post_no_memory(client);
        return;
    }
    sr->client = c;
    sr->resource = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    if (!sr->resource) {
        free(sr);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(sr->resource, &keyboard_implementation, sr, seat_keyboard_destroy);
    wl_list_insert(&c->seat_keyboards, &sr->link);
    debug_log("seat_get_keyboard: keyboard resource created, version=%u", wl_resource_get_version(sr->resource));

    if (ensure_keymap() == 0) {
        int keymap_fd = dup(g_keymap_fd);
        if (keymap_fd >= 0) {
            debug_log("seat_get_keyboard: sending keymap fd=%d size=%u", keymap_fd, g_keymap_size);
            wl_keyboard_send_keymap(sr->resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap_fd, g_keymap_size);
            close(keymap_fd);
            debug_log("seat_get_keyboard: keymap sent");
        } else {
            debug_log("seat_get_keyboard: ERROR dup(g_keymap_fd) failed: %s", strerror(errno));
        }
    } else {
        debug_log("seat_get_keyboard: ERROR ensure_keymap() failed");
    }
    if (wl_resource_get_version(sr->resource) >= 4) {
        wl_keyboard_send_repeat_info(sr->resource, 25, 600);
        debug_log("seat_get_keyboard: repeat_info sent");
    }
    send_keyboard_modifiers(c, next_keyboard_serial(c));
    debug_log("seat_get_keyboard: modifiers sent. keyboard_focus=%s",
        c->keyboard_focus ? "set" : "null");

    if (c->keyboard_focus != NULL) {
        struct wl_array keys;
        wl_array_init(&keys);
        wl_keyboard_send_enter(sr->resource, next_keyboard_serial(c), c->keyboard_focus->resource, &keys);
        wl_array_release(&keys);
        debug_log("seat_get_keyboard: enter sent to keyboard_focus handle=%u", c->keyboard_focus->client_handle);
    }
    debug_log("seat_get_keyboard: done");
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)client; (void)resource;
    debug_log("seat_get_touch: id=%u (not implemented)", id);
}

static void seat_release(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_implementation = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void seat_resource_destroy(struct wl_resource *resource) {
    struct seat_resource *sr = wl_resource_get_user_data(resource);
    free_seat_resource(sr);
}

// DMA-BUF buffer implementation
static void dmabuf_buffer_destroy(struct wl_resource *resource) {
    struct dmabuf_buffer *buf = wl_resource_get_user_data(resource);
    if (!buf) return;

    wl_resource_set_user_data(resource, NULL);
    if (buf->fd >= 0) {
        close(buf->fd);
    }
    free(buf);
}

static void dmabuf_buffer_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface dmabuf_buffer_implementation = {
    .destroy = dmabuf_buffer_destroy_request,
};

// DMA-BUF params implementation
static void params_destroy_handler(struct wl_resource *resource) {
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params) return;

    for (uint32_t i = 0; i < 4; i++) {
        if (params->fds[i] >= 0) {
            close(params->fds[i]);
        }
    }
    free(params);
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t fd, uint32_t plane_idx, uint32_t offset,
                       uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "params already used");
        close(fd);
        return;
    }

    if (plane_idx >= 4) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                              "plane index too large");
        close(fd);
        return;
    }

    if (params->plane_set[plane_idx]) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                              "plane already set");
        close(fd);
        return;
    }

    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;

    if (params->num_planes > 0 && params->modifier != modifier) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "all planes must have same modifier");
        close(fd);
        return;
    }

    params->fds[plane_idx] = fd;
    params->offsets[plane_idx] = offset;
    params->strides[plane_idx] = stride;
    params->modifier = modifier;
    params->plane_set[plane_idx] = 1;
    params->num_planes++;

    debug_log("params_add: plane_idx=%u fd=%d offset=%u stride=%u modifier=0x%lx",
              plane_idx, fd, offset, stride, (unsigned long)modifier);
}

static struct wl_resource* params_create_buffer(struct dmabuf_params *params,
                                                int32_t width, int32_t height,
                                                uint32_t format, uint32_t flags,
                                                uint32_t buffer_id) {
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                              "invalid dimensions");
        return NULL;
    }

    if (params->num_planes == 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                              "no planes added");
        return NULL;
    }

    if (params->num_planes != 1) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "only single-plane buffers supported");
        return NULL;
    }

    if (format != 0x34325241 && format != 0x34325258) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "unsupported format (only ARGB8888/XRGB8888)");
        return NULL;
    }

    if (params->modifier != 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "only LINEAR modifier supported");
        return NULL;
    }

    if (params->offsets[0] != 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
                              "offset must be 0");
        return NULL;
    }

    struct dmabuf_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        return NULL;
    }

    buf->is_dmabuf = 1;
    buf->fd = params->fds[0];
    buf->width = width;
    buf->height = height;
    buf->format = format;
    buf->modifier = params->modifier;
    buf->num_planes = 1;
    buf->offsets[0] = params->offsets[0];
    buf->strides[0] = params->strides[0];
    buf->flags = flags;

    params->fds[0] = -1;

    if (buffer_id == 0) {
        buf->resource = wl_resource_create(wl_resource_get_client(params->resource),
                                           &wl_buffer_interface, 1, 0);
    } else {
        buf->resource = wl_resource_create(wl_resource_get_client(params->resource),
                                           &wl_buffer_interface, 1, buffer_id);
    }

    if (!buf->resource) {
        close(buf->fd);
        free(buf);
        return NULL;
    }

    wl_resource_set_implementation(buf->resource, &dmabuf_buffer_implementation, buf, dmabuf_buffer_destroy);

    debug_log("Created dmabuf buffer: %dx%d format=0x%x modifier=0x%lx stride=%u",
              width, height, format, (unsigned long)params->modifier, params->strides[0]);

    return buf->resource;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
                         int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "params already used");
        return;
    }
    params->used = 1;

    struct wl_resource *buffer = params_create_buffer(params, width, height, format, flags, 0);

    if (buffer) {
        zwp_linux_buffer_params_v1_send_created(resource, buffer);
    } else {
        zwp_linux_buffer_params_v1_send_failed(resource);
    }
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
                               uint32_t buffer_id, int32_t width, int32_t height,
                               uint32_t format, uint32_t flags) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "params already used");
        return;
    }
    params->used = 1;

    struct wl_resource *buffer = params_create_buffer(params, width, height, format, flags, buffer_id);

    if (!buffer) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                              "failed to create buffer");
        return;
    }
}

static const struct zwp_linux_buffer_params_v1_interface params_implementation = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

// DMA-BUF feedback implementation
static void feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void feedback_destroy_handler(struct wl_resource *resource) {
    struct dmabuf_feedback *feedback = wl_resource_get_user_data(resource);
    free(feedback);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_implementation = {
    .destroy = feedback_destroy,
};

static int ensure_render_node_info(struct bridge_client *c) {
    if (g_render_node_info.queried) {
        return g_render_node_info.has_drm;
    }

    g_render_node_info.queried = 1;
    if (!c || !c->sprot_conn) {
        return 0;
    }

    sprot_body_render_node_t info;
    if (sprot_query_render_node(c->sprot_conn, &info, 1000) != 0) {
        debug_log("ensure_render_node_info: query failed: %s", sprot_last_error());
        return 0;
    }

    if (!info.has_drm || info.render_node_path[0] == '\0') {
        debug_log("ensure_render_node_info: SWM has no render node");
        return 0;
    }

    strncpy(g_render_node_info.render_node_path, info.render_node_path,
            sizeof(g_render_node_info.render_node_path) - 1);
    g_render_node_info.render_node_path[sizeof(g_render_node_info.render_node_path) - 1] = '\0';
    g_render_node_info.render_major = info.render_major;
    g_render_node_info.render_minor = info.render_minor;
    g_render_node_info.has_drm = 1;
    debug_log("ensure_render_node_info: render node %s dev=%u:%u",
              g_render_node_info.render_node_path,
              g_render_node_info.render_major, g_render_node_info.render_minor);
    return 1;
}

static void send_dmabuf_feedback(struct bridge_client *c, struct wl_resource *feedback_resource) {
    ensure_render_node_info(c);

    if (!g_render_node_info.queried || !g_render_node_info.has_drm) {
        debug_log("send_dmabuf_feedback: render node not available, skipping");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    struct {
        uint32_t format;
        uint32_t padding;
        uint64_t modifier;
    } formats[] = {
        {0x34325241, 0, 0},
        {0x34325258, 0, 0},
    };

    int memfd = memfd_create("dmabuf-feedback", MFD_CLOEXEC);
    if (memfd < 0) {
        debug_log("send_dmabuf_feedback: memfd_create failed");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    size_t table_size = sizeof(formats);
    if (ftruncate(memfd, table_size) != 0) {
        close(memfd);
        debug_log("send_dmabuf_feedback: ftruncate failed");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    void *map = mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (map == MAP_FAILED) {
        close(memfd);
        debug_log("send_dmabuf_feedback: mmap failed");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    memcpy(map, formats, table_size);
    munmap(map, table_size);

    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback_resource, memfd, table_size);
    close(memfd);

    dev_t dev_id = makedev(g_render_node_info.render_major, g_render_node_info.render_minor);

    struct wl_array dev_array;
    wl_array_init(&dev_array);
    void *dev_data = wl_array_add(&dev_array, sizeof(dev_id));
    memcpy(dev_data, &dev_id, sizeof(dev_id));
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback_resource, &dev_array);

    struct wl_array indices;
    wl_array_init(&indices);
    uint16_t *idx0 = wl_array_add(&indices, sizeof(uint16_t));
    uint16_t *idx1 = wl_array_add(&indices, sizeof(uint16_t));
    *idx0 = 0;
    *idx1 = 1;

    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback_resource, &dev_array);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback_resource, &indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback_resource, ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback_resource);

    wl_array_release(&indices);
    wl_array_release(&dev_array);

    zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
    debug_log("send_dmabuf_feedback: sent feedback with 2 formats");
}

// DMA-BUF global implementation
static void linux_dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void linux_dmabuf_create_params(struct wl_client *client,
                                       struct wl_resource *resource, uint32_t params_id) {
    struct bridge_client *bridge_client = get_or_create_client(client);
    if (!bridge_client) {
        debug_log("linux_dmabuf_create_params: failed to get client");
        return;
    }

    struct dmabuf_params *params = calloc(1, sizeof(*params));
    if (!params) {
        wl_client_post_no_memory(client);
        return;
    }

    params->client = bridge_client;
    for (int i = 0; i < 4; i++) {
        params->fds[i] = -1;
    }

    params->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                         wl_resource_get_version(resource), params_id);
    wl_resource_set_implementation(params->resource, &params_implementation,
                                  params, params_destroy_handler);

    debug_log("linux_dmabuf_create_params: created params object");
}

static void linux_dmabuf_get_default_feedback(struct wl_client *client,
                                              struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct bridge_client *bridge_client = get_or_create_client(client);
    struct dmabuf_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    wl_resource_set_implementation(feedback->resource, &feedback_implementation,
                                  feedback, feedback_destroy_handler);

    send_dmabuf_feedback(bridge_client, feedback->resource);
    debug_log("linux_dmabuf_get_default_feedback: sent default feedback");
}

static void linux_dmabuf_get_surface_feedback(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t id, struct wl_resource *surface) {
    (void)resource;
    (void)surface;
    struct bridge_client *bridge_client = get_or_create_client(client);
    struct dmabuf_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    feedback->surface_resource = surface;
    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    wl_resource_set_implementation(feedback->resource, &feedback_implementation,
                                  feedback, feedback_destroy_handler);

    send_dmabuf_feedback(bridge_client, feedback->resource);
    debug_log("linux_dmabuf_get_surface_feedback: sent surface feedback");
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_implementation = {
    .destroy = linux_dmabuf_destroy,
    .create_params = linux_dmabuf_create_params,
    .get_default_feedback = linux_dmabuf_get_default_feedback,
    .get_surface_feedback = linux_dmabuf_get_surface_feedback,
};

static void linux_dmabuf_bind(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_linux_dmabuf_v1_interface, version, id);
    wl_resource_set_implementation(resource, &linux_dmabuf_implementation, NULL, NULL);

    if (version < 4) {
        zwp_linux_dmabuf_v1_send_format(resource, 0x34325241);
        zwp_linux_dmabuf_v1_send_format(resource, 0x34325258);

        if (version >= 3) {
            zwp_linux_dmabuf_v1_send_modifier(resource, 0x34325241, 0, 0);
            zwp_linux_dmabuf_v1_send_modifier(resource, 0x34325258, 0, 0);
        }
    }

    debug_log("linux_dmabuf_bind: client bound to dmabuf interface version %u", version);
}

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("seat_bind: version=%u id=%u pid=%d", version, id, (int)pid);
    struct bridge_client *c = get_or_create_client(client);
    if (!c) {
        debug_log("seat_bind: ERROR get_or_create_client returned NULL");
        return;
    }

    struct seat_resource *sr = calloc(1, sizeof(*sr));
    if (!sr) {
        wl_client_post_no_memory(client);
        return;
    }
    sr->client = c;
    sr->resource = wl_resource_create(client, &wl_seat_interface, version, id);
    if (!sr->resource) {
        free(sr);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(sr->resource, &seat_implementation, sr, seat_resource_destroy);
    wl_list_insert(&c->seat_resources, &sr->link);

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    debug_log("seat_bind: sending capabilities=0x%x (POINTER|KEYBOARD)", caps);
    wl_seat_send_capabilities(sr->resource, caps);
    if (version >= 2) {
        wl_seat_send_name(sr->resource, "seat0");
        debug_log("seat_bind: sent seat name 'seat0'");
    }
    debug_log("seat_bind: done");
}

// Output implementation
static void output_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_implementation = {
    .release = output_release,
};

static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("output_bind: version=%u id=%u pid=%d", version, id, (int)pid);
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_implementation, NULL, NULL);

    wl_output_send_geometry(resource, 0, 0, 1920, 1080, 0, "swm", "wayland_bridge", WL_OUTPUT_TRANSFORM_NORMAL);
    if (version >= 2) {
        wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1920, 1080, 60000);
        wl_output_send_scale(resource, 1);
        wl_output_send_done(resource);
    }
    debug_log("output_bind: done");
}

int wayland_server_run(const char *display_name, const char *swm_socket, const char *debug_file) {
    if (debug_file) {
        const char *mode = getenv("WLBRIDGE_DEBUG_APPEND") ? "a" : "w";
        g_debug_file = fopen(debug_file, mode);
        if (g_debug_file) {
            setvbuf(g_debug_file, NULL, _IONBF, 0);
        } else {
            fprintf(stderr, "[wlbridge] Failed to open debug file %s: %s\n", debug_file, strerror(errno));
        }
    }

    debug_log("wlbridge server starting. display: %s, swm_socket: %s", display_name, swm_socket);

    g_server.display = wl_display_create();
    if (!g_server.display) {
        fprintf(stderr, "[wlbridge] Failed to create Wayland display: %s\n", strerror(errno));
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }

    g_server.loop = wl_display_get_event_loop(g_server.display);
    g_server.swm_socket = swm_socket;
    wl_list_init(&g_server.clients);
    g_server.next_client_handle = 0;

    /* log every new client connection */
    static struct wl_listener client_created_listener;
    client_created_listener.notify = on_client_created;
    wl_display_add_client_created_listener(g_server.display, &client_created_listener);

    /* log every protocol message */
    wl_display_add_protocol_logger(g_server.display, on_protocol_log, NULL);

    if (!wl_global_create(g_server.display, &wl_compositor_interface, 4, NULL, compositor_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &wl_subcompositor_interface, 1, NULL, subcompositor_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (wl_display_init_shm(g_server.display) != 0) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &wl_data_device_manager_interface, 3, NULL, data_device_manager_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &wp_viewporter_interface, 1, NULL, viewporter_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &xdg_wm_base_interface, 7, NULL, xdg_wm_base_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &zxdg_decoration_manager_v1_interface, 1, NULL, decoration_manager_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &wl_seat_interface, 5, NULL, seat_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &wl_output_interface, 3, NULL, output_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &zwp_linux_dmabuf_v1_interface, 4, NULL, linux_dmabuf_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    debug_log("Registered zwp_linux_dmabuf_v1 global (version 4)");

    int socket_added = wl_display_add_socket(g_server.display, display_name);
    if (socket_added < 0) {
        fprintf(stderr, "[wlbridge] Failed to add Wayland socket %s: %s\n", display_name, strerror(errno));
        debug_log("Failed to add Wayland socket: %s", strerror(errno));
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }

    printf("[wlbridge] Wayland compositor proxy listening on socket: %s\n", display_name);
    debug_log("Wayland proxy running.");
    wl_display_run(g_server.display);

    debug_log("wlbridge server shutting down.");
    wl_display_destroy(g_server.display);
    if (g_keymap_fd >= 0) {
        close(g_keymap_fd);
        g_keymap_fd = -1;
    }
    if (g_debug_file) {
        fclose(g_debug_file);
        g_debug_file = NULL;
    }
    return 0;
}
