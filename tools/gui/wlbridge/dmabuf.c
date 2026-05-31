#define _GNU_SOURCE
#include "wlbridge_internal.h"

// DMA-BUF buffer implementation
static void dmabuf_buffer_destroy(struct wl_resource *resource) {
    struct dmabuf_buffer *buf = wl_resource_get_user_data(resource);
    if (!buf) return;

    wl_resource_set_user_data(resource, NULL);
    if (buf->fd >= 0) {
        close(buf->fd);
    }
    free(buf);
}

static void dmabuf_buffer_destroy_request(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct wl_buffer_interface dmabuf_buffer_implementation = {
    .destroy = dmabuf_buffer_destroy_request,
};

// DMA-BUF params implementation
static void params_destroy_handler(struct wl_resource *resource) {
    struct dmabuf_params *params = wl_resource_get_user_data(resource);
    if (!params) return;

    for (uint32_t i = 0; i < 4; i++) {
        if (params->fds[i] >= 0) {
            close(params->fds[i]);
        }
    }
    free(params);
}

static void params_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void params_add(struct wl_client *client, struct wl_resource *resource,
                       int32_t fd, uint32_t plane_idx, uint32_t offset,
                       uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "params already used");
        close(fd);
        return;
    }

    if (plane_idx >= 4) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                              "plane index too large");
        close(fd);
        return;
    }

    if (params->plane_set[plane_idx]) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                              "plane already set");
        close(fd);
        return;
    }

    uint64_t modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;

    if (params->num_planes > 0 && params->modifier != modifier) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "all planes must have same modifier");
        close(fd);
        return;
    }

    params->fds[plane_idx] = fd;
    params->offsets[plane_idx] = offset;
    params->strides[plane_idx] = stride;
    params->modifier = modifier;
    params->plane_set[plane_idx] = 1;
    params->num_planes++;

    debug_log("params_add: plane_idx=%u fd=%d offset=%u stride=%u modifier=0x%lx",
              plane_idx, fd, offset, stride, (unsigned long)modifier);
}

static struct wl_resource* params_create_buffer(struct dmabuf_params *params,
                                                int32_t width, int32_t height,
                                                uint32_t format, uint32_t flags,
                                                uint32_t buffer_id) {
    if (width <= 0 || height <= 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS,
                              "invalid dimensions");
        return NULL;
    }

    if (params->num_planes == 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE,
                              "no planes added");
        return NULL;
    }

    if (params->num_planes != 1) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "only single-plane buffers supported");
        return NULL;
    }

    if (format != 0x34325241 && format != 0x34325258) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "unsupported format (only ARGB8888/XRGB8888)");
        return NULL;
    }

    if (params->modifier != 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
                              "only LINEAR modifier supported");
        return NULL;
    }

    if (params->offsets[0] != 0) {
        wl_resource_post_error(params->resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
                              "offset must be 0");
        return NULL;
    }

    struct dmabuf_buffer *buf = calloc(1, sizeof(*buf));
    if (!buf) {
        return NULL;
    }

    buf->is_dmabuf = 1;
    buf->fd = params->fds[0];
    buf->width = width;
    buf->height = height;
    buf->format = format;
    buf->modifier = params->modifier;
    buf->num_planes = 1;
    buf->offsets[0] = params->offsets[0];
    buf->strides[0] = params->strides[0];
    buf->flags = flags;

    params->fds[0] = -1;

    if (buffer_id == 0) {
        buf->resource = wl_resource_create(wl_resource_get_client(params->resource),
                                           &wl_buffer_interface, 1, 0);
    } else {
        buf->resource = wl_resource_create(wl_resource_get_client(params->resource),
                                           &wl_buffer_interface, 1, buffer_id);
    }

    if (!buf->resource) {
        close(buf->fd);
        free(buf);
        return NULL;
    }

    wl_resource_set_implementation(buf->resource, &dmabuf_buffer_implementation, buf, dmabuf_buffer_destroy);

    debug_log("Created dmabuf buffer: %dx%d format=0x%x modifier=0x%lx stride=%u",
              width, height, format, (unsigned long)params->modifier, params->strides[0]);

    return buf->resource;
}

static void params_create(struct wl_client *client, struct wl_resource *resource,
                         int32_t width, int32_t height, uint32_t format, uint32_t flags) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "params already used");
        return;
    }
    params->used = 1;

    struct wl_resource *buffer = params_create_buffer(params, width, height, format, flags, 0);

    if (buffer) {
        zwp_linux_buffer_params_v1_send_created(resource, buffer);
    } else {
        zwp_linux_buffer_params_v1_send_failed(resource);
    }
}

static void params_create_immed(struct wl_client *client, struct wl_resource *resource,
                               uint32_t buffer_id, int32_t width, int32_t height,
                               uint32_t format, uint32_t flags) {
    (void)client;
    struct dmabuf_params *params = wl_resource_get_user_data(resource);

    if (params->used) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                              "params already used");
        return;
    }
    params->used = 1;

    struct wl_resource *buffer = params_create_buffer(params, width, height, format, flags, buffer_id);

    if (!buffer) {
        wl_resource_post_error(resource, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                              "failed to create buffer");
        return;
    }
}

static const struct zwp_linux_buffer_params_v1_interface params_implementation = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

// DMA-BUF feedback implementation
static void feedback_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void feedback_destroy_handler(struct wl_resource *resource) {
    struct dmabuf_feedback *feedback = wl_resource_get_user_data(resource);
    free(feedback);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_implementation = {
    .destroy = feedback_destroy,
};

static int ensure_render_node_info(struct bridge_client *c) {
    if (g_render_node_info.queried) {
        return g_render_node_info.has_drm;
    }

    g_render_node_info.queried = 1;
    if (!c || !c->sprot_conn) {
        return 0;
    }

    sprot_body_render_node_t info;
    if (sprot_query_render_node(c->sprot_conn, &info, 1000) != 0) {
        debug_log("ensure_render_node_info: query failed: %s", sprot_last_error());
        return 0;
    }

    if (!info.has_drm || info.render_node_path[0] == '\0') {
        debug_log("ensure_render_node_info: SWM has no render node");
        return 0;
    }

    strncpy(g_render_node_info.render_node_path, info.render_node_path,
            sizeof(g_render_node_info.render_node_path) - 1);
    g_render_node_info.render_node_path[sizeof(g_render_node_info.render_node_path) - 1] = '\0';
    g_render_node_info.render_major = info.render_major;
    g_render_node_info.render_minor = info.render_minor;
    g_render_node_info.has_drm = 1;
    debug_log("ensure_render_node_info: render node %s dev=%u:%u",
              g_render_node_info.render_node_path,
              g_render_node_info.render_major, g_render_node_info.render_minor);
    return 1;
}

static void send_dmabuf_feedback(struct bridge_client *c, struct wl_resource *feedback_resource) {
    ensure_render_node_info(c);

    if (!g_render_node_info.queried || !g_render_node_info.has_drm) {
        debug_log("send_dmabuf_feedback: render node not available, skipping");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    struct {
        uint32_t format;
        uint32_t padding;
        uint64_t modifier;
    } formats[] = {
        {0x34325241, 0, 0},
        {0x34325258, 0, 0},
    };

    int memfd = memfd_create("dmabuf-feedback", MFD_CLOEXEC);
    if (memfd < 0) {
        debug_log("send_dmabuf_feedback: memfd_create failed");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    size_t table_size = sizeof(formats);
    if (ftruncate(memfd, table_size) != 0) {
        close(memfd);
        debug_log("send_dmabuf_feedback: ftruncate failed");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    void *map = mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (map == MAP_FAILED) {
        close(memfd);
        debug_log("send_dmabuf_feedback: mmap failed");
        zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
        return;
    }

    memcpy(map, formats, table_size);
    munmap(map, table_size);

    zwp_linux_dmabuf_feedback_v1_send_format_table(feedback_resource, memfd, table_size);
    close(memfd);

    dev_t dev_id = makedev(g_render_node_info.render_major, g_render_node_info.render_minor);

    struct wl_array dev_array;
    wl_array_init(&dev_array);
    void *dev_data = wl_array_add(&dev_array, sizeof(dev_id));
    memcpy(dev_data, &dev_id, sizeof(dev_id));
    zwp_linux_dmabuf_feedback_v1_send_main_device(feedback_resource, &dev_array);

    struct wl_array indices;
    wl_array_init(&indices);
    uint16_t *idx0 = wl_array_add(&indices, sizeof(uint16_t));
    uint16_t *idx1 = wl_array_add(&indices, sizeof(uint16_t));
    *idx0 = 0;
    *idx1 = 1;

    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(feedback_resource, &dev_array);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(feedback_resource, &indices);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(feedback_resource, ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(feedback_resource);

    wl_array_release(&indices);
    wl_array_release(&dev_array);

    zwp_linux_dmabuf_feedback_v1_send_done(feedback_resource);
    debug_log("send_dmabuf_feedback: sent feedback with 2 formats");
}

// DMA-BUF global implementation
static void linux_dmabuf_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void linux_dmabuf_create_params(struct wl_client *client,
                                       struct wl_resource *resource, uint32_t params_id) {
    struct bridge_client *bridge_client = get_or_create_client(client);
    if (!bridge_client) {
        debug_log("linux_dmabuf_create_params: failed to get client");
        return;
    }

    struct dmabuf_params *params = calloc(1, sizeof(*params));
    if (!params) {
        wl_client_post_no_memory(client);
        return;
    }

    params->client = bridge_client;
    for (int i = 0; i < 4; i++) {
        params->fds[i] = -1;
    }

    params->resource = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                         wl_resource_get_version(resource), params_id);
    wl_resource_set_implementation(params->resource, &params_implementation,
                                  params, params_destroy_handler);

    debug_log("linux_dmabuf_create_params: created params object");
}

static void linux_dmabuf_get_default_feedback(struct wl_client *client,
                                              struct wl_resource *resource, uint32_t id) {
    (void)resource;
    struct bridge_client *bridge_client = get_or_create_client(client);
    struct dmabuf_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    wl_resource_set_implementation(feedback->resource, &feedback_implementation,
                                  feedback, feedback_destroy_handler);

    send_dmabuf_feedback(bridge_client, feedback->resource);
    debug_log("linux_dmabuf_get_default_feedback: sent default feedback");
}

static void linux_dmabuf_get_surface_feedback(struct wl_client *client,
                                              struct wl_resource *resource,
                                              uint32_t id, struct wl_resource *surface) {
    (void)resource;
    (void)surface;
    struct bridge_client *bridge_client = get_or_create_client(client);
    struct dmabuf_feedback *feedback = calloc(1, sizeof(*feedback));
    if (!feedback) {
        wl_client_post_no_memory(client);
        return;
    }

    feedback->surface_resource = surface;
    feedback->resource = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, 1, id);
    wl_resource_set_implementation(feedback->resource, &feedback_implementation,
                                  feedback, feedback_destroy_handler);

    send_dmabuf_feedback(bridge_client, feedback->resource);
    debug_log("linux_dmabuf_get_surface_feedback: sent surface feedback");
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_implementation = {
    .destroy = linux_dmabuf_destroy,
    .create_params = linux_dmabuf_create_params,
    .get_default_feedback = linux_dmabuf_get_default_feedback,
    .get_surface_feedback = linux_dmabuf_get_surface_feedback,
};

void linux_dmabuf_bind(struct wl_client *client, void *data,
                             uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
        &zwp_linux_dmabuf_v1_interface, version, id);
    wl_resource_set_implementation(resource, &linux_dmabuf_implementation, NULL, NULL);

    if (version < 4) {
        zwp_linux_dmabuf_v1_send_format(resource, 0x34325241);
        zwp_linux_dmabuf_v1_send_format(resource, 0x34325258);

        if (version >= 3) {
            zwp_linux_dmabuf_v1_send_modifier(resource, 0x34325241, 0, 0);
            zwp_linux_dmabuf_v1_send_modifier(resource, 0x34325258, 0, 0);
        }
    }

    debug_log("linux_dmabuf_bind: client bound to dmabuf interface version %u", version);
}
