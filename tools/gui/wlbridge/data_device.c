#define _GNU_SOURCE
#include "wlbridge_internal.h"

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
}

static const struct wl_data_device_manager_interface data_device_manager_implementation = {
    .create_data_source = data_device_manager_create_data_source,
    .get_data_device = data_device_manager_get_data_device,
};

void data_device_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("data_device_manager_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &wl_data_device_manager_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &data_device_manager_implementation, NULL, NULL);
}
