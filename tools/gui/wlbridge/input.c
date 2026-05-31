#define _GNU_SOURCE
#include "wlbridge_internal.h"

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

uint32_t srapi_scancode_to_evdev(uint32_t scancode) {
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

void send_keyboard_modifiers(struct bridge_client *c, uint32_t serial) {
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

void set_keyboard_focus(struct bridge_client *c, struct bridge_surface *surface, uint32_t serial) {
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

void update_pressed_keys(struct bridge_client *c, uint32_t scancode, uint32_t pressed) {
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

void seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
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
