#define _GNU_SOURCE
#include "wlbridge_internal.h"

void destroy_bridge_surface(struct bridge_surface *s) {
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

static void surface_add_damage(struct bridge_surface *s, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (!s || width <= 0 || height <= 0) return;

    int64_t x2 = (int64_t)x + width;
    int64_t y2 = (int64_t)y + height;
    if (x2 <= x || y2 <= y) return;
    if (x2 > INT32_MAX) x2 = INT32_MAX;
    if (y2 > INT32_MAX) y2 = INT32_MAX;

    if (!s->damage_set) {
        s->damage_x1 = x;
        s->damage_y1 = y;
        s->damage_x2 = (int32_t)x2;
        s->damage_y2 = (int32_t)y2;
        s->damage_set = 1;
        return;
    }

    if (x < s->damage_x1) s->damage_x1 = x;
    if (y < s->damage_y1) s->damage_y1 = y;
    if (x2 > s->damage_x2) s->damage_x2 = (int32_t)x2;
    if (y2 > s->damage_y2) s->damage_y2 = (int32_t)y2;
}

static void request_surface_frame_if_needed(struct bridge_surface *s) {
    if (!s || !s->sprot_surface || s->sprot_id == 0) return;
    if (s->frame_request_pending || wl_list_empty(&s->frame_callbacks)) return;
    if (sprot_request_frame(s->sprot_surface) == 0) {
        s->frame_request_pending = 1;
    }
}

static void copy_shm_damage(struct bridge_surface *s, void *dst, const void *src,
                            int32_t width, int32_t height, int32_t stride, int full_copy) {
    if (!dst || !src || width <= 0 || height <= 0 || stride <= 0) return;
    if ((uint64_t)stride < (uint64_t)width * 4u) return;

    if (full_copy || !s->damage_set) {
        memcpy(dst, src, (size_t)stride * height);
        return;
    }

    int32_t x1 = s->damage_x1 < 0 ? 0 : s->damage_x1;
    int32_t y1 = s->damage_y1 < 0 ? 0 : s->damage_y1;
    int32_t x2 = s->damage_x2 > width ? width : s->damage_x2;
    int32_t y2 = s->damage_y2 > height ? height : s->damage_y2;
    if (x1 >= x2 || y1 >= y2) return;

    size_t row_bytes = (size_t)(x2 - x1) * 4u;
    const char *src_bytes = src;
    char *dst_bytes = dst;
    for (int32_t row = y1; row < y2; row++) {
        size_t offset = (size_t)row * stride + (size_t)x1 * 4u;
        memcpy(dst_bytes + offset, src_bytes + offset, row_bytes);
    }
}

void flush_pending_surface_commit(struct bridge_surface *s) {
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
            if (commit_res == 0) request_surface_frame_if_needed(s);
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
            if (commit_res == 0) request_surface_frame_if_needed(s);
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

void flush_children_waiting_for_parent(struct bridge_surface *parent) {
    if (!parent || !parent->client || parent->sprot_id == 0) return;
    struct bridge_surface *child;
    wl_list_for_each(child, &parent->client->surfaces, link) {
        if ((child->is_popup && child->popup_parent == parent) ||
            (child->is_subsurface && child->subsurface_parent == parent)) {
            flush_pending_surface_commit(child);
        }
    }
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
    (void)client;
    surface_add_damage(wl_resource_get_user_data(resource), x, y, width, height);
}

void send_cursor_image(struct bridge_client *c, struct bridge_surface *cursor_surface) {
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

void finish_frame_callbacks(struct bridge_surface *s) {
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
        wl_callback_send_done(cb, 0);
        wl_resource_destroy(cb);
    }
    s->frame_request_pending = 0;
}

void release_attached_buffer(struct bridge_surface *s) {
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
        s->role_sent = 0;
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
    if (commit_res == 0) request_surface_frame_if_needed(s);
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

        if (width <= 0 || height <= 0) return;
        if (stride <= 0 || (uint64_t)stride < (uint64_t)width * 4u) return;
        size_t size = (size_t)stride * height;

        debug_log("Surface commit for surface handle=%u. Attached SHM buffer size: %dx%d, stride: %d, size: %zu", s->client_handle, width, height, stride, size);

        if (!s->sprot_surface) {
            debug_log("Creating new Sprot surface for client connection...");
            s->sprot_surface = sprot_create_surface(s->client->sprot_conn, width, height);
            if (!s->sprot_surface) {
                debug_log("Error: Failed to create Sprot surface.");
                return;
            }
            s->role_sent = 0;
        }

        int full_copy = !s->damage_set;
        if (s->memfd < 0 || s->memfd_size < size) {
            full_copy = 1;
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
            copy_shm_damage(s, s->memfd_map, src_pixels, width, height, stride, full_copy);
        }
        wl_shm_buffer_end_access(shm_buf);
        s->damage_set = 0;

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
                if (commit_res == 0) request_surface_frame_if_needed(s);
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
    (void)client;
    surface_add_damage(wl_resource_get_user_data(resource), x, y, width, height);
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

void compositor_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("compositor_bind: version=%u id=%u pid=%d", version, id, (int)pid);
    struct wl_resource *resource = wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_implementation(resource, &compositor_implementation, NULL, NULL);
}
