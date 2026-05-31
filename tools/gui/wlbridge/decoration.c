#define _GNU_SOURCE
#include "wlbridge_internal.h"

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

void decoration_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    debug_log("decoration_manager_bind: version=%u id=%u", version, id);
    struct wl_resource *resource = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &decoration_manager_implementation, NULL, NULL);
}
