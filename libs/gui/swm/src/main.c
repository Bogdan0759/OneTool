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
    s->z = ++g_swm.next_z;
    snprintf(s->title, sizeof(s->title), "client #%d", c->sock);

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
    if (g_swm.grab_surface == s) {
        g_swm.grab_surface = NULL;
        g_swm.interaction = SWM_INTERACT_NONE;
    }
    free_surface(s);
    return 0;
}

static int handle_surface_set_title(swm_client_t *c, const sprot_header_t *hdr, const void *body, size_t blen) {
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
    swm_surface_t *s = find_surface(hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    if (b.length > sizeof(s->title) - 1) b.length = sizeof(s->title) - 1;
    memcpy(s->title, (const uint8_t *)body + sizeof(b), b.length);
    s->title[b.length] = '\0';
    return 0;
}

static int handle_surface_frame(swm_client_t *c, const sprot_header_t *hdr) {
    swm_surface_t *s = find_surface(hdr->object_id);
    if (s == NULL || s->owner != c) return 0;
    s->wants_frame = 1;
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
        case SPROT_REQ_SURFACE_FRAME:   rc = handle_surface_frame(c, &hdr); break;
        case SPROT_REQ_SURFACE_SET_TITLE: rc = handle_surface_set_title(c, &hdr, body, blen); break;
        case SPROT_REQ_PING:            rc = handle_ping(c, &hdr); break;
        default:
            logf_("unknown msg type 0x%x from fd=%d", hdr.type, c->sock);
            send_error(c->sock, SPROT_ERROR_PROTOCOL, "unknown message type");
            break;
    }
    if (incoming_fd >= 0) close(incoming_fd);
    return rc;
}

static int z_compare_asc(const void *a, const void *b) {
    const swm_surface_t * const *sa = a;
    const swm_surface_t * const *sb = b;
    if ((*sa)->z < (*sb)->z) return -1;
    if ((*sa)->z > (*sb)->z) return 1;
    return 0;
}

static int collect_surfaces_z_asc(swm_surface_t **out, int max) {
    int n = 0;
    for (int i = 0; i < SWM_MAX_SURFACES && n < max; i++) {
        swm_surface_t *s = &g_swm.surfaces[i];
        if (!s->in_use || !s->committed || s->minimized) continue;
        out[n++] = s;
    }
    qsort(out, (size_t)n, sizeof(*out), z_compare_asc);
    return n;
}

static void raise_surface(swm_surface_t *s) {
    if (s == NULL) return;
    s->z = ++g_swm.next_z;
}

typedef enum {
    SWM_HIT_NONE = 0,
    SWM_HIT_CONTENT,
    SWM_HIT_TITLEBAR,
    SWM_HIT_BTN_MIN,
    SWM_HIT_BTN_MAX,
    SWM_HIT_BTN_CLOSE,
} swm_hit_region_t;

static void surface_effective_rect(const swm_surface_t *s, int32_t *ex, int32_t *ey, int32_t *ew, int32_t *eh) {
    if (s->maximized) {
        *ex = SWM_BORDER;
        *ey = SWM_TITLEBAR_H + SWM_BORDER;
        *ew = (int32_t)g_swm.display_w - 2 * SWM_BORDER;
        *eh = (int32_t)g_swm.display_h - SWM_TITLEBAR_H - 2 * SWM_BORDER;
    } else {
        *ex = s->pos_x;
        *ey = s->pos_y;
        *ew = (int32_t)s->width;
        *eh = (int32_t)s->height;
    }
}

static void surface_outer_rect(const swm_surface_t *s, int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh) {
    int32_t ex, ey, ew, eh;
    surface_effective_rect(s, &ex, &ey, &ew, &eh);
    *ox = ex - SWM_BORDER;
    *oy = ey - SWM_TITLEBAR_H - SWM_BORDER;
    *ow = ew + 2 * SWM_BORDER;
    *oh = eh + SWM_TITLEBAR_H + 2 * SWM_BORDER;
}

static void titlebar_button_rects(const swm_surface_t *s,
                                  int32_t *min_x, int32_t *max_x, int32_t *close_x,
                                  int32_t *btn_y) {
    int32_t outer_x, outer_y, outer_w, outer_h;
    surface_outer_rect(s, &outer_x, &outer_y, &outer_w, &outer_h);
    int32_t bar_right = outer_x + outer_w - SWM_BORDER;
    *close_x = bar_right - 4 - SWM_BTN_SIZE;
    *max_x   = *close_x - 4 - SWM_BTN_SIZE;
    *min_x   = *max_x   - 4 - SWM_BTN_SIZE;
    *btn_y = outer_y + SWM_BORDER + (SWM_TITLEBAR_H - SWM_BTN_SIZE) / 2;
}

static swm_hit_region_t hit_test(int32_t mx, int32_t my, swm_surface_t **out_surface) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = collect_surfaces_z_asc(list, SWM_MAX_SURFACES);
    for (int i = n - 1; i >= 0; i--) {
        swm_surface_t *s = list[i];
        int32_t ox, oy, ow, oh;
        int32_t ex, ey, ew, eh;
        surface_outer_rect(s, &ox, &oy, &ow, &oh);
        surface_effective_rect(s, &ex, &ey, &ew, &eh);
        if (mx < ox || mx >= ox + ow || my < oy || my >= oy + oh) continue;

        if (my >= ey && my < ey + eh && mx >= ex && mx < ex + ew) {
            *out_surface = s;
            return SWM_HIT_CONTENT;
        }
        int32_t bmin, bmax, bclose, by;
        titlebar_button_rects(s, &bmin, &bmax, &bclose, &by);
        if (my >= by && my < by + SWM_BTN_SIZE) {
            if (mx >= bclose && mx < bclose + SWM_BTN_SIZE) { *out_surface = s; return SWM_HIT_BTN_CLOSE; }
            if (mx >= bmax   && mx < bmax   + SWM_BTN_SIZE) { *out_surface = s; return SWM_HIT_BTN_MAX; }
            if (mx >= bmin   && mx < bmin   + SWM_BTN_SIZE) { *out_surface = s; return SWM_HIT_BTN_MIN; }
        }
        *out_surface = s;
        return SWM_HIT_TITLEBAR;
    }
    *out_surface = NULL;
    return SWM_HIT_NONE;
}

static swm_surface_t *topmost_surface(void) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = collect_surfaces_z_asc(list, SWM_MAX_SURFACES);
    return n > 0 ? list[n - 1] : NULL;
}

static void fill_rect(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                      int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > dst_w) w = dst_w - x;
    if (y + h > dst_h) h = dst_h - y;
    if (w <= 0 || h <= 0) return;
    for (int32_t row = 0; row < h; row++) {
        uint32_t *dr = dst + (y + row) * dst_pitch_px + x;
        for (int32_t col = 0; col < w; col++) dr[col] = color;
    }
}

static const uint8_t SWM_FONT_5x7[95][7];

static void draw_glyph(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                       int32_t x, int32_t y, char c, uint32_t color) {
    int idx = (c >= 32 && c <= 126) ? (c - 32) : ('?' - 32);
    const uint8_t *g = SWM_FONT_5x7[idx];
    for (int gy = 0; gy < 7; gy++) {
        uint8_t row = g[gy];
        for (int gx = 0; gx < 5; gx++) {
            if (row & (1 << (4 - gx))) {
                int32_t px = x + gx, py = y + gy;
                if (px >= 0 && px < dst_w && py >= 0 && py < dst_h) {
                    dst[py * dst_pitch_px + px] = color;
                }
            }
        }
    }
}

static int32_t draw_text(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                         int32_t x, int32_t y, const char *s, uint32_t color, int32_t max_w) {
    int32_t cur = x;
    for (; *s; s++) {
        if (cur + 5 > x + max_w) break;
        draw_glyph(dst, dst_w, dst_h, dst_pitch_px, cur, y, *s, color);
        cur += 6;
    }
    return cur;
}

static void draw_cursor(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                        int32_t cx, int32_t cy) {
    static const uint8_t arrow[16][12] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,1,1,1,1,0,0},
        {1,2,2,1,2,2,1,0,0,0,0,0},
        {1,2,1,0,1,2,2,1,0,0,0,0},
        {1,1,0,0,1,2,2,1,0,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,1,2,2,1,0,0,0},
        {0,0,0,0,0,0,1,1,0,0,0,0},
    };
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 12; x++) {
            uint8_t v = arrow[y][x];
            if (v == 0) continue;
            int32_t px = cx + x, py = cy + y;
            if (px < 0 || px >= dst_w || py < 0 || py >= dst_h) continue;
            dst[py * dst_pitch_px + px] = (v == 1) ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
}

static void draw_titlebar_chrome(uint32_t *dst, int32_t dst_w, int32_t dst_h, int32_t dst_pitch_px,
                                 swm_surface_t *s, int is_focused) {
    uint32_t bar_color    = is_focused ? 0xFF3A6CB0u : 0xFF333742u;
    uint32_t border_color = is_focused ? 0xFF5AA0F0u : 0xFF4A4F5Bu;
    uint32_t text_color   = is_focused ? 0xFFFFFFFFu : 0xFFB5BAC4u;
    int32_t outer_x, outer_y, outer_w, outer_h;
    surface_outer_rect(s, &outer_x, &outer_y, &outer_w, &outer_h);

    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x, outer_y, outer_w, SWM_BORDER, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x, outer_y + outer_h - SWM_BORDER, outer_w, SWM_BORDER, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x, outer_y, SWM_BORDER, outer_h, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, outer_x + outer_w - SWM_BORDER, outer_y, SWM_BORDER, outer_h, border_color);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px,
              outer_x + SWM_BORDER, outer_y + SWM_BORDER,
              outer_w - 2 * SWM_BORDER, SWM_TITLEBAR_H, bar_color);

    int32_t bmin, bmax, bclose, by;
    titlebar_button_rects(s, &bmin, &bmax, &bclose, &by);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, bmin,   by, SWM_BTN_SIZE, SWM_BTN_SIZE, 0xFFD0B040u);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, bmax,   by, SWM_BTN_SIZE, SWM_BTN_SIZE, 0xFF40C060u);
    fill_rect(dst, dst_w, dst_h, dst_pitch_px, bclose, by, SWM_BTN_SIZE, SWM_BTN_SIZE, 0xFFE05050u);

    int32_t title_x = outer_x + SWM_BORDER + 6;
    int32_t title_y = outer_y + SWM_BORDER + (SWM_TITLEBAR_H - 7) / 2;
    int32_t title_max = bmin - title_x - 6;
    if (title_max > 0) {
        draw_text(dst, dst_w, dst_h, dst_pitch_px, title_x, title_y, s->title, text_color, title_max);
    }
}

static void composite_surfaces(srapi_framebuffer_t *fb, uint32_t bg_color) {
    uint32_t *dst = srapi_framebuffer_pixels(fb);
    int32_t dst_w = (int32_t)srapi_framebuffer_width(fb);
    int32_t dst_h = (int32_t)srapi_framebuffer_height(fb);
    int32_t dst_pitch_px = (int32_t)(srapi_framebuffer_pitch(fb) / 4u);
    if (dst == NULL) return;

    for (int32_t y = 0; y < dst_h; y++) {
        uint32_t *row = dst + (size_t)y * dst_pitch_px;
        for (int32_t x = 0; x < dst_w; x++) row[x] = bg_color;
    }

    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = collect_surfaces_z_asc(list, SWM_MAX_SURFACES);
    swm_surface_t *focused = n > 0 ? list[n - 1] : NULL;

    for (int i = 0; i < n; i++) {
        swm_surface_t *s = list[i];
        draw_titlebar_chrome(dst, dst_w, dst_h, dst_pitch_px, s, s == focused);

        if (s->buf_map == NULL) continue;
        int32_t ex, ey, ew, eh;
        surface_effective_rect(s, &ex, &ey, &ew, &eh);
        int32_t src_pitch_px = (int32_t)(s->stride / 4u);
        const uint32_t *src = (const uint32_t *)s->buf_map;
        int32_t src_w = (int32_t)s->width;
        int32_t src_h = (int32_t)s->height;

        if (ew == src_w && eh == src_h) {
            int32_t sx0 = 0, sy0 = 0;
            int32_t w = src_w, h = src_h;
            int32_t dx = ex, dy = ey;
            if (dx < 0) { sx0 = -dx; w -= sx0; dx = 0; }
            if (dy < 0) { sy0 = -dy; h -= sy0; dy = 0; }
            if (dx + w > dst_w) w = dst_w - dx;
            if (dy + h > dst_h) h = dst_h - dy;
            if (w <= 0 || h <= 0) continue;
            for (int32_t row = 0; row < h; row++) {
                const uint32_t *sr = src + (sy0 + row) * src_pitch_px + sx0;
                uint32_t *dr = dst + (dy + row) * dst_pitch_px + dx;
                memcpy(dr, sr, (size_t)w * 4u);
            }
        } else {
            int32_t dy0 = ey > 0 ? ey : 0;
            int32_t dy1 = ey + eh < dst_h ? ey + eh : dst_h;
            int32_t dx0 = ex > 0 ? ex : 0;
            int32_t dx1 = ex + ew < dst_w ? ex + ew : dst_w;
            if (ew <= 0 || eh <= 0) continue;
            for (int32_t dy = dy0; dy < dy1; dy++) {
                int32_t sy = (int32_t)(((int64_t)(dy - ey) * src_h) / eh);
                if (sy < 0) sy = 0; else if (sy >= src_h) sy = src_h - 1;
                const uint32_t *sr = src + sy * src_pitch_px;
                uint32_t *dr = dst + dy * dst_pitch_px;
                for (int32_t dx = dx0; dx < dx1; dx++) {
                    int32_t sx = (int32_t)(((int64_t)(dx - ex) * src_w) / ew);
                    if (sx < 0) sx = 0; else if (sx >= src_w) sx = src_w - 1;
                    dr[dx] = sr[sx];
                }
            }
        }
    }
    draw_cursor(dst, dst_w, dst_h, dst_pitch_px, g_swm.mouse_x, g_swm.mouse_y);
}

static void send_close_to(swm_surface_t *s) {
    if (s == NULL || s->owner == NULL) return;
    sprot_header_t hdr = { .type = SPROT_EVT_SURFACE_CLOSE, .object_id = s->id };
    sprot_send_message(s->owner->sock, &hdr, NULL, 0, -1);
}

static void send_configure_to(swm_surface_t *s, uint32_t state_flags) {
    if (s == NULL || s->owner == NULL) return;
    sprot_body_configure_t body = {
        .width = s->width,
        .height = s->height,
        .state = state_flags,
        .serial = ++g_swm.next_z,
    };
    sprot_header_t hdr = { .type = SPROT_EVT_SURFACE_CONFIGURE, .object_id = s->id, .serial = body.serial };
    sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
}

static void toggle_maximize(swm_surface_t *s) {
    s->maximized = !s->maximized;
    send_configure_to(s, (s->maximized ? SPROT_SURFACE_STATE_MAXIMIZED : 0) | SPROT_SURFACE_STATE_FOCUSED);
}

static void forward_input(const srapi_input_event_t *ev) {
    swm_surface_t *s = NULL;
    swm_hit_region_t region;

    switch (ev->type) {
        case SRAPI_INPUT_EVENT_MOUSE_MOTION: {
            g_swm.mouse_x = ev->mouse_motion.x;
            g_swm.mouse_y = ev->mouse_motion.y;
            if (g_swm.interaction == SWM_INTERACT_MOVE && g_swm.grab_surface != NULL) {
                g_swm.grab_surface->pos_x = g_swm.mouse_x - g_swm.grab_offset_x;
                g_swm.grab_surface->pos_y = g_swm.mouse_y - g_swm.grab_offset_y;
                return;
            }
            region = hit_test(g_swm.mouse_x, g_swm.mouse_y, &s);
            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                int32_t ex, ey, ew, eh;
                surface_effective_rect(s, &ex, &ey, &ew, &eh);
                int32_t lx = (int32_t)(((int64_t)(g_swm.mouse_x - ex) * (int64_t)s->width) / (ew > 0 ? ew : 1));
                int32_t ly = (int32_t)(((int64_t)(g_swm.mouse_y - ey) * (int64_t)s->height) / (eh > 0 ? eh : 1));
                sprot_body_pointer_motion_t body = { .x = lx, .y = ly };
                send_event(s->owner->sock, SPROT_EVT_POINTER_MOTION, s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN: {
            g_swm.mouse_left_down = (ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT);
            region = hit_test(g_swm.mouse_x, g_swm.mouse_y, &s);
            if (s != NULL) raise_surface(s);
            int alt_held = (g_swm.modifiers & SRAPI_KMOD_ALT) != 0;
            if (region == SWM_HIT_BTN_CLOSE) {
                send_close_to(s);
                return;
            }
            if (region == SWM_HIT_BTN_MIN) {
                s->minimized = 1;
                return;
            }
            if (region == SWM_HIT_BTN_MAX) {
                toggle_maximize(s);
                return;
            }
            if ((region == SWM_HIT_TITLEBAR ||
                 (region == SWM_HIT_CONTENT && alt_held)) && s != NULL &&
                ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT && !s->maximized) {
                g_swm.interaction = SWM_INTERACT_MOVE;
                g_swm.grab_surface = s;
                g_swm.grab_offset_x = g_swm.mouse_x - s->pos_x;
                g_swm.grab_offset_y = g_swm.mouse_y - s->pos_y;
                return;
            }
            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                sprot_body_pointer_button_t body = {
                    .button = ev->mouse_button.button,
                    .state = SPROT_BUTTON_STATE_PRESSED,
                };
                send_event(s->owner->sock, SPROT_EVT_POINTER_BUTTON, s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP: {
            if (ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT) {
                g_swm.mouse_left_down = 0;
                if (g_swm.interaction == SWM_INTERACT_MOVE) {
                    g_swm.interaction = SWM_INTERACT_NONE;
                    g_swm.grab_surface = NULL;
                    return;
                }
            }
            region = hit_test(g_swm.mouse_x, g_swm.mouse_y, &s);
            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                sprot_body_pointer_button_t body = {
                    .button = ev->mouse_button.button,
                    .state = SPROT_BUTTON_STATE_RELEASED,
                };
                send_event(s->owner->sock, SPROT_EVT_POINTER_BUTTON, s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_KEY_DOWN:
        case SRAPI_INPUT_EVENT_KEY_UP: {
            g_swm.modifiers = ev->key.modifiers;
            if (ev->type == SRAPI_INPUT_EVENT_KEY_DOWN &&
                ev->key.scancode == SRAPI_SCANCODE_ESCAPE &&
                (ev->key.modifiers & (SRAPI_KMOD_CTRL | SRAPI_KMOD_ALT)) != 0) {
                g_swm.should_quit = 1;
                break;
            }
            swm_surface_t *focused = topmost_surface();
            if (focused != NULL && focused->owner != NULL) {
                sprot_body_key_t body = {
                    .scancode = ev->key.scancode,
                    .state = ev->type == SRAPI_INPUT_EVENT_KEY_DOWN
                             ? SPROT_KEY_STATE_PRESSED : SPROT_KEY_STATE_RELEASED,
                    .modifiers = ev->key.modifiers,
                };
                send_event(focused->owner->sock, SPROT_EVT_KEY, focused->id, 0, &body, sizeof(body));
            }
            break;
        }
        default:
            break;
    }
}

static void deliver_frame_callbacks(void) {
    uint64_t now_ms = (uint64_t)g_swm.frame_count * 16u;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &g_swm.surfaces[i];
        if (!s->in_use || !s->wants_frame || s->owner == NULL) continue;
        s->wants_frame = 0;
        sprot_body_frame_t body = { .time_ms = (uint32_t)now_ms, .serial = (uint32_t)g_swm.frame_count };
        sprot_header_t hdr = { .type = SPROT_EVT_SURFACE_FRAME, .object_id = s->id, .serial = body.serial };
        sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
    }
}

static void usage(const char *argv0) {
    printf("swm - simple window manager / compositor (sprot v%d.%d)\n",
           SPROT_VERSION_MAJOR, SPROT_VERSION_MINOR);
    printf("usage: %s [--socket /path/swm.sock] [--debug] [--bg RRGGBB]\n", argv0);
    printf("controls inside swm:\n");
    printf("  drag titlebar / Alt+LMB drag = move window\n");
    printf("  yellow btn = minimize, green = maximize, red = close\n");
    printf("  Ctrl+Alt+Esc = quit compositor\n");
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

        srapi_framebuffer_t *fb = srapi_drm_backbuffer(g_swm.drm);
        composite_surfaces(fb, bg_color);
        srapi_drm_present(g_swm.drm);
        g_swm.frame_count++;
        deliver_frame_callbacks();
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

#define GS(a,b,c,d,e,f,g) { a,b,c,d,e,f,g }
static const uint8_t SWM_FONT_5x7[95][7] = {
    GS(0x00,0x00,0x00,0x00,0x00,0x00,0x00), GS(0x04,0x04,0x04,0x04,0x04,0x00,0x04),
    GS(0x0A,0x0A,0x00,0x00,0x00,0x00,0x00), GS(0x0A,0x1F,0x0A,0x0A,0x0A,0x1F,0x0A),
    GS(0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04), GS(0x19,0x19,0x02,0x04,0x08,0x13,0x13),
    GS(0x0C,0x12,0x14,0x08,0x15,0x12,0x0D), GS(0x04,0x04,0x00,0x00,0x00,0x00,0x00),
    GS(0x02,0x04,0x08,0x08,0x08,0x04,0x02), GS(0x08,0x04,0x02,0x02,0x02,0x04,0x08),
    GS(0x00,0x0A,0x04,0x1F,0x04,0x0A,0x00), GS(0x00,0x04,0x04,0x1F,0x04,0x04,0x00),
    GS(0x00,0x00,0x00,0x00,0x00,0x04,0x08), GS(0x00,0x00,0x00,0x1F,0x00,0x00,0x00),
    GS(0x00,0x00,0x00,0x00,0x00,0x00,0x04), GS(0x01,0x02,0x02,0x04,0x08,0x08,0x10),
    GS(0x0E,0x11,0x13,0x15,0x19,0x11,0x0E), GS(0x04,0x0C,0x04,0x04,0x04,0x04,0x0E),
    GS(0x0E,0x11,0x01,0x02,0x04,0x08,0x1F), GS(0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E),
    GS(0x02,0x06,0x0A,0x12,0x1F,0x02,0x02), GS(0x1F,0x10,0x1E,0x01,0x01,0x01,0x1E),
    GS(0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E), GS(0x1F,0x01,0x02,0x04,0x08,0x10,0x10),
    GS(0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E), GS(0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E),
    GS(0x00,0x04,0x00,0x00,0x00,0x04,0x00), GS(0x00,0x04,0x00,0x00,0x00,0x04,0x08),
    GS(0x01,0x02,0x04,0x08,0x04,0x02,0x01), GS(0x00,0x00,0x1F,0x00,0x1F,0x00,0x00),
    GS(0x10,0x08,0x04,0x02,0x04,0x08,0x10), GS(0x0E,0x11,0x01,0x02,0x04,0x00,0x04),
    GS(0x0E,0x11,0x17,0x15,0x17,0x10,0x0E), GS(0x0E,0x11,0x11,0x1F,0x11,0x11,0x11),
    GS(0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E), GS(0x0F,0x10,0x10,0x10,0x10,0x10,0x0F),
    GS(0x1E,0x11,0x11,0x11,0x11,0x11,0x1E), GS(0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F),
    GS(0x1F,0x10,0x10,0x1E,0x10,0x10,0x10), GS(0x0F,0x10,0x10,0x13,0x11,0x11,0x0F),
    GS(0x11,0x11,0x11,0x1F,0x11,0x11,0x11), GS(0x0E,0x04,0x04,0x04,0x04,0x04,0x0E),
    GS(0x01,0x01,0x01,0x01,0x01,0x11,0x0E), GS(0x11,0x12,0x14,0x18,0x14,0x12,0x11),
    GS(0x10,0x10,0x10,0x10,0x10,0x10,0x1F), GS(0x11,0x1B,0x15,0x15,0x11,0x11,0x11),
    GS(0x11,0x19,0x15,0x13,0x11,0x11,0x11), GS(0x0E,0x11,0x11,0x11,0x11,0x11,0x0E),
    GS(0x1E,0x11,0x11,0x1E,0x10,0x10,0x10), GS(0x0E,0x11,0x11,0x11,0x15,0x12,0x0D),
    GS(0x1E,0x11,0x11,0x1E,0x14,0x12,0x11), GS(0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E),
    GS(0x1F,0x04,0x04,0x04,0x04,0x04,0x04), GS(0x11,0x11,0x11,0x11,0x11,0x11,0x0E),
    GS(0x11,0x11,0x11,0x11,0x11,0x0A,0x04), GS(0x11,0x11,0x11,0x11,0x15,0x15,0x0A),
    GS(0x11,0x11,0x0A,0x04,0x0A,0x11,0x11), GS(0x11,0x11,0x0A,0x04,0x04,0x04,0x04),
    GS(0x1F,0x01,0x02,0x04,0x08,0x10,0x1F), GS(0x0E,0x08,0x08,0x08,0x08,0x08,0x0E),
    GS(0x10,0x08,0x08,0x04,0x02,0x02,0x01), GS(0x0E,0x02,0x02,0x02,0x02,0x02,0x0E),
    GS(0x04,0x0A,0x11,0x00,0x00,0x00,0x00), GS(0x00,0x00,0x00,0x00,0x00,0x00,0x1F),
    GS(0x08,0x04,0x00,0x00,0x00,0x00,0x00), GS(0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F),
    GS(0x10,0x10,0x1E,0x11,0x11,0x11,0x1E), GS(0x00,0x00,0x0F,0x10,0x10,0x10,0x0F),
    GS(0x01,0x01,0x0F,0x11,0x11,0x11,0x0F), GS(0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E),
    GS(0x06,0x09,0x08,0x1C,0x08,0x08,0x08), GS(0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E),
    GS(0x10,0x10,0x1E,0x11,0x11,0x11,0x11), GS(0x04,0x00,0x0C,0x04,0x04,0x04,0x0E),
    GS(0x02,0x00,0x06,0x02,0x02,0x12,0x0C), GS(0x10,0x10,0x12,0x14,0x18,0x14,0x12),
    GS(0x0C,0x04,0x04,0x04,0x04,0x04,0x0E), GS(0x00,0x00,0x1A,0x15,0x15,0x15,0x15),
    GS(0x00,0x00,0x1E,0x11,0x11,0x11,0x11), GS(0x00,0x00,0x0E,0x11,0x11,0x11,0x0E),
    GS(0x00,0x00,0x1E,0x11,0x1E,0x10,0x10), GS(0x00,0x00,0x0F,0x11,0x0F,0x01,0x01),
    GS(0x00,0x00,0x16,0x19,0x10,0x10,0x10), GS(0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E),
    GS(0x08,0x08,0x1C,0x08,0x08,0x09,0x06), GS(0x00,0x00,0x11,0x11,0x11,0x11,0x0F),
    GS(0x00,0x00,0x11,0x11,0x11,0x0A,0x04), GS(0x00,0x00,0x11,0x11,0x15,0x15,0x0A),
    GS(0x00,0x00,0x11,0x0A,0x04,0x0A,0x11), GS(0x00,0x00,0x11,0x11,0x0F,0x01,0x0E),
    GS(0x00,0x00,0x1F,0x02,0x04,0x08,0x1F), GS(0x02,0x04,0x04,0x08,0x04,0x04,0x02),
    GS(0x04,0x04,0x04,0x04,0x04,0x04,0x04), GS(0x08,0x04,0x04,0x02,0x04,0x04,0x08),
    GS(0x09,0x15,0x12,0x00,0x00,0x00,0x00),
};
