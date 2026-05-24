#define _GNU_SOURCE
#include "protocol.h"
#include "window.h"
#include "buffer/buffer.h"
#include "compositor.h"
#include "de/de.h"

#include <sprot/sprot.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

void swm_logf(const char *fmt, ...);

static int swm_send_raw(int fd, const sprot_header_t *hdr, const void *body, size_t body_len, int attach_fd) {
    return sprot_send_message(fd, hdr, body, body_len, attach_fd);
}

static void swm_send_error(swm_state_t *swm, int fd, uint32_t code, const char *msg) {
    (void)swm;
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

static int send_event(swm_state_t *swm, int fd, uint16_t type, uint32_t object_id,
                      uint32_t serial, const void *body, size_t body_len) {
    (void)swm;
    sprot_header_t hdr = {
        .type = type,
        .object_id = object_id,
        .serial = serial,
    };
    return swm_send_raw(fd, &hdr, body, body_len, -1);
}

/* -------- protocol handlers -------- */

static int handle_hello(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                        const void *body, size_t blen) {
    (void)hdr;
    if (blen < sizeof(sprot_body_hello_t)) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short HELLO");
        return -1;
    }
    sprot_body_welcome_t welcome = {
        .display_width = swm->display_w,
        .display_height = swm->display_h,
        .version_major = SPROT_VERSION_MAJOR,
        .version_minor = SPROT_VERSION_MINOR,
    };
    if (send_event(swm, c->sock, SPROT_EVT_WELCOME, 0, hdr->serial, &welcome, sizeof(welcome)) != 0) {
        return -1;
    }
    c->has_hello = 1;
    return 0;
}

static int handle_surface_create(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                                 const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_surface_create_t)) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_CREATE");
        return -1;
    }
    sprot_body_surface_create_t b;
    memcpy(&b, body, sizeof(b));
    if (b.format != SPROT_PIXEL_FORMAT_BGRA8888 || b.width == 0 || b.height == 0 ||
        b.width > swm->display_w * 2 || b.height > swm->display_h * 2) {
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "bad surface dims");
        return -1;
    }
    if (c->surface_count >= (int)(sizeof(c->surfaces)/sizeof(c->surfaces[0]))) {
        swm_send_error(swm, c->sock, SPROT_ERROR_LIMIT, "surface limit per client");
        return -1;
    }
    swm_surface_t *s = swm_alloc_surface(swm);
    if (s == NULL) {
        swm_send_error(swm, c->sock, SPROT_ERROR_LIMIT, "server surface table full");
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

    swm_mark_dirty_surface_outer(swm, s);

    sprot_body_surface_created_t resp = { .surface_id = s->id, .client_handle = s->client_handle };
    if (send_event(swm, c->sock, SPROT_EVT_SURFACE_CREATED, s->id, hdr->serial, &resp, sizeof(resp)) != 0) {
        return -1;
    }
    swm_logf("surface %u created %ux%u for client fd=%d at (%d,%d)",
             s->id, s->width, s->height, c->sock, s->pos_x, s->pos_y);
    return 0;
}

static int handle_surface_attach(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                                 const void *body, size_t blen, int incoming_fd) {
    if (blen < sizeof(sprot_body_surface_attach_t)) {
        if (incoming_fd >= 0) close(incoming_fd);
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_ATTACH");
        return -1;
    }
    if (incoming_fd < 0) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "SURFACE_ATTACH missing fd");
        return -1;
    }
    sprot_body_surface_attach_t b;
    memcpy(&b, body, sizeof(b));
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        close(incoming_fd);
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if (b.width != s->width || b.height != s->height || b.stride != s->stride ||
        b.buffer_size != s->buffer_size) {
        close(incoming_fd);
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "ATTACH dims mismatch");
        return -1;
    }
    swm_buffer_destroy(s->buffer);
    s->buffer = swm_buffer_create(SPROT_BUFFER_SHM, incoming_fd, s->width, s->height, s->stride, s->buffer_size);
    if (s->buffer == NULL) {
        swm_send_error(swm, c->sock, SPROT_ERROR_OUT_OF_MEMORY, "mmap failed");
        return -1;
    }
    s->buffer_kind = SPROT_BUFFER_SHM;
    s->has_pending = 1;
    swm_mark_dirty_surface_outer(swm, s);
    return 0;
}

static int handle_surface_attach_buffer(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                                        const void *body, size_t blen, int incoming_fd) {
    sprot_body_surface_attach_buffer_t b;
    swm_surface_t *s;

    if (blen < sizeof(b)) {
        if (incoming_fd >= 0) close(incoming_fd);
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_ATTACH_BUFFER");
        return -1;
    }
    if (incoming_fd < 0) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "SURFACE_ATTACH_BUFFER missing fd");
        return -1;
    }

    memcpy(&b, body, sizeof(b));
    s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        close(incoming_fd);
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if ((b.buffer_kind != SPROT_BUFFER_SHM && b.buffer_kind != SPROT_BUFFER_DMABUF) ||
        b.format != SPROT_PIXEL_FORMAT_BGRA8888 ||
        b.width != s->width || b.height != s->height || b.stride != s->stride ||
        b.buffer_size != s->buffer_size) {
        close(incoming_fd);
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "ATTACH_BUFFER mismatch");
        return -1;
    }

    swm_buffer_destroy(s->buffer);
    s->buffer = swm_buffer_create(b.buffer_kind, incoming_fd, b.width, b.height, b.stride, b.buffer_size);
    if (s->buffer == NULL) {
        swm_send_error(swm, c->sock, SPROT_ERROR_OUT_OF_MEMORY, "buffer import failed");
        return -1;
    }
    s->buffer_kind = b.buffer_kind;
    s->has_pending = 1;
    swm_mark_dirty_surface_outer(swm, s);
    return 0;
}

static int handle_surface_commit(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if (s->buffer == NULL) {
        swm_send_error(swm, c->sock, SPROT_ERROR_INVALID_ARG, "commit without attached buffer");
        return -1;
    }
    s->committed = 1;
    s->has_pending = 0;
    swm_mark_dirty_surface_outer(swm, s);
    return 0;
}

static int handle_surface_destroy(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) {
        return 0;
    }
    swm_mark_dirty_surface_outer(swm, s);
    swm_logf("surface %u destroyed", s->id);
    swm_free_surface(swm, s);
    return 0;
}

static int handle_surface_set_title(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                                    const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_set_title_t)) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short SET_TITLE");
        return -1;
    }
    sprot_body_set_title_t b;
    memcpy(&b, body, sizeof(b));
    if (b.length > SPROT_MAX_TITLE || sizeof(b) + b.length > blen) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "bad SET_TITLE length");
        return -1;
    }
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    if (b.length > sizeof(s->title) - 1) b.length = sizeof(s->title) - 1;
    memcpy(s->title, (const uint8_t *)body + sizeof(b), b.length);
    s->title[b.length] = '\0';
    return 0;
}

static int handle_surface_frame(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    s->wants_frame = 1;
    return 0;
}

static int handle_surface_set_role(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                                   const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_surface_set_role_t)) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short SET_ROLE");
        return -1;
    }
    sprot_body_surface_set_role_t b;
    memcpy(&b, body, sizeof(b));
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    s->role = b.role;
    if (b.role == SPROT_SURFACE_ROLE_POPUP) {
        s->parent_id = b.parent_id;
        s->rel_x = b.x;
        s->rel_y = b.y;
    }
    return 0;
}

static int handle_set_cursor(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr,
                             const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_set_cursor_t)) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "short SET_CURSOR");
        return -1;
    }
    sprot_body_set_cursor_t b;
    memcpy(&b, body, sizeof(b));
    swm_surface_t *s = swm_find_surface(swm, hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    swm->current_cursor = b.cursor_type;
    return 0;
}

static int handle_ping(swm_state_t *swm, swm_client_t *c, const sprot_header_t *hdr) {
    return send_event(swm, c->sock, SPROT_EVT_PONG, 0, hdr->serial, NULL, 0);
}

/* -------- public API -------- */

int swm_dispatch_message(swm_state_t *swm, swm_client_t *c) {
    sprot_header_t hdr;
    uint8_t body[SPROT_MAX_MESSAGE];
    int incoming_fd = -1;

    int r = sprot_recv_message(c->sock, &hdr, body, sizeof(body), &incoming_fd);
    if (r != 0) {
        swm_drop_client(swm, c, strerror(errno));
        return -1;
    }
    size_t blen = hdr.length >= sizeof(hdr) ? hdr.length - sizeof(hdr) : 0;

    if (!c->has_hello && hdr.type != SPROT_REQ_HELLO) {
        swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "must HELLO first");
        if (incoming_fd >= 0) close(incoming_fd);
        swm_drop_client(swm, c, "no HELLO");
        return -1;
    }

    int rc = 0;
    switch (hdr.type) {
        case SPROT_REQ_HELLO:           rc = handle_hello(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_CREATE:  rc = handle_surface_create(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_ATTACH:  rc = handle_surface_attach(swm, c, &hdr, body, blen, incoming_fd);
                                        incoming_fd = -1; break;
        case SPROT_REQ_SURFACE_ATTACH_BUFFER: rc = handle_surface_attach_buffer(swm, c, &hdr, body, blen, incoming_fd);
                                        incoming_fd = -1; break;
        case SPROT_REQ_SURFACE_COMMIT:  rc = handle_surface_commit(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_DESTROY: rc = handle_surface_destroy(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_DAMAGE:  /* no-op v1, full-frame composite */ break;
        case SPROT_REQ_SURFACE_FRAME:   rc = handle_surface_frame(swm, c, &hdr); break;
        case SPROT_REQ_SURFACE_SET_TITLE: rc = handle_surface_set_title(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_SET_ROLE:  rc = handle_surface_set_role(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_SET_CURSOR:        rc = handle_set_cursor(swm, c, &hdr, body, blen); break;
        case SPROT_REQ_PING:            rc = handle_ping(swm, c, &hdr); break;
        default:
            swm_logf("unknown msg type 0x%x from fd=%d", hdr.type, c->sock);
            swm_send_error(swm, c->sock, SPROT_ERROR_PROTOCOL, "unknown message type");
            break;
    }
    if (incoming_fd >= 0) close(incoming_fd);
    return rc;
}

void swm_send_close_to(swm_state_t *swm, swm_surface_t *s) {
    (void)swm;
    if (s == NULL || s->owner == NULL) return;
    sprot_header_t hdr = { .type = SPROT_EVT_SURFACE_CLOSE, .object_id = s->id };
    sprot_send_message(s->owner->sock, &hdr, NULL, 0, -1);
}

void swm_send_configure_to(swm_state_t *swm, swm_surface_t *s, uint32_t state_flags) {
    (void)swm;
    if (s == NULL || s->owner == NULL) return;
    sprot_body_configure_t body = {
        .width = s->width,
        .height = s->height,
        .state = state_flags,
        .serial = ++swm->next_z,
    };
    sprot_header_t hdr = { .type = SPROT_EVT_SURFACE_CONFIGURE, .object_id = s->id, .serial = body.serial };
    sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
}

void swm_toggle_maximize(swm_state_t *swm, swm_surface_t *s) {
    if (!s->maximized) {
        s->saved_pos_x = s->pos_x;
        s->saved_pos_y = s->pos_y;
    } else {
        s->pos_x = s->saved_pos_x;
        s->pos_y = s->saved_pos_y;
    }
    s->maximized = !s->maximized;
    swm_mark_dirty_surface_outer(swm, s);
    swm_send_configure_to(swm, s, (s->maximized ? SPROT_SURFACE_STATE_MAXIMIZED : 0) | SPROT_SURFACE_STATE_FOCUSED);
}

void swm_deliver_frame_callbacks(swm_state_t *swm) {
    uint64_t now_ms = (uint64_t)swm->frame_count * 16u;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &swm->surfaces[i];
        if (!s->in_use || !s->wants_frame || s->owner == NULL) continue;
        s->wants_frame = 0;
        sprot_body_frame_t body = { .time_ms = (uint32_t)now_ms, .serial = (uint32_t)swm->frame_count };
        sprot_header_t hdr = { .type = SPROT_EVT_SURFACE_FRAME, .object_id = s->id, .serial = body.serial };
        sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
    }
}

int swm_setup_socket(swm_state_t *swm, const char *path) {
    struct sockaddr_un addr;
    int fd;

    if (path == NULL || path[0] == '\0') {
        path = SPROT_DEFAULT_SOCKET;
    }
    snprintf(swm->socket_path, sizeof(swm->socket_path), "%s", path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        swm_logf("socket: %s", strerror(errno));
        return -1;
    }
    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        swm_logf("bind %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (chmod(path, 0666) != 0) {
        swm_logf("chmod %s: %s (continuing)", path, strerror(errno));
    }
    if (listen(fd, 8) != 0) {
        swm_logf("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    swm_logf("listening on %s", path);
    return fd;
}
