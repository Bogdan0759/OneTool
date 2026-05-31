#define _GNU_SOURCE
#include "wlbridge_internal.h"

static const struct xdg_toplevel_interface xdg_toplevel_implementation;

static int32_t bridge_surface_window_x(const struct bridge_surface *s) {
    return s && s->window_geometry_set ? s->window_geometry_x : 0;
}

static int32_t bridge_surface_window_y(const struct bridge_surface *s) {
    return s && s->window_geometry_set ? s->window_geometry_y : 0;
}

static int32_t bridge_surface_window_width(const struct bridge_surface *s) {
    if (!s) return 0;
    if (s->window_geometry_set && s->window_geometry_width > 0) return s->window_geometry_width;
    return s->width;
}

static int32_t bridge_surface_window_height(const struct bridge_surface *s) {
    if (!s) return 0;
    if (s->window_geometry_set && s->window_geometry_height > 0) return s->window_geometry_height;
    return s->height;
}

static int positioner_anchor_left(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
}

static int positioner_anchor_right(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_RIGHT ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_RIGHT ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}

static int positioner_anchor_top(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_TOP ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_TOP_RIGHT;
}

static int positioner_anchor_bottom(uint32_t anchor) {
    return anchor == XDG_POSITIONER_ANCHOR_BOTTOM ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_LEFT ||
           anchor == XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
}

static int positioner_gravity_left(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
}

static int positioner_gravity_right(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_RIGHT ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_RIGHT ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
}

static int positioner_gravity_top(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_TOP ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_TOP_RIGHT;
}

static int positioner_gravity_bottom(uint32_t gravity) {
    return gravity == XDG_POSITIONER_GRAVITY_BOTTOM ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_LEFT ||
           gravity == XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
}

static uint32_t positioner_flip_anchor_x(uint32_t anchor) {
    switch (anchor) {
        case XDG_POSITIONER_ANCHOR_LEFT: return XDG_POSITIONER_ANCHOR_RIGHT;
        case XDG_POSITIONER_ANCHOR_RIGHT: return XDG_POSITIONER_ANCHOR_LEFT;
        case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
        default: return anchor;
    }
}

static uint32_t positioner_flip_anchor_y(uint32_t anchor) {
    switch (anchor) {
        case XDG_POSITIONER_ANCHOR_TOP: return XDG_POSITIONER_ANCHOR_BOTTOM;
        case XDG_POSITIONER_ANCHOR_BOTTOM: return XDG_POSITIONER_ANCHOR_TOP;
        case XDG_POSITIONER_ANCHOR_TOP_LEFT: return XDG_POSITIONER_ANCHOR_BOTTOM_LEFT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: return XDG_POSITIONER_ANCHOR_TOP_LEFT;
        case XDG_POSITIONER_ANCHOR_TOP_RIGHT: return XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT;
        case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: return XDG_POSITIONER_ANCHOR_TOP_RIGHT;
        default: return anchor;
    }
}

static uint32_t positioner_flip_gravity_x(uint32_t gravity) {
    switch (gravity) {
        case XDG_POSITIONER_GRAVITY_LEFT: return XDG_POSITIONER_GRAVITY_RIGHT;
        case XDG_POSITIONER_GRAVITY_RIGHT: return XDG_POSITIONER_GRAVITY_LEFT;
        case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
        case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
        default: return gravity;
    }
}

static uint32_t positioner_flip_gravity_y(uint32_t gravity) {
    switch (gravity) {
        case XDG_POSITIONER_GRAVITY_TOP: return XDG_POSITIONER_GRAVITY_BOTTOM;
        case XDG_POSITIONER_GRAVITY_BOTTOM: return XDG_POSITIONER_GRAVITY_TOP;
        case XDG_POSITIONER_GRAVITY_TOP_LEFT: return XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: return XDG_POSITIONER_GRAVITY_TOP_LEFT;
        case XDG_POSITIONER_GRAVITY_TOP_RIGHT: return XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
        case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: return XDG_POSITIONER_GRAVITY_TOP_RIGHT;
        default: return gravity;
    }
}

static void positioner_calc_xy(const struct xdg_positioner_data *pos,
                               uint32_t anchor, uint32_t gravity,
                               int32_t width, int32_t height,
                               int32_t *out_x, int32_t *out_y) {
    int32_t ax = pos->anchor_rect_x;
    int32_t ay = pos->anchor_rect_y;

    if (positioner_anchor_right(anchor)) ax += pos->anchor_rect_width;
    else if (!positioner_anchor_left(anchor)) ax += pos->anchor_rect_width / 2;

    if (positioner_anchor_bottom(anchor)) ay += pos->anchor_rect_height;
    else if (!positioner_anchor_top(anchor)) ay += pos->anchor_rect_height / 2;

    if (positioner_gravity_left(gravity)) *out_x = ax - width;
    else if (positioner_gravity_right(gravity)) *out_x = ax;
    else *out_x = ax - width / 2;

    if (positioner_gravity_top(gravity)) *out_y = ay - height;
    else if (positioner_gravity_bottom(gravity)) *out_y = ay;
    else *out_y = ay - height / 2;

    *out_x += pos->offset_x;
    *out_y += pos->offset_y;
}

static int positioner_axis_fits(int32_t value, int32_t size, int32_t bounds) {
    return bounds <= 0 || (value >= 0 && value + size <= bounds);
}

static void positioner_apply_constraints(const struct xdg_positioner_data *pos,
                                         const struct bridge_surface *parent,
                                         int32_t *x, int32_t *y,
                                         int32_t *width, int32_t *height) {
    int32_t bounds_w = pos->has_parent_size ? pos->parent_width : bridge_surface_window_width(parent);
    int32_t bounds_h = pos->has_parent_size ? pos->parent_height : bridge_surface_window_height(parent);

    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;

    if (bounds_w > 0 && !positioner_axis_fits(*x, *width, bounds_w)) {
        if (pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X) {
            int32_t fx, fy;
            positioner_calc_xy(pos, positioner_flip_anchor_x(pos->anchor),
                               positioner_flip_gravity_x(pos->gravity),
                               *width, *height, &fx, &fy);
            if (positioner_axis_fits(fx, *width, bounds_w)) *x = fx;
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X) &&
            !positioner_axis_fits(*x, *width, bounds_w)) {
            if (*width <= bounds_w) {
                if (*x < 0) *x = 0;
                if (*x + *width > bounds_w) *x = bounds_w - *width;
            }
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X) &&
            !positioner_axis_fits(*x, *width, bounds_w)) {
            if (*x < 0) *x = 0;
            if (*x >= bounds_w) *x = bounds_w - 1;
            if (*x + *width > bounds_w) *width = bounds_w - *x;
            if (*width < 1) *width = 1;
        }
    }

    if (bounds_h > 0 && !positioner_axis_fits(*y, *height, bounds_h)) {
        if (pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y) {
            int32_t fx, fy;
            positioner_calc_xy(pos, positioner_flip_anchor_y(pos->anchor),
                               positioner_flip_gravity_y(pos->gravity),
                               *width, *height, &fx, &fy);
            if (positioner_axis_fits(fy, *height, bounds_h)) *y = fy;
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y) &&
            !positioner_axis_fits(*y, *height, bounds_h)) {
            if (*height <= bounds_h) {
                if (*y < 0) *y = 0;
                if (*y + *height > bounds_h) *y = bounds_h - *height;
            }
        }
        if ((pos->constraint_adjustment & XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y) &&
            !positioner_axis_fits(*y, *height, bounds_h)) {
            if (*y < 0) *y = 0;
            if (*y >= bounds_h) *y = bounds_h - 1;
            if (*y + *height > bounds_h) *height = bounds_h - *y;
            if (*height < 1) *height = 1;
        }
    }
}

static void compute_popup_geometry(struct bridge_surface *popup,
                                   struct bridge_surface *parent,
                                   const struct xdg_positioner_data *pos) {
    int32_t x, y;
    int32_t width = pos->width;
    int32_t height = pos->height;

    positioner_calc_xy(pos, pos->anchor, pos->gravity, width, height, &x, &y);
    positioner_apply_constraints(pos, parent, &x, &y, &width, &height);

    popup->popup_x = x;
    popup->popup_y = y;
    popup->popup_width = width;
    popup->popup_height = height;
}

int32_t popup_sprot_x(const struct bridge_surface *s) {
    return s ? s->popup_x + bridge_surface_window_x(s->popup_parent) : 0;
}

int32_t popup_sprot_y(const struct bridge_surface *s) {
    return s ? s->popup_y + bridge_surface_window_y(s->popup_parent) : 0;
}

int send_sprot_role_for_surface(struct bridge_surface *s) {
    if (!s || !s->sprot_surface || s->sprot_id == 0) return -1;

    uint32_t role = SPROT_SURFACE_ROLE_TOPLEVEL;
    uint32_t parent_id = 0;
    int32_t x = 0;
    int32_t y = 0;

    if (s->is_popup) {
        if (!s->popup_parent || s->popup_parent->sprot_id == 0) return -2;
        role = SPROT_SURFACE_ROLE_POPUP;
        parent_id = s->popup_parent->sprot_id;
        x = popup_sprot_x(s);
        y = popup_sprot_y(s);
    } else if (s->is_subsurface) {
        if (!s->subsurface_parent || s->subsurface_parent->sprot_id == 0) return -2;
        role = SPROT_SURFACE_ROLE_SUBSURFACE;
        parent_id = s->subsurface_parent->sprot_id;
        x = s->subsurface_x;
        y = s->subsurface_y;
    }

    if (s->role_sent && s->last_role == role && s->last_role_parent_id == parent_id &&
        s->last_role_x == x && s->last_role_y == y) {
        return 0;
    }

    int res = sprot_set_role(s->sprot_surface, role, parent_id, x, y);
    if (res == 0) {
        s->role_sent = 1;
        s->last_role = role;
        s->last_role_parent_id = parent_id;
        s->last_role_x = x;
        s->last_role_y = y;
    }
    return res;
}

void configure_popup_surface(struct bridge_surface *s, uint32_t token, int repositioned) {
    if (!s || !s->xdg_popup_resource || !s->xdg_surface_resource) return;
    uint32_t version = wl_resource_get_version(s->xdg_popup_resource);
    if (repositioned && version >= XDG_POPUP_REPOSITIONED_SINCE_VERSION) {
        xdg_popup_send_repositioned(s->xdg_popup_resource, token);
    }
    xdg_popup_send_configure(s->xdg_popup_resource, s->popup_x, s->popup_y,
                             s->popup_width, s->popup_height);
    xdg_surface_send_configure(s->xdg_surface_resource, next_keyboard_serial(s->client));
    s->configured = 1;
}

// XDG WM Base (xdg_wm_base) implementation
static void xdg_wm_base_destroy(struct wl_client *client, struct wl_resource *resource) {
    wl_resource_destroy(resource);
}

static void xdg_positioner_resource_destroy(struct wl_resource *resource) {
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    free(pos);
}

static void xdg_positioner_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_positioner_set_size(struct wl_client *client, struct wl_resource *resource,
                                    int32_t width, int32_t height) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad positioner size");
        return;
    }
    pos->has_size = 1;
    pos->width = width;
    pos->height = height;
}

static void xdg_positioner_set_anchor_rect(struct wl_client *client, struct wl_resource *resource,
                                           int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    if (width < 0 || height < 0) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad anchor rect");
        return;
    }
    pos->has_anchor_rect = 1;
    pos->anchor_rect_x = x;
    pos->anchor_rect_y = y;
    pos->anchor_rect_width = width;
    pos->anchor_rect_height = height;
}

static void xdg_positioner_set_anchor(struct wl_client *client, struct wl_resource *resource, uint32_t anchor) {
    (void)client;
    if (!xdg_positioner_anchor_is_valid(anchor, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad anchor");
        return;
    }
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->anchor = anchor;
}

static void xdg_positioner_set_gravity(struct wl_client *client, struct wl_resource *resource, uint32_t gravity) {
    (void)client;
    if (!xdg_positioner_gravity_is_valid(gravity, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad gravity");
        return;
    }
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->gravity = gravity;
}

static void xdg_positioner_set_constraint_adjustment(struct wl_client *client, struct wl_resource *resource,
                                                     uint32_t constraint_adjustment) {
    (void)client;
    if (!xdg_positioner_constraint_adjustment_is_valid(constraint_adjustment, wl_resource_get_version(resource))) {
        wl_resource_post_error(resource, XDG_POSITIONER_ERROR_INVALID_INPUT, "bad constraint adjustment");
        return;
    }
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->constraint_adjustment = constraint_adjustment;
}

static void xdg_positioner_set_offset(struct wl_client *client, struct wl_resource *resource,
                                      int32_t x, int32_t y) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->offset_x = x;
    pos->offset_y = y;
}

static void xdg_positioner_set_reactive(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->reactive = 1;
}

static void xdg_positioner_set_parent_size(struct wl_client *client, struct wl_resource *resource,
                                           int32_t parent_width, int32_t parent_height) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->has_parent_size = parent_width > 0 && parent_height > 0;
    pos->parent_width = parent_width;
    pos->parent_height = parent_height;
}

static void xdg_positioner_set_parent_configure(struct wl_client *client, struct wl_resource *resource,
                                                uint32_t serial) {
    (void)client;
    struct xdg_positioner_data *pos = wl_resource_get_user_data(resource);
    pos->has_parent_configure = 1;
    pos->parent_configure_serial = serial;
}

static const struct xdg_positioner_interface xdg_positioner_implementation = {
    .destroy = xdg_positioner_destroy,
    .set_size = xdg_positioner_set_size,
    .set_anchor_rect = xdg_positioner_set_anchor_rect,
    .set_anchor = xdg_positioner_set_anchor,
    .set_gravity = xdg_positioner_set_gravity,
    .set_constraint_adjustment = xdg_positioner_set_constraint_adjustment,
    .set_offset = xdg_positioner_set_offset,
    .set_reactive = xdg_positioner_set_reactive,
    .set_parent_size = xdg_positioner_set_parent_size,
    .set_parent_configure = xdg_positioner_set_parent_configure,
};

static void xdg_wm_base_create_positioner(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    debug_log("xdg_wm_base_create_positioner: id=%u", id);
    struct xdg_positioner_data *pos = calloc(1, sizeof(*pos));
    if (!pos) {
        wl_client_post_no_memory(client);
        return;
    }
    struct wl_resource *positioner = wl_resource_create(client, &xdg_positioner_interface,
                                                       wl_resource_get_version(resource), id);
    if (!positioner) {
        free(pos);
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(positioner, &xdg_positioner_implementation,
                                   pos, xdg_positioner_resource_destroy);
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

static void xdg_toplevel_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s && s->xdg_toplevel_resource == resource) {
        s->xdg_toplevel_resource = NULL;
    }
}

static void xdg_surface_get_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    debug_log("Wayland request: xdg_surface.get_toplevel for surface handle=%u", s->client_handle);

    if (s->xdg_toplevel_resource || s->xdg_popup_resource) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "xdg_surface already has a role");
        return;
    }

    s->xdg_toplevel_resource = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
    wl_resource_set_implementation(s->xdg_toplevel_resource, &xdg_toplevel_implementation, s, xdg_toplevel_resource_destroy);
}

static void xdg_popup_resource_destroy(struct wl_resource *resource) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (!s || s->xdg_popup_resource != resource) return;

    debug_log("xdg_popup destroyed for surface handle=%u", s->client_handle);
    s->xdg_popup_resource = NULL;
    s->is_popup = 0;
    s->popup_grabbed = 0;
    s->popup_parent = NULL;
    s->configured = 0;
    if (s->sprot_surface) {
        sprot_destroy_surface(s->sprot_surface);
        s->sprot_surface = NULL;
        s->sprot_id = 0;
        s->role_sent = 0;
    }
}

static void xdg_popup_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void xdg_popup_grab(struct wl_client *client, struct wl_resource *resource,
                           struct wl_resource *seat, uint32_t serial) {
    (void)client; (void)seat;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s) {
        s->popup_grabbed = 1;
        debug_log("xdg_popup.grab: handle=%u serial=%u", s->client_handle, serial);
    }
}

static void xdg_popup_reposition(struct wl_client *client, struct wl_resource *resource,
                                 struct wl_resource *positioner, uint32_t token) {
    (void)client;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    struct xdg_positioner_data *pos = positioner ? wl_resource_get_user_data(positioner) : NULL;
    if (!s || !pos || !pos->has_size || !pos->has_anchor_rect) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "bad popup repositioner");
        return;
    }

    compute_popup_geometry(s, s->popup_parent, pos);
    if (s->sprot_surface && s->sprot_id != 0) {
        send_sprot_role_for_surface(s);
    }
    debug_log("xdg_popup.reposition: handle=%u pos=%d,%d size=%dx%d token=%u",
              s->client_handle, s->popup_x, s->popup_y,
              s->popup_width, s->popup_height, token);
    configure_popup_surface(s, token, 1);
}

static const struct xdg_popup_interface xdg_popup_implementation = {
    .destroy = xdg_popup_destroy,
    .grab = xdg_popup_grab,
    .reposition = xdg_popup_reposition,
};

static void xdg_surface_get_popup(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *parent, struct wl_resource *positioner) {
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    struct bridge_surface *parent_s = parent ? wl_resource_get_user_data(parent) : NULL;
    struct xdg_positioner_data *pos = positioner ? wl_resource_get_user_data(positioner) : NULL;
    debug_log("Wayland request: xdg_surface.get_popup for surface handle=%u", s ? s->client_handle : 0);

    if (!s || !parent_s || (!parent_s->xdg_toplevel_resource && !parent_s->xdg_popup_resource)) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT, "bad popup parent");
        return;
    }
    if (!pos || !pos->has_size || !pos->has_anchor_rect) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_INVALID_POSITIONER, "bad popup positioner");
        return;
    }
    if (s->xdg_toplevel_resource || s->xdg_popup_resource) {
        wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE, "xdg_surface already has a role");
        return;
    }

    s->xdg_popup_resource = wl_resource_create(client, &xdg_popup_interface,
                                               wl_resource_get_version(resource), id);
    if (!s->xdg_popup_resource) {
        wl_client_post_no_memory(client);
        return;
    }

    s->is_popup = 1;
    s->popup_parent = parent_s;
    compute_popup_geometry(s, parent_s, pos);

    wl_resource_set_implementation(s->xdg_popup_resource, &xdg_popup_implementation,
                                   s, xdg_popup_resource_destroy);
    debug_log("xdg_surface.get_popup: handle=%u parent_handle=%u pos=%d,%d size=%dx%d",
              s->client_handle, parent_s->client_handle, s->popup_x, s->popup_y,
              s->popup_width, s->popup_height);
    configure_popup_surface(s, 0, 0);
}

static void xdg_surface_set_window_geometry(struct wl_client *client, struct wl_resource *resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)client;
    struct bridge_surface *s = wl_resource_get_user_data(resource);
    if (s && width > 0 && height > 0) {
        s->window_geometry_set = 1;
        s->window_geometry_x = x;
        s->window_geometry_y = y;
        s->window_geometry_width = width;
        s->window_geometry_height = height;
    }
    debug_log("Wayland request: xdg_surface.set_window_geometry to %d,%d %dx%d", x, y, width, height);
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

void xdg_wm_base_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    (void)data;
    pid_t pid = 0; uid_t uid = 0; gid_t gid = 0;
    wl_client_get_credentials(client, &pid, &uid, &gid);
    debug_log("xdg_wm_base_bind: version=%u id=%u pid=%d", version, id, (int)pid);
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
