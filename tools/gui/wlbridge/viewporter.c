#define _GNU_SOURCE
#include "wlbridge_internal.h"

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

void viewporter_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("viewporter_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wp_viewporter_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &viewporter_implementation, NULL, NULL);
}
