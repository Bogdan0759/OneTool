#include <sprot/client.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *argv0) {
    printf("sprot_hello - minimal sprot test client\n");
    printf("usage: %s [--socket /path/swm.sock] [-w width] [-h height] [--seconds n] [--color RRGGBB]\n", argv0);
    printf("connects, creates a surface, paints it a color, commits, and drains events until N seconds pass.\n");
}

static uint32_t parse_hex(const char *s, uint32_t fallback) {
    if (s == NULL) return fallback;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == NULL || *end != '\0' || v > 0xFFFFFFu) return fallback;
    return 0xFF000000u | (uint32_t)v;
}

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void paint(uint32_t *pixels, uint32_t w, uint32_t h, uint32_t fill, uint32_t border, int phase) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = fill;
            if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4) c = border;
            else if (((x + phase) ^ (y + phase)) & 0x10) c = (fill & 0xFF000000u) | ((fill & 0x00FFFFFFu) ^ 0x002B2B2Bu);
            pixels[y * w + x] = c;
        }
    }
}

int main(int argc, char *argv[]) {
    const char *socket_path = NULL;
    int w = 320, h = 200;
    int seconds = 10;
    uint32_t color = 0xFF4070C8u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            w = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            h = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--color") == 0 && i + 1 < argc) {
            color = parse_hex(argv[++i], color);
        }
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        fprintf(stderr, "bad size %dx%d\n", w, h);
        return 1;
    }

    sprot_connection_t *conn = sprot_connect(socket_path);
    if (conn == NULL) {
        fprintf(stderr, "sprot_hello: %s\n", sprot_last_error());
        return 1;
    }
    fprintf(stderr, "sprot_hello: connected, display %ux%u\n",
            sprot_display_width(conn), sprot_display_height(conn));

    sprot_surface_t *s = sprot_create_surface(conn, (uint32_t)w, (uint32_t)h);
    if (s == NULL) {
        fprintf(stderr, "sprot_hello: %s\n", sprot_last_error());
        sprot_disconnect(conn);
        return 1;
    }

    sprot_event_t ev;
    int got_id = 0;
    while (!got_id) {
        int r = sprot_poll_event(conn, &ev, 2000);
        if (r < 0) {
            fprintf(stderr, "sprot_hello: poll: %s\n", sprot_last_error());
            sprot_disconnect(conn);
            return 1;
        }
        if (r == 0) {
            fprintf(stderr, "sprot_hello: timeout waiting for SURFACE_CREATED\n");
            sprot_disconnect(conn);
            return 1;
        }
        if (ev.kind == SPROT_EVENT_SURFACE_CREATED) {
            got_id = 1;
            fprintf(stderr, "sprot_hello: surface id=%u\n", ev.u.surface_created.surface_id);
        } else if (ev.kind == SPROT_EVENT_DISCONNECT) {
            fprintf(stderr, "sprot_hello: disconnected before surface ready\n");
            sprot_disconnect(conn);
            return 1;
        }
    }

    uint32_t *pixels = sprot_surface_pixels(s);
    if (pixels == NULL) {
        fprintf(stderr, "sprot_hello: no pixel buffer\n");
        sprot_disconnect(conn);
        return 1;
    }
    paint(pixels, (uint32_t)w, (uint32_t)h, color, 0xFFE0E8F0u, 0);
    if (sprot_commit(s) != 0) {
        fprintf(stderr, "sprot_hello: commit: %s\n", sprot_last_error());
        sprot_disconnect(conn);
        return 1;
    }
    fprintf(stderr, "sprot_hello: committed first frame\n");

    double start = mono_seconds();
    int phase = 0;
    int repaint_due_at_ms = 100;
    uint64_t pointer_events = 0, button_events = 0, key_events = 0;
    int disconnected = 0;
    while (!disconnected) {
        double elapsed = mono_seconds() - start;
        if (elapsed >= (double)seconds) break;
        int wait_ms = repaint_due_at_ms - (int)(elapsed * 1000.0) % repaint_due_at_ms;
        if (wait_ms < 1) wait_ms = 1;
        int r = sprot_poll_event(conn, &ev, wait_ms);
        if (r < 0) break;
        if (r == 1) {
            switch (ev.kind) {
                case SPROT_EVENT_POINTER_MOTION: pointer_events++; break;
                case SPROT_EVENT_POINTER_BUTTON: button_events++;
                    fprintf(stderr, "sprot_hello: button %u state=%u\n",
                            ev.u.pointer_button.button, ev.u.pointer_button.state);
                    break;
                case SPROT_EVENT_KEY:
                    key_events++;
                    if (ev.u.key.state == SPROT_KEY_STATE_PRESSED) {
                        fprintf(stderr, "sprot_hello: key scancode=%u\n", ev.u.key.scancode);
                    }
                    break;
                case SPROT_EVENT_DISCONNECT:
                    disconnected = 1;
                    fprintf(stderr, "sprot_hello: compositor closed connection\n");
                    break;
                case SPROT_EVENT_ERROR:
                    fprintf(stderr, "sprot_hello: server error %u: %s\n",
                            ev.u.error.code, ev.u.error.message);
                    break;
                default: break;
            }
        }
        phase++;
        if ((phase & 1) == 0) {
            paint(pixels, (uint32_t)w, (uint32_t)h, color, 0xFFE0E8F0u, phase);
            sprot_commit(s);
        }
    }
    fprintf(stderr, "sprot_hello: done. motion=%llu button=%llu key=%llu\n",
            (unsigned long long)pointer_events,
            (unsigned long long)button_events,
            (unsigned long long)key_events);
    sprot_destroy_surface(s);
    sprot_disconnect(conn);
    return 0;
}
