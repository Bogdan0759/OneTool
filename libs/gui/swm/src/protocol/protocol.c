#define _GNU_SOURCE
#include "protocol.h"

#include "../backend/backend.h"
#include "../buffer/buffer.h"
#include "../de/de.h"
#include "../probe/render_node.h"
#include "../surface/surface.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int swm_protocol_send_event(int fd, uint16_t type, uint32_t object_id,
                            uint32_t serial, const void *body, size_t body_len) {
    sprot_header_t hdr = {
        .type = type,
        .object_id = object_id,
        .serial = serial,
    };
    return sprot_send_message(fd, &hdr, body, body_len, -1);
}

int swm_protocol_send_event_nb(int fd, uint16_t type, uint32_t object_id,
                               uint32_t serial, const void *body, size_t body_len) {
    struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
    int pr = poll(&pfd, 1, 0);
    if (pr <= 0 || (pfd.revents & POLLOUT) == 0) return -1;
    return swm_protocol_send_event(fd, type, object_id, serial, body, body_len);
}

static void send_error(int fd, uint32_t code, const char *msg) {
    char buf[256];
    sprot_body_error_t body = { .code = code, .length = (uint32_t)strlen(msg) };
    if (body.length > sizeof(buf) - sizeof(body)) {
        body.length = sizeof(buf) - sizeof(body);
    }
    memcpy(buf, &body, sizeof(body));
    memcpy(buf + sizeof(body), msg, body.length);
    sprot_header_t hdr = { .type = SPROT_EVT_ERROR };
    sprot_send_message(fd, &hdr, buf, sizeof(body) + body.length, -1);
}

void swm_protocol_clear_client_cursor(swm_state_t *swm) {
    if (swm == NULL) return;
    if (swm->cursor_buffer != NULL) {
        swm_buffer_destroy(swm->cursor_buffer);
        swm->cursor_buffer = NULL;
    }
    swm->cursor_visible = 0;
    swm->current_cursor = SPROT_CURSOR_ARROW;
}

void swm_protocol_drop_client(swm_state_t *swm, swm_client_t *c, const char *reason) {
    if (swm == NULL || c == NULL || !c->in_use) return;
    fprintf(stderr, "[swm] client fd=%d dropped: %s\n", c->sock, reason);
    while (c->surface_count > 0) {
        swm_surface_free(swm, c->surfaces[c->surface_count - 1]);
    }
    if (c->sock >= 0) close(c->sock);
    memset(c, 0, sizeof(*c));
}

swm_client_t *swm_protocol_alloc_client(swm_state_t *swm, int sock) {
    if (swm == NULL) return NULL;
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (!swm->clients[i].in_use) {
            memset(&swm->clients[i], 0, sizeof(swm->clients[i]));
            swm->clients[i].in_use = 1;
            swm->clients[i].sock = sock;
            return &swm->clients[i];
        }
    }
    return NULL;
}

int swm_protocol_setup_socket(swm_state_t *swm, const char *path) {
    struct sockaddr_un addr;
    int fd;

    if (swm == NULL) return -1;
    if (path == NULL || path[0] == '\0') path = SPROT_DEFAULT_SOCKET;
    snprintf(swm->socket_path, sizeof(swm->socket_path), "%s", path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        fprintf(stderr, "[swm] socket: %s\n", strerror(errno));
        return -1;
    }
    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[swm] bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (chmod(path, 0666) != 0) {
        fprintf(stderr, "[swm] chmod %s: %s (continuing)\n", path, strerror(errno));
    }
    if (listen(fd, 8) != 0) {
        fprintf(stderr, "[swm] listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    fprintf(stderr, "[swm] listening on %s\n", path);
    return fd;
}

static int handle_hello(swm_state_t *swm, swm_client_t *c,
                        const sprot_header_t *hdr, const void *body, size_t blen) {
    (void)body;
    if (blen < sizeof(sprot_body_hello_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short HELLO");
        return -1;
    }
    sprot_body_welcome_t welcome = {
        .display_width = swm->display_w,
        .display_height = swm->display_h,
        .version_major = SPROT_VERSION_MAJOR,
        .version_minor = SPROT_VERSION_MINOR,
    };
    if (swm_protocol_send_event(c->sock, SPROT_EVT_WELCOME, 0, hdr->serial,
                                &welcome, sizeof(welcome)) != 0) {
        return -1;
    }
    c->has_hello = 1;
    return 0;
}

static int handle_surface_create(swm_state_t *swm, swm_client_t *c,
                                 const sprot_header_t *hdr, const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_surface_create_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_CREATE");
        return -1;
    }
    sprot_body_surface_create_t b;
    memcpy(&b, body, sizeof(b));
    if (b.format != SPROT_PIXEL_FORMAT_BGRA8888 || b.width == 0 || b.height == 0 ||
        b.width > swm->display_w * 2 || b.height > swm->display_h * 2) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface dims");
        return -1;
    }
    if (c->surface_count >= (int)(sizeof(c->surfaces) / sizeof(c->surfaces[0]))) {
        send_error(c->sock, SPROT_ERROR_LIMIT, "surface limit per client");
        return -1;
    }
    swm_surface_t *s = swm_surface_alloc(swm);
    if (s == NULL) {
        send_error(c->sock, SPROT_ERROR_LIMIT, "server surface table full");
        return -1;
    }
    s->id = ++swm->next_surface_id;
    s->client_handle = b.client_handle;
    s->owner = c;
    s->width = b.width;
    s->height = b.height;
    s->stride = b.width * 4u;
    s->buffer_size = (size_t)s->stride * b.height;
    s->pos_x = swm->next_cascade_x;
    s->pos_y = swm->next_cascade_y;
    swm->next_cascade_x += 32;
    swm->next_cascade_y += 32;
    if (swm->next_cascade_x + 100 > (int32_t)swm->display_w) swm->next_cascade_x = 32;
    {
        int32_t bottom_limit = (int32_t)swm->display_h;
        if (swm->de != NULL) {
            int32_t top = de_workspace_height(swm->de);
            if (top > 0 && top < bottom_limit) bottom_limit = top;
        }
        if (swm->next_cascade_y + 100 > bottom_limit) swm->next_cascade_y = 32;
    }
    s->z = ++swm->next_z;
    snprintf(s->title, sizeof(s->title), "client #%d", c->sock);
    c->surfaces[c->surface_count++] = s;

    sprot_body_surface_created_t resp = { .surface_id = s->id, .client_handle = s->client_handle };
    if (swm_protocol_send_event(c->sock, SPROT_EVT_SURFACE_CREATED, s->id,
                                hdr->serial, &resp, sizeof(resp)) != 0) {
        return -1;
    }
    fprintf(stderr, "[swm] surface %u created %ux%u for client fd=%d at (%d,%d)\n",
            s->id, s->width, s->height, c->sock, s->pos_x, s->pos_y);
    return 0;
}

static int validate_surface_buffer_dims(swm_state_t *swm, uint32_t width, uint32_t height,
                                        uint32_t stride, size_t buffer_size) {
    if (width == 0 || height == 0 || stride < width * 4u ||
        buffer_size < (size_t)stride * height ||
        width > swm->display_w * 2 || height > swm->display_h * 2) {
        return -1;
    }
    return 0;
}

static int handle_surface_attach(swm_state_t *swm, swm_client_t *c,
                                 const sprot_header_t *hdr, const void *body,
                                 size_t blen, int incoming_fd) {
    if (blen < sizeof(sprot_body_surface_attach_t)) {
        if (incoming_fd >= 0) close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_ATTACH");
        return -1;
    }
    if (incoming_fd < 0) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "SURFACE_ATTACH missing fd");
        return -1;
    }
    sprot_body_surface_attach_t b;
    memcpy(&b, body, sizeof(b));
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if (validate_surface_buffer_dims(swm, b.width, b.height, b.stride, b.buffer_size) != 0) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad ATTACH dims");
        return -1;
    }
    s->width = b.width;
    s->height = b.height;
    s->stride = b.stride;
    s->buffer_size = b.buffer_size;
    swm_buffer_destroy(s->buffer);
    s->buffer = swm_buffer_create(SPROT_BUFFER_SHM, incoming_fd, s->width, s->height,
                                  s->stride, s->buffer_size);
    if (s->buffer == NULL) {
        send_error(c->sock, SPROT_ERROR_OUT_OF_MEMORY, "mmap failed");
        return -1;
    }
    s->buffer_kind = SPROT_BUFFER_SHM;
    s->buffer_has_alpha = 1;
    s->has_pending = 1;
    return 0;
}

static int handle_surface_attach_buffer(swm_state_t *swm, swm_client_t *c,
                                        const sprot_header_t *hdr, const void *body,
                                        size_t blen, int incoming_fd) {
    sprot_body_surface_attach_buffer_t b;
    swm_surface_t *s;

    if (blen < sizeof(b)) {
        if (incoming_fd >= 0) close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_ATTACH_BUFFER");
        return -1;
    }
    if (incoming_fd < 0) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "SURFACE_ATTACH_BUFFER missing fd");
        return -1;
    }

    memcpy(&b, body, sizeof(b));
    s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if ((b.buffer_kind != SPROT_BUFFER_SHM && b.buffer_kind != SPROT_BUFFER_DMABUF) ||
        b.format != SPROT_PIXEL_FORMAT_BGRA8888 ||
        validate_surface_buffer_dims(swm, b.width, b.height, b.stride, b.buffer_size) != 0) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad ATTACH_BUFFER dims");
        return -1;
    }

    s->width = b.width;
    s->height = b.height;
    s->stride = b.stride;
    s->buffer_size = b.buffer_size;
    swm_buffer_destroy(s->buffer);
    s->buffer = swm_buffer_create(b.buffer_kind, incoming_fd, b.width, b.height,
                                  b.stride, b.buffer_size);
    if (s->buffer == NULL) {
        send_error(c->sock, SPROT_ERROR_OUT_OF_MEMORY, "buffer import failed");
        return -1;
    }
    s->buffer_kind = b.buffer_kind;
    s->buffer_has_alpha = 1;
    s->has_pending = 1;
    return 0;
}

static int handle_surface_commit(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if (s->buffer == NULL) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "commit without attached buffer");
        return -1;
    }
    s->committed = 1;
    s->has_pending = 0;
    return 0;
}

static int handle_surface_destroy(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    fprintf(stderr, "[swm] surface %u destroyed\n", s->id);
    swm_surface_free(swm, s);
    return 0;
}

static int handle_surface_set_title(swm_state_t *swm, swm_client_t *c,
                                    const sprot_header_t *hdr, const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_set_title_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SET_TITLE");
        return -1;
    }
    sprot_body_set_title_t b;
    memcpy(&b, body, sizeof(b));
    if (b.length > SPROT_MAX_TITLE || sizeof(b) + b.length > blen) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "bad SET_TITLE length");
        return -1;
    }
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    if (b.length > sizeof(s->title) - 1) b.length = sizeof(s->title) - 1;
    memcpy(s->title, (const uint8_t *)body + sizeof(b), b.length);
    s->title[b.length] = '\0';
    return 0;
}

static int handle_surface_frame(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    s->wants_frame = 1;
    return 0;
}

static int handle_surface_set_role(swm_state_t *swm, swm_client_t *c,
                                   const sprot_header_t *hdr, const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_surface_set_role_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SET_ROLE");
        return -1;
    }
    sprot_body_surface_set_role_t b;
    memcpy(&b, body, sizeof(b));
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    if (b.role != SPROT_SURFACE_ROLE_TOPLEVEL && !swm_surface_role_is_child(b.role)) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface role");
        return -1;
    }
    int z_order_changed = s->role != b.role || s->parent_id != b.parent_id;
    s->role = b.role;
    if (swm_surface_role_is_child(b.role)) {
        swm_surface_t *parent = swm_surface_find(swm, b.parent_id);
        if (parent == NULL || parent == s || parent->owner != c) {
            send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad popup parent");
            return -1;
        }
        s->parent_id = b.parent_id;
        s->rel_x = b.x;
        s->rel_y = b.y;
        s->maximized = 0;
        s->minimized = 0;
        if (z_order_changed || s->z == 0) s->z = ++swm->next_z;
    } else {
        s->parent_id = 0;
        s->rel_x = 0;
        s->rel_y = 0;
    }
    return 0;
}

static int handle_set_cursor(swm_state_t *swm, swm_client_t *c,
                             const sprot_header_t *hdr, const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_set_cursor_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SET_CURSOR");
        return -1;
    }
    sprot_body_set_cursor_t b;
    memcpy(&b, body, sizeof(b));
    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    if (swm->hovered_surface != s) return 0;
    swm_protocol_clear_client_cursor(swm);
    swm->current_cursor = b.cursor_type;
    return 0;
}

static int handle_set_cursor_image(swm_state_t *swm, swm_client_t *c,
                                   const sprot_header_t *hdr, const void *body,
                                   size_t blen, int incoming_fd) {
    if (blen < sizeof(sprot_body_set_cursor_image_t)) {
        if (incoming_fd >= 0) close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SET_CURSOR_IMAGE");
        return -1;
    }

    sprot_body_set_cursor_image_t b;
    memcpy(&b, body, sizeof(b));

    swm_surface_t *s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c || swm->hovered_surface != s) {
        if (incoming_fd >= 0) close(incoming_fd);
        return 0;
    }

    if (!b.visible) {
        if (incoming_fd >= 0) close(incoming_fd);
        swm_protocol_clear_client_cursor(swm);
        return 0;
    }

    if (incoming_fd < 0) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "SET_CURSOR_IMAGE missing fd");
        return -1;
    }
    if (b.format != SPROT_PIXEL_FORMAT_BGRA8888 || b.width == 0 || b.height == 0 ||
        b.width > 256 || b.height > 256 || b.stride < b.width * 4u ||
        b.buffer_size < b.stride * b.height) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad cursor image");
        return -1;
    }

    swm_buffer_t *cursor = swm_buffer_create(SPROT_BUFFER_SHM, incoming_fd,
                                             b.width, b.height, b.stride, b.buffer_size);
    if (cursor == NULL) {
        send_error(c->sock, SPROT_ERROR_OUT_OF_MEMORY, "cursor import failed");
        return -1;
    }
    swm_buffer_destroy(swm->cursor_buffer);
    swm->cursor_buffer = cursor;
    swm->cursor_visible = 1;
    swm->cursor_hotspot_x = b.hotspot_x;
    swm->cursor_hotspot_y = b.hotspot_y;
    return 0;
}

static int handle_ping(swm_client_t *c, const sprot_header_t *hdr) {
    return swm_protocol_send_event(c->sock, SPROT_EVT_PONG, 0, hdr->serial, NULL, 0);
}

static int handle_query_render_node(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    sprot_body_render_node_t body;
    const char *path = swm_output_device_path(swm->output);
    swm_probe_render_node(path, &body);
    return swm_protocol_send_event(c->sock, SPROT_EVT_RENDER_NODE, 0, hdr->serial,
                                   &body, sizeof(body));
}

static int handle_surface_attach_dmabuf(swm_state_t *swm, swm_client_t *c,
                                        const sprot_header_t *hdr, const void *body,
                                        size_t blen, int incoming_fd) {
    sprot_body_surface_attach_dmabuf_t b;
    swm_surface_t *s;
    uint32_t stride;

    if (blen < sizeof(b)) {
        if (incoming_fd >= 0) close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_ATTACH_DMABUF");
        return -1;
    }
    if (incoming_fd < 0) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "SURFACE_ATTACH_DMABUF missing fd");
        return -1;
    }
    memcpy(&b, body, sizeof(b));

    s = swm_surface_find(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }

    if (b.modifier != SPROT_DRM_FORMAT_MOD_LINEAR) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG,
                   "swm only accepts DRM_FORMAT_MOD_LINEAR");
        return -1;
    }
    if (b.num_planes != 1) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG,
                   "multi-plane dmabuf not supported");
        return -1;
    }
    if (b.drm_format != SPROT_DRM_FORMAT_ARGB8888 &&
        b.drm_format != SPROT_DRM_FORMAT_XRGB8888) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG,
                   "swm only accepts DRM ARGB/XRGB 8888");
        return -1;
    }
    if (validate_surface_buffer_dims(swm, b.width, b.height, b.plane_strides[0],
                                     (size_t)b.total_size) != 0) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad ATTACH_DMABUF dims");
        return -1;
    }

    stride = b.plane_strides[0];
    if (stride < b.width * 4u) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "plane stride too small for BGRA");
        return -1;
    }

    if (b.plane_offsets[0] != 0) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG,
                   "plane offset != 0 not supported by current swm");
        return -1;
    }
    uint32_t expected_min = stride * b.height;
    if (b.total_size < expected_min) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "dmabuf total_size too small");
        return -1;
    }

    swm_buffer_destroy(s->buffer);
    errno = 0;
    s->buffer = swm_buffer_create(SPROT_BUFFER_DMABUF, incoming_fd,
                                  b.width, b.height, stride, (size_t)b.total_size);
    if (s->buffer == NULL) {
        char msg[128];
        snprintf(msg, sizeof(msg), "dmabuf import failed: %s",
                 errno ? strerror(errno) : "unknown error");
        send_error(c->sock, SPROT_ERROR_OUT_OF_MEMORY, msg);
        return -1;
    }

    s->width        = b.width;
    s->height       = b.height;
    s->stride       = stride;
    s->buffer_size  = (size_t)b.total_size;
    s->buffer_kind  = SPROT_BUFFER_DMABUF;
    s->buffer_has_alpha = (b.drm_format == SPROT_DRM_FORMAT_ARGB8888);
    s->has_pending  = 1;
    return 0;
}

int swm_protocol_dispatch(swm_state_t *swm, swm_client_t *c) {
    sprot_header_t hdr;
    uint8_t body[SPROT_MAX_MESSAGE];
    int incoming_fd = -1;

    int r = sprot_recv_message(c->sock, &hdr, body, sizeof(body), &incoming_fd);
    if (r != 0) {
        swm_protocol_drop_client(swm, c, strerror(errno));
        return -1;
    }
    size_t blen = hdr.length >= sizeof(hdr) ? hdr.length - sizeof(hdr) : 0;

    if (!c->has_hello && hdr.type != SPROT_REQ_HELLO) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "must HELLO first");
        if (incoming_fd >= 0) close(incoming_fd);
        swm_protocol_drop_client(swm, c, "no HELLO");
        return -1;
    }

    int rc = 0;
    switch (hdr.type) {
        case SPROT_REQ_HELLO: rc = handle_hello(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_CREATE: rc = handle_surface_create(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_ATTACH:
            rc = handle_surface_attach(swm, c, &hdr, body, blen, incoming_fd);
            incoming_fd = -1;
            break;
        case SPROT_REQ_SURFACE_ATTACH_BUFFER:
            rc = handle_surface_attach_buffer(swm, c, &hdr, body, blen, incoming_fd);
            incoming_fd = -1;
            break;
        case SPROT_REQ_SURFACE_ATTACH_DMABUF:
            rc = handle_surface_attach_dmabuf(swm, c, &hdr, body, blen, incoming_fd);
            incoming_fd = -1;
            break;
        case SPROT_REQ_QUERY_RENDER_NODE: rc = handle_query_render_node(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_COMMIT: rc = handle_surface_commit(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_DESTROY: rc = handle_surface_destroy(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_DAMAGE: break;
        case SPROT_REQ_SURFACE_FRAME: rc = handle_surface_frame(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_SET_TITLE: rc = handle_surface_set_title(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_SET_ROLE: rc = handle_surface_set_role(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SET_CURSOR: rc = handle_set_cursor(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SET_CURSOR_IMAGE:
            rc = handle_set_cursor_image(swm, c, &hdr, body, blen, incoming_fd);
            incoming_fd = -1;
            break;
        case SPROT_REQ_PING: rc = handle_ping(c, &hdr); break;
        default:
            fprintf(stderr, "[swm] unknown msg type 0x%x from fd=%d\n", hdr.type, c->sock);
            send_error(c->sock, SPROT_ERROR_PROTOCOL, "unknown message type");
            break;
    }
    if (incoming_fd >= 0) close(incoming_fd);
    return rc;
}
