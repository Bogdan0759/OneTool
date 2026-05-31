#ifndef ONETOOL_TOOLS_GUI_WLBRIDGE_INTERNAL_H
#define ONETOOL_TOOLS_GUI_WLBRIDGE_INTERNAL_H

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
#include <stdint.h>
#include <stddef.h>
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
    int damage_set;
    int32_t damage_x1;
    int32_t damage_y1;
    int32_t damage_x2;
    int32_t damage_y2;
    int frame_request_pending;
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
    int role_sent;
    uint32_t last_role;
    uint32_t last_role_parent_id;
    int32_t last_role_x;
    int32_t last_role_y;
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

struct wlbridge_render_node_info {
    char render_node_path[256];
    uint32_t render_major;
    uint32_t render_minor;
    int has_drm;
    int queried;
};

extern struct bridge_server g_server;
extern struct wlbridge_render_node_info g_render_node_info;
extern FILE *g_debug_file;
extern int g_keymap_fd;
extern uint32_t g_keymap_size;
extern const char g_default_keymap[];

void wlbridge_debug_log(const char *fmt, ...);
#define debug_log(...) do { if (g_debug_file) wlbridge_debug_log(__VA_ARGS__); } while (0)
uint32_t wlbridge_now_ms(void);
uint32_t wayland_button_from_srapi(uint32_t button);
uint32_t next_keyboard_serial(struct bridge_client *c);

struct bridge_client *get_or_create_client(struct wl_client *wl_client);
struct bridge_surface *find_surface_by_sprot_id(struct bridge_client *c, uint32_t sprot_id);
void destroy_bridge_surface(struct bridge_surface *s);
void free_seat_resource(struct seat_resource *sr);

void flush_pending_surface_commit(struct bridge_surface *s);
void flush_children_waiting_for_parent(struct bridge_surface *parent);
void send_cursor_image(struct bridge_client *c, struct bridge_surface *cursor_surface);
void finish_frame_callbacks(struct bridge_surface *s);
void release_attached_buffer(struct bridge_surface *s);

int32_t popup_sprot_x(const struct bridge_surface *s);
int32_t popup_sprot_y(const struct bridge_surface *s);
int send_sprot_role_for_surface(struct bridge_surface *s);
void configure_popup_surface(struct bridge_surface *s, uint32_t token, int repositioned);

uint32_t srapi_scancode_to_evdev(uint32_t scancode);
void send_keyboard_modifiers(struct bridge_client *c, uint32_t serial);
void set_keyboard_focus(struct bridge_client *c, struct bridge_surface *surface, uint32_t serial);
void update_pressed_keys(struct bridge_client *c, uint32_t scancode, uint32_t pressed);

void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void data_device_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void xdg_wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void decoration_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
void linux_dmabuf_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id);

#endif
