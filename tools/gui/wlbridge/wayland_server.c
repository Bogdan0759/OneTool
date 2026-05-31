#define _GNU_SOURCE
#include "wayland_server.h"
#include "xdg-shell-protocol.h"
#include <sprot/client.h>

#include <wayland-server.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
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

static struct bridge_server g_server;
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

static uint32_t next_keyboard_serial(struct bridge_client *c) {
    c->keyboard_serial++;
    if (c->keyboard_serial == 0) {
        c->keyboard_serial = 1;
    }
    return c->keyboard_serial;
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
        if (i == c->pressed_key_count && c->pressed_key_count < MAX_SURFACES) {
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

static struct bridge_surface *find_surface_by_handle(struct bridge_client *c, uint32_t handle) {
    struct bridge_surface *s;
    wl_list_for_each(s, &c->surfaces, link) {
        if (s->client_handle == handle) {
            return s;
        }
    }
    return NULL;
}

static struct bridge_surface *find_surface_by_sprot_id(struct bridge_client *c, uint32_t sprot_id) {
    struct bridge_surface *s;
    wl_list_for_each(s, &c->surfaces, link) {
        if (s->sprot_id == sprot_id) {
            return s;
        }
    }
    return NULL;
}

static void destroy_bridge_surface(struct bridge_surface *s) {
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
                struct bridge_surface *s = find_surface_by_handle(c, ev.u.surface_created.client_handle);
                if (s) {
                    s->sprot_id = ev.u.surface_created.surface_id;
                    debug_log("Sprot event: Surface created. Sprot ID=%u assigned to handle=%u", s->sprot_id, s->client_handle);
                    // Explicitly set the Sprot surface role to TOPLEVEL so that SWM maps the window
                    int role_res = sprot_set_role(s->sprot_surface, SPROT_SURFACE_ROLE_TOPLEVEL, 0, 0, 0);
                    debug_log("Set Sprot role to SPROT_SURFACE_ROLE_TOPLEVEL for Sprot ID=%u (res=%d)", s->sprot_id, role_res);

                    if (s->pending_commit) {
                        debug_log("Executing deferred surface commit for surface Sprot ID=%u", s->sprot_id);
                        int dup_fd = dup(s->memfd);
                        if (dup_fd >= 0) {
                            int attach_res = sprot_attach_fd(s->sprot_surface, dup_fd, s->pending_width, s->pending_height, s->pending_stride, (uint32_t)s->pending_size, SPROT_BUFFER_SHM, SPROT_PIXEL_FORMAT_BGRA8888);
                            int commit_res = sprot_commit(s->sprot_surface);
                            sprot_request_frame(s->sprot_surface);
                            debug_log("Deferred commit results: attach=%d, commit=%d", attach_res, commit_res);
                        } else {
                            debug_log("Error: deferred dup failed: %s", strerror(errno));
                        }
                        s->pending_commit = 0;
                    }
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
                        // Forward top-level configure
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
                if (s && s->xdg_toplevel_resource) {
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
                wl_list_for_each(sr, &c->seat_pointers, link) {
                    wl_pointer_send_motion(sr->resource, 0,
                        wl_fixed_from_int(ev.u.pointer_motion.x),
                        wl_fixed_from_int(ev.u.pointer_motion.y));
                }
                break;
            }

            case SPROT_EVENT_POINTER_BUTTON: {
                struct seat_resource *sr;
                debug_log("Sprot event: Pointer button button=%u state=%u", ev.u.pointer_button.button, ev.u.pointer_button.state);
                wl_list_for_each(sr, &c->seat_pointers, link) {
                    // Translate button state: pressed = 1, released = 0
                    uint32_t state = ev.u.pointer_button.state ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
                    wl_pointer_send_button(sr->resource, ev.serial, 0, ev.u.pointer_button.button, state);
                }
                break;
            }

            case SPROT_EVENT_KEY: {
                debug_log("Sprot event: Keyboard key scancode=%u state=%u", ev.u.key.scancode, ev.u.key.state);
                if (c->keyboard_focus != NULL) {
                    uint32_t serial = ev.serial ? ev.serial : next_keyboard_serial(c);
                    uint32_t state = ev.u.key.state ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
                    update_pressed_keys(c, ev.u.key.scancode, ev.u.key.state);

                    struct seat_resource *sr;
                    wl_list_for_each(sr, &c->seat_keyboards, link) {
                        wl_keyboard_send_key(sr->resource, serial, 0, ev.u.key.scancode, state);
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
    if (c->sprot_conn) {
        sprot_disconnect(c->sprot_conn);
    }

    struct bridge_surface *s, *stmp;
    wl_list_for_each_safe(s, stmp, &c->surfaces, link) {
        destroy_bridge_surface(s);
    }

    struct seat_resource *sr, *srtmp;
    wl_list_for_each_safe(sr, srtmp, &c->seat_pointers, link) {
        wl_list_remove(&sr->link);
        free(sr);
    }
    wl_list_for_each_safe(sr, srtmp, &c->seat_keyboards, link) {
        wl_list_remove(&sr->link);
        free(sr);
    }
    wl_list_for_each_safe(sr, srtmp, &c->seat_resources, link) {
        wl_list_remove(&sr->link);
        free(sr);
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

    debug_log("New Wayland client connected. Initializing Sprot connection...");

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

static void surface_set_opaque_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void surface_set_input_region(struct wl_client *client, struct wl_resource *resource, struct wl_resource *region) {
    (void)client; (void)resource; (void)region;
}

static void surface_commit(struct wl_client *client, struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    (void)client;

    debug_log("surface_commit: handle=%u buffer=%s configured=%d sprot_id=%u",
        s->client_handle, s->buffer_resource ? "set" : "null", s->configured, s->sprot_id);

    if (!s->buffer_resource) {
        if (!s->configured) {
            debug_log("surface_commit: initial commit (no buffer) for handle=%u. Sending XDG configure.", s->client_handle);
            if (s->xdg_toplevel_resource) {
                struct wl_array states;
                wl_array_init(&states);
                xdg_toplevel_send_configure(s->xdg_toplevel_resource, 0, 0, &states);
                wl_array_release(&states);
            }
            if (s->xdg_surface_resource) {
                xdg_surface_send_configure(s->xdg_surface_resource, 1);
            }
            s->configured = 1;
        } else {
            debug_log("surface_commit: no buffer, already configured. Skipping.");
        }
        return;
    }

    struct wl_shm_buffer *shm_buf = wl_shm_buffer_get(s->buffer_resource);
    if (!shm_buf) {
        debug_log("surface_commit: WARNING handle=%u buffer is not SHM (maybe EGL/dmabuf?)", s->client_handle);
        return;
    }

    int32_t width = wl_shm_buffer_get_width(shm_buf);
    int32_t height = wl_shm_buffer_get_height(shm_buf);
    int32_t stride = wl_shm_buffer_get_stride(shm_buf);
    size_t size = (size_t)stride * height;

    if (width <= 0 || height <= 0) return;

    debug_log("Surface commit for surface handle=%u. Attached buffer size: %dx%d, stride: %d, size: %zu", s->client_handle, width, height, stride, size);

    if (!s->sprot_surface) {
        debug_log("Creating new Sprot surface for client connection...");
        s->sprot_surface = sprot_create_surface(s->client->sprot_conn, width, height);
        if (!s->sprot_surface) {
            debug_log("Error: Failed to create Sprot surface.");
            return;
        }
    }

    // Allocate/resize memfd
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

    // Copy buffer contents
    wl_shm_buffer_begin_access(shm_buf);
    void *src_pixels = wl_shm_buffer_get_data(shm_buf);
    if (src_pixels && s->memfd_map) {
        memcpy(s->memfd_map, src_pixels, size);
    }
    wl_shm_buffer_end_access(shm_buf);

    if (s->sprot_id == 0) {
        debug_log("Sprot surface ID is not yet ready. Deferring attach and commit.");
        s->pending_commit = 1;
        s->pending_width = width;
        s->pending_height = height;
        s->pending_stride = stride;
        s->pending_size = size;
    } else {
        // Send attach and commit to SWM immediately
        int dup_fd = dup(s->memfd);
        if (dup_fd >= 0) {
            debug_log("Forwarding buffer attach to SWM (Sprot ID=%u)", s->sprot_id);
            int attach_res = sprot_attach_fd(s->sprot_surface, dup_fd, width, height, stride, (uint32_t)size, SPROT_BUFFER_SHM, SPROT_PIXEL_FORMAT_BGRA8888);
            int commit_res = sprot_commit(s->sprot_surface);
            sprot_request_frame(s->sprot_surface);
            debug_log("Commit results: attach=%d, commit=%d", attach_res, commit_res);
        } else {
            debug_log("Error: dup failed: %s", strerror(errno));
        }
    }

    wl_buffer_send_release(s->buffer_resource);
    s->buffer_resource = NULL;
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
    (void)client; (void)resource;
    debug_log("compositor_create_region: id=%u", id);
}

static const struct wl_compositor_interface compositor_implementation = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("compositor_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &compositor_implementation, NULL, NULL);
}

// XDG WM Base (xdg_wm_base) implementation
static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    (void)client; (void)resource;
    debug_log("xdg_wm_base_create_positioner: id=%u", id);
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

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    debug_log("Wayland request: xdg_surface.get_toplevel for surface handle=%u", s->client_handle);

    s->xdg_toplevel_resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(s->xdg_toplevel_resource, &xdg_toplevel_implementation, s, NULL);
}

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) {
    (void)client; (void)resource; (void)parent; (void)positioner; (void)id;
    debug_log("Wayland request: xdg_surface.get_popup");
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client; (void)resource; (void)x; (void)y; (void)width; (void)height;
    debug_log("Wayland request: xdg_surface.set_window_geometry to %dx%d", width, height);
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
    debug_log("xdg_wm_base_bind: version=%u id=%u", version, id);
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

// Seat resources
static void seat_pointer_destroy(struct wl_resource *resource) {
    struct seat_resource *sr = wl_resource_get_user_data(resource);
    wl_list_remove(&sr->link);
    free(sr);
}

static void seat_keyboard_destroy(struct wl_resource *resource) {
    struct seat_resource *sr = wl_resource_get_user_data(resource);
    wl_list_remove(&sr->link);
    free(sr);
}

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_client *c = wl_resource_get_user_data(resource);
    (void)client;
    debug_log("seat_get_pointer: id=%u", id);

    struct seat_resource *sr = calloc(1, sizeof(*sr));
    sr->resource = wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(sr->resource, NULL, sr, seat_pointer_destroy);
    wl_list_insert(&c->seat_pointers, &sr->link);
    debug_log("seat_get_pointer: done, pointer resource created");
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_client *c = wl_resource_get_user_data(resource);
    (void)client;
    debug_log("seat_get_keyboard: id=%u seat_version=%u", id, wl_resource_get_version(resource));

    struct seat_resource *sr = calloc(1, sizeof(*sr));
    sr->resource = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(sr->resource, NULL, sr, seat_keyboard_destroy);
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
    wl_list_remove(&sr->link);
    free(sr);
}

static void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("seat_bind: version=%u id=%u", version, id);
    struct bridge_client *c = get_or_create_client(client);
    if (!c) {
        debug_log("seat_bind: ERROR get_or_create_client returned NULL");
        return;
    }

    struct seat_resource *sr = calloc(1, sizeof(*sr));
    sr->resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(sr->resource, &seat_implementation, c, seat_resource_destroy);
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
static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("output_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wl_output_interface, version, id);
    wl_resource_set_implementation(resource, NULL, NULL, NULL);

    wl_output_send_geometry(resource, 0, 0, 1920, 1080, 0, "swm", "wayland_bridge", WL_OUTPUT_TRANSFORM_NORMAL);
    if (version >= 2) {
        wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1920, 1080, 60000);
    }
    if (version >= 4) {
        wl_output_send_scale(resource, 1);
    }
    if (version >= 3) {
        wl_output_send_done(resource);
    }
    debug_log("output_bind: done");
}

int wayland_server_run(const char *display_name, const char *swm_socket, const char *debug_file) {
    if (debug_file) {
        g_debug_file = fopen(debug_file, "w");
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

    // Add standard globals
    if (!wl_global_create(g_server.display, &wl_compositor_interface, 4, NULL, compositor_bind)) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (wl_display_init_shm(g_server.display) != 0) {
        wl_display_destroy(g_server.display);
        if (g_debug_file) fclose(g_debug_file);
        return 1;
    }
    if (!wl_global_create(g_server.display, &xdg_wm_base_interface, 1, NULL, xdg_wm_base_bind)) {
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
