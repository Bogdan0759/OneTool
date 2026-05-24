#define _GNU_SOURCE
#include <swm/swm.h>
#include "backend/backend.h"
#include "buffer/buffer.h"
#include "de/de.h"
#include "window.h"
#include "protocol.h"
#include "compositor.h"
#include "input.h"

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

void swm_logf(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[swm] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void usage(const char *argv0) {
    printf("swm - simple window manager / compositor (sprot v%d.%d)\n",
           SPROT_VERSION_MAJOR, SPROT_VERSION_MINOR);
    printf("usage: %s [--socket /path/swm.sock] [--debug] [--bg RRGGBB]\n", argv0);
    printf("           [--record file.srvid] [--record-fps N]\n");
    printf("controls inside swm:\n");
    printf("  drag titlebar / Alt+LMB drag = move window\n");
    printf("  yellow btn = minimize, green = maximize, red = close\n");
    printf("  Ctrl+Alt+Esc = quit compositor\n");
    printf("recording:\n");
    printf("  --record FILE       capture the whole compositor to FILE (.srvid)\n");
    printf("  --record-fps N      target frames-per-second (default: 30)\n");
    printf("  convert later with: srvid2mp4 FILE out.mp4\n");
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
    const char *record_path = NULL;
    uint32_t record_fps = 30;

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
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--record-fps") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (end == NULL || *end != '\0' || v == 0 || v > 240) {
                fprintf(stderr, "swm: bad --record-fps (must be 1..240)\n");
                return 1;
            }
            record_fps = (uint32_t)v;
        }
    }
    if (debug) srapi = 1;

    memset(&g_swm, 0, sizeof(g_swm));
    g_swm.next_cascade_x = 64;
    g_swm.next_cascade_y = 64;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (swm_output_create_default(&g_swm.output) != SRAPI_OK) {
        swm_logf("drm open: %s", srapi_last_error());
        return 1;
    }
    g_swm.display_w = swm_output_width(g_swm.output);
    g_swm.display_h = swm_output_height(g_swm.output);
    g_swm.mouse_x = (int32_t)g_swm.display_w / 2;
    g_swm.mouse_y = (int32_t)g_swm.display_h / 2;
    g_swm.prev_cursor_x = g_swm.mouse_x;
    g_swm.prev_cursor_y = g_swm.mouse_y;
    swm_logf("display %ux%u", g_swm.display_w, g_swm.display_h);
    if (srapi_input_create(&(srapi_input_desc_t){
            .auto_discover = 1,
            .grab = 1,
            .initial_mouse_x = g_swm.mouse_x,
            .initial_mouse_y = g_swm.mouse_y,
        }, &g_swm.input) != SRAPI_OK) {
        swm_logf("input: %s (continuing without input)", srapi_last_error());
        g_swm.input = NULL;
    } else {
        srapi_input_set_bounds(g_swm.input, (int32_t)g_swm.display_w, (int32_t)g_swm.display_h);
    }

    g_swm.listen_fd = swm_setup_socket(&g_swm, socket_path);
    if (g_swm.listen_fd < 0) {
        if (g_swm.input != NULL) srapi_input_destroy(g_swm.input);
        swm_output_destroy(g_swm.output);
        g_swm.output = NULL;
        return 1;
    }

    g_swm.de = de_create(&g_swm);
    if (g_swm.de == NULL) {
        swm_logf("de: failed to initialize (continuing without DE)");
    }

    if (record_path != NULL) {
        if (swm_output_record_start(g_swm.output, record_path, record_fps * 1000u) != SRAPI_OK) {
            swm_logf("record: %s (continuing without recording)", srapi_last_error());
        } else {
            swm_logf("recording to %s @ %u fps (BGRA8888 srvid)", record_path, record_fps);
        }
    }

    swm_logf("ready. press Esc to quit.");
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
            swm_logf("poll: %s", strerror(errno));
            break;
        }
        if (pr > 0) {
            if (pfds[0].revents & POLLIN) {
                int cfd = accept4(g_swm.listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (cfd >= 0) {
                    swm_client_t *c = swm_alloc_client(&g_swm, cfd);
                    if (c == NULL) {
                        swm_logf("client limit reached, rejecting");
                        close(cfd);
                    } else {
                        swm_logf("client connected fd=%d", cfd);
                    }
                }
            }
            for (int i = 1; i < nfds; i++) {
                if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    swm_client_t *c = &g_swm.clients[client_indices[i - 1]];
                    if (c->in_use) {
                        if (pfds[i].revents & POLLIN) {
                            swm_dispatch_message(&g_swm, c);
                        } else {
                            swm_drop_client(&g_swm, c, "hup");
                        }
                    }
                }
            }
        }

        if (g_swm.input != NULL) {
            srapi_input_event_t ev;
            while (srapi_input_poll(g_swm.input, &ev) == 1) {
                swm_forward_input(&g_swm, &ev);
                if (g_swm.should_quit) break;
            }
        }
        if (g_swm.should_quit) break;

        srapi_framebuffer_t *fb = swm_output_backbuffer(g_swm.output);
        if (g_swm.de != NULL) de_tick(g_swm.de, g_swm.frame_count);
        swm_composite_surfaces(&g_swm, fb, bg_color);
        swm_output_present(g_swm.output);
        g_swm.frame_count++;
        swm_deliver_frame_callbacks(&g_swm);
    }

    swm_logf("shutting down");
    if (swm_output_is_recording(g_swm.output)) {
        swm_logf("record: flushing");
        swm_output_record_stop(g_swm.output);
    }
    if (g_swm.de != NULL) {
        de_destroy(g_swm.de);
        g_swm.de = NULL;
    }
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (g_swm.clients[i].in_use) {
            swm_drop_client(&g_swm, &g_swm.clients[i], "shutdown");
        }
    }
    if (g_swm.listen_fd >= 0) close(g_swm.listen_fd);
    if (g_swm.socket_path[0] != '\0') unlink(g_swm.socket_path);
    if (g_swm.input != NULL) srapi_input_destroy(g_swm.input);
    swm_output_destroy(g_swm.output);
    return 0;
}
