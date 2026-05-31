#define _GNU_SOURCE
#include "wlbridge_internal.h"

#include <stdarg.h>
#include <time.h>

struct bridge_server g_server;
struct wlbridge_render_node_info g_render_node_info = {0};
FILE *g_debug_file = NULL;
int g_keymap_fd = -1;
uint32_t g_keymap_size = 0;
const char g_default_keymap[] =
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

void wlbridge_debug_log(const char *fmt, ...) {
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

uint32_t wlbridge_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

uint32_t wayland_button_from_srapi(uint32_t button) {
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

uint32_t next_keyboard_serial(struct bridge_client *c) {
    c->keyboard_serial++;
    if (c->keyboard_serial == 0) {
        c->keyboard_serial = 1;
    }
    return c->keyboard_serial;
}

struct bridge_surface *find_surface_by_sprot_id(struct bridge_client *c, uint32_t sprot_id) {
    struct bridge_surface *s;
    wl_list_for_each(s, &c->surfaces, link) {
        if (s->sprot_id == sprot_id ||
            (s->sprot_surface && sprot_surface_id(s->sprot_surface) == sprot_id)) {
            return s;
        }
    }
    return NULL;
}

void free_seat_resource(struct seat_resource *sr) {
    if (!sr) return;
    if (sr->resource) {
        wl_resource_set_user_data(sr->resource, NULL);
    }
    wl_list_remove(&sr->link);
    free(sr);
}

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
                    s->frame_request_pending = 0;
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
struct bridge_client *get_or_create_client(struct wl_client *wl_client) {
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

    if (g_debug_file) {
        static struct wl_listener client_created_listener;
        client_created_listener.notify = on_client_created;
        wl_display_add_client_created_listener(g_server.display, &client_created_listener);
        wl_display_add_protocol_logger(g_server.display, on_protocol_log, NULL);
    }

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
