#define _GNU_SOURCE
#include "wlbridge_internal.h"

// Output implementation
static void output_release(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_output_interface output_implementation = {
    .release = output_release,
};

void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
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
