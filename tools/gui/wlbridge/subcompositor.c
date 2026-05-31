#define _GNU_SOURCE
#include "wlbridge_internal.h"

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
        s->role_sent = 0;
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

void subcompositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("subcompositor_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wl_subcompositor_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &subcompositor_implementation, NULL, NULL);
}
