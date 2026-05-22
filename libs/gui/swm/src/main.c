#define _GNU_SOURCE
#include <swm/swm.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static swm_state_t g_swm;
static volatile sig_atomic_t g_signal_quit = 0;

static void on_signal(int sig) {
    (void)sig;
    g_signal_quit = 1;
}

static void logf_(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[swm] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static swm_surface_t *alloc_surface(void) {
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (!g_swm.surfaces[i].in_use) {
            memset(&g_swm.surfaces[i], 0, sizeof(g_swm.surfaces[i]));
            g_swm.surfaces[i].in_use = 1;
            g_swm.surfaces[i].buf_fd = -1;
            return &g_swm.surfaces[i];
        }
    }
    return NULL;
}

static void free_surface(swm_surface_t *s) {
    if (s == NULL || !s->in_use) return;
    if (s->buf_map != NULL && s->buf_map != MAP_FAILED) {
        munmap(s->buf_map, s->buffer_size);
    }
    if (s->buf_fd >= 0) close(s->buf_fd);
    if (s->owner != NULL) {
        for (int i = 0; i < s->owner->surface_count; i++) {
            if (s->owner->surfaces[i] == s) {
                s->owner->surfaces[i] = s->owner->surfaces[s->owner->surface_count - 1];
                s->owner->surface_count--;
                break;
            }
        }
    }
    memset(s, 0, sizeof(*s));
}

static swm_surface_t *find_surface(uint32_t id) {
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (g_swm.surfaces[i].in_use && g_swm.surfaces[i].id == id) {
            return &g_swm.surfaces[i];
        }
    }
    return NULL;
}

static int send_event(int fd, uint16_t type, uint32_t object_id, uint32_t serial, const void *body, size_t body_len) {
    sprot_header_t hdr = {
        .type = type,
        .object_id = object_id,
        .serial = serial,
    };
    return sprot_send_message(fd, &hdr, body, body_len, -1);
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

static void drop_client(swm_client_t *c, const char *reason) {
    if (c == NULL || !c->in_use) return;
    logf_("client fd=%d dropped: %s", c->sock, reason);
    for (int i = 0; i < c->surface_count; i++) {
        free_surface(c->surfaces[i]);
    }
    if (c->sock >= 0) close(c->sock);
    memset(c, 0, sizeof(*c));
}

static swm_client_t *alloc_client(int sock) {
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (!g_swm.clients[i].in_use) {
            memset(&g_swm.clients[i], 0, sizeof(g_swm.clients[i]));
            g_swm.clients[i].in_use = 1;
            g_swm.clients[i].sock = sock;
            return &g_swm.clients[i];
        }
    }
    return NULL;
}

static int setup_socket(const char *path) {
    struct sockaddr_un addr;
    int fd;

    if (path == NULL || path[0] == '\0') {
        path = SPROT_DEFAULT_SOCKET;
    }
    snprintf(g_swm.socket_path, sizeof(g_swm.socket_path), "%s", path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        logf_("socket: %s", strerror(errno));
        return -1;
    }
    unlink(path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        logf_("bind %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (chmod(path, 0666) != 0) {
        logf_("chmod %s: %s (continuing)", path, strerror(errno));
    }
    if (listen(fd, 8) != 0) {
        logf_("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    logf_("listening on %s", path);
    return fd;
}

static int handle_hello(swm_client_t *c, const sprot_header_t *hdr, const void *body, size_t blen) {
    (void)hdr;
    if (blen < sizeof(sprot_body_hello_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short HELLO");
        return -1;
    }
    sprot_body_welcome_t welcome = {
        .display_width = g_swm.display_w,
        .display_height = g_swm.display_h,
        .version_major = SPROT_VERSION_MAJOR,
        .version_minor = SPROT_VERSION_MINOR,
    };
    if (send_event(c->sock, SPROT_EVT_WELCOME, 0, hdr->serial, &welcome, sizeof(welcome)) != 0) {
        return -1;
    }
    c->has_hello = 1;
    return 0;
}

static int handle_surface_create(swm_client_t *c, const sprot_header_t *hdr, const void *body, size_t blen) {
    if (blen < sizeof(sprot_body_surface_create_t)) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "short SURFACE_CREATE");
        return -1;
    }
    sprot_body_surface_create_t b;
    memcpy(&b, body, sizeof(b));
    if (b.format != SPROT_PIXEL_FORMAT_BGRA8888 || b.width == 0 || b.height == 0 ||
        b.width > g_swm.display_w * 2 || b.height > g_swm.display_h * 2) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface dims");
        return -1;
    }
    if (c->surface_count >= (int)(sizeof(c->surfaces)/sizeof(c->surfaces[0]))) {
        send_error(c->sock, SPROT_ERROR_LIMIT, "surface limit per client");
        return -1;
    }
    swm_surface_t *s = alloc_surface();
    if (s == NULL) {
        send_error(c->sock, SPROT_ERROR_LIMIT, "server surface table full");
        return -1;
    }
    s->id = ++g_swm.next_surface_id;
    s->client_handle = b.client_handle;
    s->owner = c;
    s->width = b.width;
    s->height = b.height;
    s->stride = b.width * 4u;
    s->buffer_size = (size_t)s->stride * b.height;
    s->pos_x = g_swm.next_cascade_x;
    s->pos_y = g_swm.next_cascade_y;
    g_swm.next_cascade_x += 32;
    g_swm.next_cascade_y += 32;
    if (g_swm.next_cascade_x + 100 > (int32_t)g_swm.display_w) g_swm.next_cascade_x = 32;
    if (g_swm.next_cascade_y + 100 > (int32_t)g_swm.display_h) g_swm.next_cascade_y = 32;

    c->surfaces[c->surface_count++] = s;

    sprot_body_surface_created_t resp = { .surface_id = s->id, .client_handle = s->client_handle };
    if (send_event(c->sock, SPROT_EVT_SURFACE_CREATED, s->id, hdr->serial, &resp, sizeof(resp)) != 0) {
        return -1;
    }
    logf_("surface %u created %ux%u for client fd=%d at (%d,%d)",
          s->id, s->width, s->height, c->sock, s->pos_x, s->pos_y);
    return 0;
}

static int handle_surface_attach(swm_client_t *c, const sprot_header_t *hdr, const void *body, size_t blen, int incoming_fd) {
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
    swm_surface_t *s = find_surface(hdr->object_id);
    if (s == NULL || s->owner != c) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if (b.width != s->width || b.height != s->height || b.stride != s->stride ||
        b.buffer_size != s->buffer_size) {
        close(incoming_fd);
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "ATTACH dims mismatch");
        return -1;
    }
    if (s->buf_map != NULL && s->buf_map != MAP_FAILED) {
        munmap(s->buf_map, s->buffer_size);
        s->buf_map = NULL;
    }
    if (s->buf_fd >= 0) {
        close(s->buf_fd);
        s->buf_fd = -1;
    }
    s->buf_fd = incoming_fd;
    s->buf_map = mmap(NULL, s->buffer_size, PROT_READ, MAP_SHARED, incoming_fd, 0);
    if (s->buf_map == MAP_FAILED) {
        s->buf_map = NULL;
        close(s->buf_fd);
        s->buf_fd = -1;
        send_error(c->sock, SPROT_ERROR_OUT_OF_MEMORY, "mmap failed");
        return -1;
    }
    s->has_pending = 1;
    return 0;
}

static int handle_surface_commit(swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = find_surface(hdr->object_id);
    if (s == NULL || s->owner != c) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "bad surface id");
        return -1;
    }
    if (s->buf_map == NULL) {
        send_error(c->sock, SPROT_ERROR_INVALID_ARG, "commit without attached buffer");
        return -1;
    }
    s->committed = 1;
    s->has_pending = 0;
    return 0;
}

static int handle_surface_destroy(swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = find_surface(hdr->object_id);
    if (s == NULL || s->owner != c) {
        return 0;
    }
    logf_("surface %u destroyed", s->id);
    free_surface(s);
    return 0;
}

static int handle_ping(swm_client_t *c, const sprot_header_t *hdr) {
    return send_event(c->sock, SPROT_EVT_PONG, 0, hdr->serial, NULL, 0);
}

static int dispatch_message(swm_client_t *c) {
    sprot_header_t hdr;
    uint8_t body[SPROT_MAX_MESSAGE];
    int incoming_fd = -1;

    int r = sprot_recv_message(c->sock, &hdr, body, sizeof(body), &incoming_fd);
    if (r != 0) {
        drop_client(c, strerror(errno));
        return -1;
    }
    size_t blen = hdr.length >= sizeof(hdr) ? hdr.length - sizeof(hdr) : 0;

    if (!c->has_hello && hdr.type != SPROT_REQ_HELLO) {
        send_error(c->sock, SPROT_ERROR_PROTOCOL, "must HELLO first");
        if (incoming_fd >= 0) close(incoming_fd);
        drop_client(c, "no HELLO");
        return -1;
    }

    int rc = 0;
    switch (hdr.type) {
        case SPROT_REQ_HELLO:           rc = handle_hello(c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_CREATE:  rc = handle_surface_create(c, &hdr, body, blen); break;
        case SPROT_REQ_SURFACE_ATTACH:  rc = handle_surface_attach(c, &hdr, body, blen, incoming_fd);
                                        incoming_fd = -1; break;
        case SPROT_REQ_SURFACE_COMMIT:  rc = handle_surface_commit(c, &hdr); break;
        case SPROT_REQ_SURFACE_DESTROY: rc = handle_surface_destroy(c, &hdr); break;
        case SPROT_REQ_SURFACE_DAMAGE:  /* no-op v1, full-frame composite */ break;
        case SPROT_REQ_PING:            rc = handle_ping(c, &hdr); break;
        default:
            logf_("unknown msg type 0x%x from fd=%d", hdr.type, c->sock);
            send_error(c->sock, SPROT_ERROR_PROTOCOL, "unknown message type");
            break;
    }
    if (incoming_fd >= 0) close(incoming_fd);
    return rc;
}

static void composite_surfaces(srapi_framebuffer_t *fb) {
    uint32_t *dst = srapi_framebuffer_pixels(fb);
    int32_t dst_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t dst_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t dst_pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
    if (dst == NULL) return;

    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &g_swm.surfaces[i];
        if (!s->in_use || !s->committed || s->buf_map == NULL) continue;
        int32_t sx0 = 0, sy0 = 0;
        int32_t w = (int32_t)s->width;
        int32_t h = (int32_t)s->height;
        int32_t dx = s->pos_x;
        int32_t dy = s->pos_y;
        if (dx < 0) { sx0 = -dx; w -= sx0; dx = 0; }
        if (dy < 0) { sy0 = -dy; h -= sy0; dy = 0; }
        if (dx + w > dst_w) w = dst_w - dx;
        if (dy + h > dst_h) h = dst_h - dy;
        if (w <= 0 || h <= 0) continue;
        int32_t src_pitch_px = (int32_t)(s->stride / 4u);
        const uint32_t *src = (const uint32_t *)s->buf_map;
        for (int32_t row = 0; row < h; row++) {
            const uint32_t *sr = src + (sy0 + row) * src_pitch_px + sx0;
            uint32_t *dr = dst + (dy + row) * dst_pitch_px + dx;
            memcpy(dr, sr, (size_t)w * 4u);
        }
    }
}

static swm_surface_t *surface_under(int32_t x, int32_t y) {
    swm_surface_t *hit = NULL;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &g_swm.surfaces[i];
        if (!s->in_use || !s->committed) continue;
        if (x >= s->pos_x && x < s->pos_x + (int32_t)s->width &&
            y >= s->pos_y && y < s->pos_y + (int32_t)s->height) {
            hit = s;
        }
    }
    return hit;
}

static void forward_input(const srapi_input_event_t *ev) {
    switch (ev->type) {
        case SRAPI_INPUT_EVENT_MOUSE_MOTION: {
            g_swm.mouse_x = ev->mouse_motion.x;
            g_swm.mouse_y = ev->mouse_motion.y;
            swm_surface_t *s = surface_under(g_swm.mouse_x, g_swm.mouse_y);
            if (s != NULL && s->owner != NULL) {
                sprot_body_pointer_motion_t body = {
                    .x = g_swm.mouse_x - s->pos_x,
                    .y = g_swm.mouse_y - s->pos_y,
                };
                send_event(s->owner->sock, SPROT_EVT_POINTER_MOTION, s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN:
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP: {
            swm_surface_t *s = surface_under(g_swm.mouse_x, g_swm.mouse_y);
            if (s != NULL && s->owner != NULL) {
                sprot_body_pointer_button_t body = {
                    .button = ev->mouse_button.button,
                    .state = ev->type == SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN
                             ? SPROT_BUTTON_STATE_PRESSED : SPROT_BUTTON_STATE_RELEASED,
                };
                send_event(s->owner->sock, SPROT_EVT_POINTER_BUTTON, s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_KEY_DOWN:
        case SRAPI_INPUT_EVENT_KEY_UP: {
            if (ev->type == SRAPI_INPUT_EVENT_KEY_DOWN &&
                ev->key.scancode == SRAPI_SCANCODE_ESCAPE) {
                g_swm.should_quit = 1;
                break;
            }
            swm_surface_t *s = surface_under(g_swm.mouse_x, g_swm.mouse_y);
            if (s != NULL && s->owner != NULL) {
                sprot_body_key_t body = {
                    .scancode = ev->key.scancode,
                    .state = ev->type == SRAPI_INPUT_EVENT_KEY_DOWN
                             ? SPROT_KEY_STATE_PRESSED : SPROT_KEY_STATE_RELEASED,
                    .modifiers = ev->key.modifiers,
                };
                send_event(s->owner->sock, SPROT_EVT_KEY, s->id, 0, &body, sizeof(body));
            }
            break;
        }
        default:
            break;
    }
}

static void usage(const char *argv0) {
    printf("swm - simple window manager / compositor (sprot v%d.%d)\n",
           SPROT_VERSION_MAJOR, SPROT_VERSION_MINOR);
    printf("usage: %s [--socket /path/swm.sock] [--debug] [--bg RRGGBB]\n", argv0);
    printf("press Esc inside the compositor TTY to quit.\n");
}

static uint32_t parse_hex_color(const char *s, uint32_t fallback) {
    if (s == NULL) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == NULL || *end != '\0' || v > 0xFFFFFFu) return fallback;
    return 0xFF000000u | (uint32_t)v;
}

int main(int argc, char *argv[]) {
    const char *socket_path = SPROT_DEFAULT_SOCKET;
    int debug = 0;
    uint32_t bg_color = 0xFF101418u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "--bg") == 0 && i + 1 < argc) {
            bg_color = parse_hex_color(argv[++i], bg_color);
        }
    }
    if (debug) srapi = 1;

    memset(&g_swm, 0, sizeof(g_swm));
    for (int i = 0; i < SWM_MAX_SURFACES; i++) g_swm.surfaces[i].buf_fd = -1;
    g_swm.next_cascade_x = 64;
    g_swm.next_cascade_y = 64;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    srapi_drm_recommendation_t rec;
    if (srapi_drm_recommend(&rec) != SRAPI_OK) {
        logf_("drm recommend: %s", srapi_last_error());
        return 1;
    }
    if (srapi_drm_open_display(&(srapi_drm_display_desc_t){ .device_path = rec.path }, &g_swm.drm) != SRAPI_OK) {
        logf_("drm open: %s", srapi_last_error());
        return 1;
    }
    g_swm.display_w = srapi_drm_width(g_swm.drm);
    g_swm.display_h = srapi_drm_height(g_swm.drm);
    g_swm.mouse_x = (int32_t)g_swm.display_w / 2;
    g_swm.mouse_y = (int32_t)g_swm.display_h / 2;
    logf_("display %ux%u", g_swm.display_w, g_swm.display_h);

    if (srapi_create_context(&(srapi_context_desc_t){
            .width = g_swm.display_w, .height = g_swm.display_h,
            .backend = SRAPI_BACKEND_CPU,
        }, &g_swm.srctx) != SRAPI_OK) {
        logf_("create context: %s", srapi_last_error());
        srapi_drm_close(g_swm.drm);
        return 1;
    }
    if (srapi_create_cmd_buffer(g_swm.srctx, &g_swm.cmd) != SRAPI_OK) {
        logf_("create cmd: %s", srapi_last_error());
        srapi_destroy_context(g_swm.srctx);
        srapi_drm_close(g_swm.drm);
        return 1;
    }
    if (srapi_input_create(&(srapi_input_desc_t){
            .auto_discover = 1,
            .initial_mouse_x = g_swm.mouse_x,
            .initial_mouse_y = g_swm.mouse_y,
        }, &g_swm.input) != SRAPI_OK) {
        logf_("input: %s (continuing without input)", srapi_last_error());
        g_swm.input = NULL;
    } else {
        srapi_input_set_bounds(g_swm.input, (int32_t)g_swm.display_w, (int32_t)g_swm.display_h);
    }

    g_swm.listen_fd = setup_socket(socket_path);
    if (g_swm.listen_fd < 0) {
        if (g_swm.input != NULL) srapi_input_destroy(g_swm.input);
        srapi_destroy_cmd_buffer(g_swm.cmd);
        srapi_destroy_context(g_swm.srctx);
        srapi_drm_close(g_swm.drm);
        return 1;
    }

    logf_("ready. press Esc to quit.");
    while (!g_swm.should_quit && !g_signal_quit) {
        struct pollfd pfds[1 + SWM_MAX_CLIENTS];
        int nfds = 0;
        pfds[nfds].fd = g_swm.listen_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
        int client_indices[SWM_MAX_CLIENTS];
        for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
            if (g_swm.clients[i].in_use) {
                pfds[nfds].fd = g_swm.clients[i].sock;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                client_indices[nfds - 1] = i;
                nfds++;
            }
        }

        int pr = poll(pfds, (nfds_t)nfds, 8);
        if (pr < 0) {
            if (errno == EINTR) continue;
            logf_("poll: %s", strerror(errno));
            break;
        }
        if (pr > 0) {
            if (pfds[0].revents & POLLIN) {
                int cfd = accept4(g_swm.listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (cfd >= 0) {
                    swm_client_t *c = alloc_client(cfd);
                    if (c == NULL) {
                        logf_("client limit reached, rejecting");
                        close(cfd);
                    } else {
                        logf_("client connected fd=%d", cfd);
                    }
                }
            }
            for (int i = 1; i < nfds; i++) {
                if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    swm_client_t *c = &g_swm.clients[client_indices[i - 1]];
                    if (c->in_use) {
                        if (pfds[i].revents & POLLIN) {
                            dispatch_message(c);
                        } else {
                            drop_client(c, "hup");
                        }
                    }
                }
            }
        }

        if (g_swm.input != NULL) {
            srapi_input_event_t ev;
            while (srapi_input_poll(g_swm.input, &ev) == 1) {
                forward_input(&ev);
                if (g_swm.should_quit) break;
            }
        }
        if (g_swm.should_quit) break;

        srapi_cmd_reset(g_swm.cmd);
        srapi_cmd_emit(g_swm.cmd, &(srapi_command_t){
            .kind = SRAPI_COMMAND_CLEAR,
            .color = bg_color,
        });
        srapi_framebuffer_t *fb = srapi_drm_backbuffer(g_swm.drm);
        srapi_submit(g_swm.srctx, fb, g_swm.cmd);
        composite_surfaces(fb);
        srapi_drm_present(g_swm.drm);
    }

    logf_("shutting down");
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (g_swm.clients[i].in_use) {
            drop_client(&g_swm.clients[i], "shutdown");
        }
    }
    if (g_swm.listen_fd >= 0) close(g_swm.listen_fd);
    if (g_swm.socket_path[0] != '\0') unlink(g_swm.socket_path);
    if (g_swm.input != NULL) srapi_input_destroy(g_swm.input);
    srapi_destroy_cmd_buffer(g_swm.cmd);
    srapi_destroy_context(g_swm.srctx);
    srapi_drm_close(g_swm.drm);
    return 0;
}
