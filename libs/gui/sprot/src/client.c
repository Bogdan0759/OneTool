#define _GNU_SOURCE
#include <sprot/client.h>
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

struct sprot_surface {
    sprot_connection_t *conn;
    uint32_t id;
    uint32_t client_handle;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    size_t size;
    int fd;
    void *map;
    int attached;
    uint32_t attach_kind;
    uint32_t attach_format;
    struct sprot_surface *next;
};

struct sprot_connection {
    int fd;
    uint32_t serial;
    uint32_t next_handle;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t version_major;
    uint32_t version_minor;
    int welcomed;
    struct sprot_surface *surfaces;
    sprot_event_t pending[16];
    int           pending_head;
    int           pending_count;
};

static char g_err[256] = "ok";

static void set_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

const char *sprot_last_error(void) {
    return g_err;
}
void sprot_internal_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

int sprot_internal_conn_fd(const sprot_connection_t *conn) {
    return conn != NULL ? conn->fd : -1;
}

uint32_t sprot_internal_conn_next_serial(sprot_connection_t *conn) {
    return conn != NULL ? conn->serial++ : 0;
}

sprot_connection_t *sprot_internal_surface_conn(sprot_surface_t *surface) {
    return surface != NULL ? surface->conn : NULL;
}

uint32_t sprot_internal_surface_id(const sprot_surface_t *surface) {
    return surface != NULL ? surface->id : 0;
}

void sprot_internal_surface_mark_attached(sprot_surface_t *surface,
                                           uint32_t kind, uint32_t format) {
    if (surface == NULL) return;
    surface->attached = 1;
    surface->attach_kind = kind;
    surface->attach_format = format;
}

int sprot_internal_pending_push(sprot_connection_t *conn, const sprot_event_t *ev) {
    if (conn == NULL || ev == NULL) return -1;
    int cap = (int)(sizeof(conn->pending)/sizeof(conn->pending[0]));
    if (conn->pending_count >= cap) return -1;
    int tail = (conn->pending_head + conn->pending_count) % cap;
    conn->pending[tail] = *ev;
    conn->pending_count++;
    return 0;
}

int sprot_internal_pending_pop(sprot_connection_t *conn, sprot_event_t *out) {
    if (conn == NULL || out == NULL || conn->pending_count == 0) return 0;
    *out = conn->pending[conn->pending_head];
    int cap = (int)(sizeof(conn->pending)/sizeof(conn->pending[0]));
    conn->pending_head = (conn->pending_head + 1) % cap;
    conn->pending_count--;
    return 1;
}

static int memfd_anon(const char *name, size_t size) {
    int fd = (int)syscall(SYS_memfd_create, name, 0);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

sprot_connection_t *sprot_connect(const char *socket_path) {
    struct sockaddr_un addr;
    sprot_connection_t *c;

    if (socket_path == NULL || socket_path[0] == '\0') {
        socket_path = SPROT_DEFAULT_SOCKET;
    }

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        set_err("sprot: out of memory");
        return NULL;
    }
    c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0) {
        set_err("sprot: socket: %s", strerror(errno));
        free(c);
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        set_err("sprot: connect %s: %s", socket_path, strerror(errno));
        close(c->fd);
        free(c);
        return NULL;
    }

    c->serial = 1;
    c->next_handle = 1;

    sprot_header_t hdr = {
        .type = SPROT_REQ_HELLO,
        .flags = 0,
        .length = 0,
        .object_id = 0,
        .serial = c->serial++,
    };
    sprot_body_hello_t hello = {
        .version_major = SPROT_VERSION_MAJOR,
        .version_minor = SPROT_VERSION_MINOR,
    };
    if (sprot_send_message(c->fd, &hdr, &hello, sizeof(hello), -1) != 0) {
        set_err("sprot: send HELLO: %s", strerror(errno));
        close(c->fd);
        free(c);
        return NULL;
    }

    sprot_header_t rhdr;
    sprot_body_welcome_t welcome;
    if (sprot_recv_message(c->fd, &rhdr, &welcome, sizeof(welcome), NULL) != 0) {
        set_err("sprot: recv WELCOME: %s", strerror(errno));
        close(c->fd);
        free(c);
        return NULL;
    }
    if (rhdr.type != SPROT_EVT_WELCOME) {
        set_err("sprot: expected WELCOME, got %s", sprot_msg_type_name(rhdr.type));
        close(c->fd);
        free(c);
        return NULL;
    }
    c->display_width = welcome.display_width;
    c->display_height = welcome.display_height;
    c->version_major = welcome.version_major;
    c->version_minor = welcome.version_minor;
    c->welcomed = 1;
    return c;
}

void sprot_disconnect(sprot_connection_t *conn) {
    if (conn == NULL) return;
    while (conn->surfaces != NULL) {
        sprot_destroy_surface(conn->surfaces);
    }
    if (conn->fd >= 0) close(conn->fd);
    free(conn);
}

int sprot_connection_fd(const sprot_connection_t *conn) {
    return conn != NULL ? conn->fd : -1;
}

uint32_t sprot_display_width(const sprot_connection_t *conn) {
    return conn != NULL ? conn->display_width : 0;
}

uint32_t sprot_display_height(const sprot_connection_t *conn) {
    return conn != NULL ? conn->display_height : 0;
}

sprot_surface_t *sprot_create_surface(sprot_connection_t *conn, uint32_t width, uint32_t height) {
    if (conn == NULL || width == 0 || height == 0) {
        set_err("sprot: bad args");
        return NULL;
    }
    sprot_surface_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_err("sprot: out of memory");
        return NULL;
    }
    s->conn = conn;
    s->width = width;
    s->height = height;
    s->stride = width * 4u;
    s->size = (size_t)s->stride * height;
    s->fd = memfd_anon("sprot-buf", s->size);
    if (s->fd < 0) {
        set_err("sprot: memfd_create: %s", strerror(errno));
        free(s);
        return NULL;
    }
    s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->map == MAP_FAILED) {
        set_err("sprot: mmap: %s", strerror(errno));
        close(s->fd);
        free(s);
        return NULL;
    }
    memset(s->map, 0, s->size);

    s->client_handle = conn->next_handle++;

    sprot_header_t hdr = {
        .type = SPROT_REQ_SURFACE_CREATE,
        .serial = conn->serial++,
    };
    sprot_body_surface_create_t body = {
        .width = width,
        .height = height,
        .format = SPROT_PIXEL_FORMAT_BGRA8888,
        .client_handle = s->client_handle,
    };
    if (sprot_send_message(conn->fd, &hdr, &body, sizeof(body), -1) != 0) {
        set_err("sprot: send SURFACE_CREATE: %s", strerror(errno));
        munmap(s->map, s->size);
        close(s->fd);
        free(s);
        return NULL;
    }
    s->next = conn->surfaces;
    conn->surfaces = s;
    return s;
}

void sprot_destroy_surface(sprot_surface_t *surface) {
    if (surface == NULL) return;
    sprot_connection_t *conn = surface->conn;
    if (conn != NULL && conn->fd >= 0 && surface->id != 0) {
        sprot_header_t hdr = {
            .type = SPROT_REQ_SURFACE_DESTROY,
            .object_id = surface->id,
            .serial = conn->serial++,
        };
        sprot_send_message(conn->fd, &hdr, NULL, 0, -1);
    }
    if (conn != NULL) {
        sprot_surface_t **link = &conn->surfaces;
        while (*link != NULL) {
            if (*link == surface) {
                *link = surface->next;
                break;
            }
            link = &(*link)->next;
        }
    }
    if (surface->map != NULL && surface->map != MAP_FAILED) {
        munmap(surface->map, surface->size);
    }
    if (surface->fd >= 0) close(surface->fd);
    free(surface);
}

uint32_t *sprot_surface_pixels(sprot_surface_t *surface) {
    return surface != NULL ? (uint32_t *)surface->map : NULL;
}

uint32_t sprot_surface_width(const sprot_surface_t *surface) {
    return surface != NULL ? surface->width : 0;
}

uint32_t sprot_surface_height(const sprot_surface_t *surface) {
    return surface != NULL ? surface->height : 0;
}

uint32_t sprot_surface_stride(const sprot_surface_t *surface) {
    return surface != NULL ? surface->stride : 0;
}

uint32_t sprot_surface_id(const sprot_surface_t *surface) {
    return surface != NULL ? surface->id : 0;
}

int sprot_commit(sprot_surface_t *surface) {
    if (surface == NULL || surface->conn == NULL) {
        set_err("sprot: bad surface");
        return -1;
    }
    if (surface->id == 0) {
        set_err("sprot: surface not yet created by server");
        return -1;
    }
    sprot_connection_t *conn = surface->conn;
    if (!surface->attached) {
        if (sprot_attach_fd(surface,
                            surface->fd,
                            surface->width,
                            surface->height,
                            surface->stride,
                            (uint32_t)surface->size,
                            SPROT_BUFFER_SHM,
                            SPROT_PIXEL_FORMAT_BGRA8888) != 0) {
            return -1;
        }
    }
    sprot_header_t hdr = {
        .type = SPROT_REQ_SURFACE_COMMIT,
        .object_id = surface->id,
        .serial = conn->serial++,
    };
    if (sprot_send_message(conn->fd, &hdr, NULL, 0, -1) != 0) {
        set_err("sprot: send SURFACE_COMMIT: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int sprot_resize_surface(sprot_surface_t *surface, uint32_t width, uint32_t height) {
    if (surface == NULL || surface->conn == NULL || width == 0 || height == 0) {
        set_err("sprot: bad resize args");
        return -1;
    }
    if (surface->width == width && surface->height == height) {
        return 0;
    }

    uint32_t new_stride = width * 4u;
    size_t new_size = (size_t)new_stride * height;
    int new_fd = memfd_anon("sprot-buf", new_size);
    if (new_fd < 0) {
        set_err("sprot: resize memfd_create: %s", strerror(errno));
        return -1;
    }
    void *new_map = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, new_fd, 0);
    if (new_map == MAP_FAILED) {
        set_err("sprot: resize mmap: %s", strerror(errno));
        close(new_fd);
        return -1;
    }
    memset(new_map, 0, new_size);

    if (surface->map != NULL && surface->map != MAP_FAILED) {
        munmap(surface->map, surface->size);
    }
    if (surface->fd >= 0) close(surface->fd);

    surface->fd = new_fd;
    surface->map = new_map;
    surface->width = width;
    surface->height = height;
    surface->stride = new_stride;
    surface->size = new_size;
    surface->attached = 0;
    return 0;
}

int sprot_attach_fd(
    sprot_surface_t *surface,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t buffer_size,
    uint32_t buffer_kind,
    uint32_t format
) {
    sprot_header_t hdr;
    sprot_body_surface_attach_buffer_t body;

    if (surface == NULL || surface->conn == NULL || surface->id == 0 || fd < 0) {
        set_err("sprot: bad attach args");
        return -1;
    }

    hdr.type = SPROT_REQ_SURFACE_ATTACH_BUFFER;
    hdr.object_id = surface->id;
    hdr.serial = surface->conn->serial++;
    hdr.flags = 0;
    hdr.length = 0;

    body.width = width;
    body.height = height;
    body.stride = stride;
    body.buffer_size = buffer_size;
    body.buffer_kind = buffer_kind;
    body.format = format;

    if (sprot_send_message(surface->conn->fd, &hdr, &body, sizeof(body), fd) != 0) {
        set_err("sprot: send SURFACE_ATTACH_BUFFER: %s", strerror(errno));
        return -1;
    }

    surface->attached = 1;
    surface->attach_kind = buffer_kind;
    surface->attach_format = format;
    return 0;
}

int sprot_damage(sprot_surface_t *surface, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (surface == NULL || surface->conn == NULL || surface->id == 0) {
        return -1;
    }
    sprot_header_t hdr = {
        .type = SPROT_REQ_SURFACE_DAMAGE,
        .object_id = surface->id,
        .serial = surface->conn->serial++,
    };
    sprot_body_surface_damage_t body = { .x = x, .y = y, .width = w, .height = h };
    return sprot_send_message(surface->conn->fd, &hdr, &body, sizeof(body), -1);
}

int sprot_request_frame(sprot_surface_t *surface) {
    if (surface == NULL || surface->conn == NULL || surface->id == 0) {
        return -1;
    }
    sprot_header_t hdr = {
        .type = SPROT_REQ_SURFACE_FRAME,
        .object_id = surface->id,
        .serial = surface->conn->serial++,
    };
    return sprot_send_message(surface->conn->fd, &hdr, NULL, 0, -1);
}

int sprot_set_title(sprot_surface_t *surface, const char *title) {
    if (surface == NULL || surface->conn == NULL || surface->id == 0) {
        return -1;
    }
    if (title == NULL) title = "";
    size_t tlen = strlen(title);
    if (tlen > SPROT_MAX_TITLE) tlen = SPROT_MAX_TITLE;
    uint8_t buf[sizeof(sprot_body_set_title_t) + SPROT_MAX_TITLE];
    sprot_body_set_title_t hdr_body = { .length = (uint32_t)tlen };
    memcpy(buf, &hdr_body, sizeof(hdr_body));
    memcpy(buf + sizeof(hdr_body), title, tlen);
    sprot_header_t hdr = {
        .type = SPROT_REQ_SURFACE_SET_TITLE,
        .object_id = surface->id,
        .serial = surface->conn->serial++,
    };
    return sprot_send_message(surface->conn->fd, &hdr, buf, sizeof(hdr_body) + tlen, -1);
}

int sprot_set_role(sprot_surface_t *surface, uint32_t role, uint32_t parent_id, int32_t x, int32_t y) {
    if (surface == NULL || surface->conn == NULL || surface->id == 0) {
        return -1;
    }
    sprot_header_t hdr = {
        .type = SPROT_REQ_SURFACE_SET_ROLE,
        .object_id = surface->id,
        .serial = surface->conn->serial++,
    };
    sprot_body_surface_set_role_t body = {
        .role = role,
        .parent_id = parent_id,
        .x = x,
        .y = y,
    };
    return sprot_send_message(surface->conn->fd, &hdr, &body, sizeof(body), -1);
}

int sprot_set_cursor(sprot_surface_t *surface, uint32_t cursor_type) {
    if (surface == NULL || surface->conn == NULL || surface->id == 0) {
        return -1;
    }
    sprot_header_t hdr = {
        .type = SPROT_REQ_SET_CURSOR,
        .object_id = surface->id,
        .serial = surface->conn->serial++,
    };
    sprot_body_set_cursor_t body = {
        .cursor_type = cursor_type,
    };
    return sprot_send_message(surface->conn->fd, &hdr, &body, sizeof(body), -1);
}

int sprot_set_cursor_image(
    sprot_surface_t *surface,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t buffer_size,
    int32_t hotspot_x,
    int32_t hotspot_y,
    uint32_t visible
) {
    if (surface == NULL || surface->conn == NULL || surface->id == 0) {
        return -1;
    }
    if (visible && (fd < 0 || width == 0 || height == 0 || stride < width * 4u ||
                    buffer_size < stride * height)) {
        return -1;
    }
    sprot_header_t hdr = {
        .type = SPROT_REQ_SET_CURSOR_IMAGE,
        .object_id = surface->id,
        .serial = surface->conn->serial++,
    };
    sprot_body_set_cursor_image_t body = {
        .width = width,
        .height = height,
        .stride = stride,
        .buffer_size = buffer_size,
        .hotspot_x = hotspot_x,
        .hotspot_y = hotspot_y,
        .visible = visible ? 1u : 0u,
        .format = SPROT_PIXEL_FORMAT_BGRA8888,
    };
    return sprot_send_message(surface->conn->fd, &hdr, &body, sizeof(body), visible ? fd : -1);
}

int sprot_ping(sprot_connection_t *conn, uint32_t serial) {
    if (conn == NULL) return -1;
    sprot_header_t hdr = { .type = SPROT_REQ_PING, .serial = serial };
    return sprot_send_message(conn->fd, &hdr, NULL, 0, -1);
}

static void apply_surface_created(sprot_connection_t *conn, const sprot_body_surface_created_t *body) {
    for (sprot_surface_t *s = conn->surfaces; s != NULL; s = s->next) {
        if (s->client_handle == body->client_handle) {
            s->id = body->surface_id;
            return;
        }
    }
}

int sprot_poll_event(sprot_connection_t *conn, sprot_event_t *out_event, int timeout_ms) {
    struct pollfd pfd;
    sprot_header_t hdr;
    uint8_t body[SPROT_MAX_MESSAGE];
    int fd_in_msg = -1;

    if (conn == NULL || out_event == NULL) {
        set_err("sprot: bad args");
        return -1;
    }
    memset(out_event, 0, sizeof(*out_event));

    if (sprot_internal_pending_pop(conn, out_event)) {
        return 1;
    }

    pfd.fd = conn->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;
        set_err("sprot: poll: %s", strerror(errno));
        return -1;
    }
    if (pr == 0) {
        return 0;
    }
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
        out_event->kind = SPROT_EVENT_DISCONNECT;
        return 1;
    }

    if (sprot_recv_message(conn->fd, &hdr, body, sizeof(body), &fd_in_msg) != 0) {
        set_err("sprot: recv: %s", strerror(errno));
        out_event->kind = SPROT_EVENT_DISCONNECT;
        return 1;
    }
    if (fd_in_msg >= 0) close(fd_in_msg);

    out_event->serial = hdr.serial;
    out_event->object_id = hdr.object_id;

    switch (hdr.type) {
        case SPROT_EVT_WELCOME: {
            if (hdr.length - sizeof(hdr) >= sizeof(sprot_body_welcome_t)) {
                memcpy(&out_event->u.welcome, body, sizeof(sprot_body_welcome_t));
            }
            out_event->kind = SPROT_EVENT_WELCOME;
            break;
        }
        case SPROT_EVT_SURFACE_CREATED: {
            sprot_body_surface_created_t b;
            memcpy(&b, body, sizeof(b));
            apply_surface_created(conn, &b);
            out_event->u.surface_created = b;
            out_event->kind = SPROT_EVENT_SURFACE_CREATED;
            break;
        }
        case SPROT_EVT_POINTER_MOTION:
            memcpy(&out_event->u.pointer_motion, body, sizeof(sprot_body_pointer_motion_t));
            out_event->kind = SPROT_EVENT_POINTER_MOTION;
            break;
        case SPROT_EVT_POINTER_BUTTON:
            memcpy(&out_event->u.pointer_button, body, sizeof(sprot_body_pointer_button_t));
            out_event->kind = SPROT_EVENT_POINTER_BUTTON;
            break;
        case SPROT_EVT_POINTER_ENTER:
            out_event->kind = SPROT_EVENT_POINTER_ENTER;
            break;
        case SPROT_EVT_POINTER_LEAVE:
            out_event->kind = SPROT_EVENT_POINTER_LEAVE;
            break;
        case SPROT_EVT_POINTER_AXIS:
            if (hdr.length - sizeof(hdr) >= sizeof(sprot_body_pointer_axis_t)) {
                memcpy(&out_event->u.pointer_axis, body, sizeof(sprot_body_pointer_axis_t));
            }
            out_event->kind = SPROT_EVENT_POINTER_AXIS;
            break;
        case SPROT_EVT_KEY:
            memcpy(&out_event->u.key, body, sizeof(sprot_body_key_t));
            out_event->kind = SPROT_EVENT_KEY;
            break;
        case SPROT_EVT_SURFACE_CONFIGURE:
            memcpy(&out_event->u.configure, body, sizeof(sprot_body_configure_t));
            out_event->kind = SPROT_EVENT_SURFACE_CONFIGURE;
            break;
        case SPROT_EVT_SURFACE_CLOSE:
            out_event->kind = SPROT_EVENT_SURFACE_CLOSE;
            break;
        case SPROT_EVT_SURFACE_FRAME:
            if (hdr.length - sizeof(hdr) >= sizeof(sprot_body_frame_t)) {
                memcpy(&out_event->u.frame, body, sizeof(sprot_body_frame_t));
            }
            out_event->kind = SPROT_EVENT_SURFACE_FRAME;
            break;
        case SPROT_EVT_RENDER_NODE:
            if (hdr.length - sizeof(hdr) >= sizeof(sprot_body_render_node_t)) {
                memcpy(&out_event->u.render_node, body, sizeof(sprot_body_render_node_t));
            }
            out_event->kind = SPROT_EVENT_RENDER_NODE;
            break;
        case SPROT_EVT_PONG:
            out_event->kind = SPROT_EVENT_PONG;
            break;
        case SPROT_EVT_ERROR: {
            sprot_body_error_t e;
            memcpy(&e, body, sizeof(e));
            out_event->u.error.code = e.code;
            size_t msg_len = e.length < sizeof(out_event->u.error.message) - 1
                ? e.length : sizeof(out_event->u.error.message) - 1;
            if (sizeof(e) + msg_len <= sizeof(body)) {
                memcpy(out_event->u.error.message, body + sizeof(e), msg_len);
            }
            out_event->u.error.message[msg_len] = '\0';
            out_event->kind = SPROT_EVENT_ERROR;
            break;
        }
        default:
            out_event->kind = SPROT_EVENT_NONE;
            break;
    }
    return 1;
}
