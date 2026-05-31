#define _GNU_SOURCE
#include <swm/swm.h>
#include "backend/backend.h"
#include "de/de.h"
#include "interaction/interaction.h"
#include "protocol/protocol.h"
#include "render/render.h"
#include "surface/surface.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
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

typedef struct {
    uint32_t sent;
    uint32_t dropped;
} frame_delivery_t;

typedef struct {
    uint64_t start_ns;
    uint64_t loops;
    uint64_t frames;
    uint64_t skipped;
    uint64_t poll_ns;
    uint64_t protocol_ns;
    uint64_t input_ns;
    uint64_t de_ns;
    uint64_t render_ns;
    uint64_t present_ns;
    uint64_t frame_cb_ns;
    uint64_t protocol_msgs;
    uint64_t input_events;
    uint64_t accepts;
    uint64_t frame_sent;
    uint64_t frame_dropped;
} swm_profile_t;

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double avg_ms(uint64_t ns, uint64_t count) {
    return count > 0 ? (double)ns / (double)count / 1000000.0 : 0.0;
}

static int count_clients(const swm_state_t *swm) {
    int n = 0;
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (swm->clients[i].in_use) n++;
    }
    return n;
}

static void profile_report_if_due(swm_profile_t *p) {
    uint64_t now = monotonic_ns();
    if (p->start_ns == 0) {
        p->start_ns = now;
        return;
    }
    uint64_t elapsed_ns = now - p->start_ns;
    if (elapsed_ns < 1000000000ull) return;

    swm_surface_t *visible_list[SWM_MAX_SURFACES];
    int visible_surfaces = swm_surface_collect_z_asc(&g_swm, visible_list, SWM_MAX_SURFACES);
    double sec = (double)elapsed_ns / 1000000000.0;
    double loop_hz = sec > 0.0 ? (double)p->loops / sec : 0.0;
    double repaint_fps = sec > 0.0 ? (double)p->frames / sec : 0.0;

    logf_("prof %.2fs loop=%.0f/s repaint=%.1f/s skip=%llu clients=%d surfaces=%d "
          "input=%.0f/s proto=%.0f/s accept=%.0f/s cb=%llu/%llu avg_ms wait=%.2f proto=%.3f "
          "input=%.3f de=%.3f render=%.3f present=%.3f cb=%.3f",
          sec, loop_hz, repaint_fps, (unsigned long long)p->skipped,
          count_clients(&g_swm), visible_surfaces,
          sec > 0.0 ? (double)p->input_events / sec : 0.0,
          sec > 0.0 ? (double)p->protocol_msgs / sec : 0.0,
          sec > 0.0 ? (double)p->accepts / sec : 0.0,
          (unsigned long long)p->frame_sent,
          (unsigned long long)p->frame_dropped,
          avg_ms(p->poll_ns, p->loops),
          avg_ms(p->protocol_ns, p->protocol_msgs),
          avg_ms(p->input_ns, p->loops),
          avg_ms(p->de_ns, p->frames),
          avg_ms(p->render_ns, p->frames),
          avg_ms(p->present_ns, p->frames),
          avg_ms(p->frame_cb_ns, p->frames));

    *p = (swm_profile_t){ .start_ns = now };
}

static frame_delivery_t deliver_frame_callbacks(void) {
    frame_delivery_t stats = {0};
    uint64_t now_ms = (uint64_t)g_swm.frame_count * 16u;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *s = &g_swm.surfaces[i];
        if (!s->in_use || !s->wants_frame || s->owner == NULL) continue;
        s->wants_frame = 0;
        sprot_body_frame_t body = { .time_ms = (uint32_t)now_ms, .serial = (uint32_t)g_swm.frame_count };
        /* Best-effort: if the client is busy and can't read fast enough,
         * drop the frame event rather than stalling the compositor. */
        if (swm_protocol_send_event_nb(s->owner->sock, SPROT_EVT_SURFACE_FRAME, s->id,
                          body.serial, &body, sizeof(body)) != 0) {
            /* Re-arm so the client can pick up the frame next tick. */
            s->wants_frame = 1;
            stats.dropped++;
        } else {
            stats.sent++;
        }
    }
    return stats;
}

static void usage(const char *argv0) {
    printf("swm - simple window manager / compositor (sprot v%d.%d)\n",
           SPROT_VERSION_MAJOR, SPROT_VERSION_MINOR);
    printf("usage: %s [--socket /path/swm.sock] [--profile] [--debug] [--bg RRGGBB]\n", argv0);
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
    int profile_enabled = 0;
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
            profile_enabled = 1;
        } else if (strcmp(argv[i], "--profile") == 0) {
            profile_enabled = 1;
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
        logf_("drm open: %s", srapi_last_error());
        return 1;
    }
    g_swm.display_w = swm_output_width(g_swm.output);
    g_swm.display_h = swm_output_height(g_swm.output);
    g_swm.mouse_x = (int32_t)g_swm.display_w / 2;
    g_swm.mouse_y = (int32_t)g_swm.display_h / 2;
    logf_("display %ux%u", g_swm.display_w, g_swm.display_h);
    if (srapi_input_create(&(srapi_input_desc_t){
            .auto_discover = 1,
            .grab = 1,
            .initial_mouse_x = g_swm.mouse_x,
            .initial_mouse_y = g_swm.mouse_y,
        }, &g_swm.input) != SRAPI_OK) {
        logf_("input: %s (continuing without input)", srapi_last_error());
        g_swm.input = NULL;
    } else {
        srapi_input_set_bounds(g_swm.input, (int32_t)g_swm.display_w, (int32_t)g_swm.display_h);
    }

    g_swm.listen_fd = swm_protocol_setup_socket(&g_swm, socket_path);
    if (g_swm.listen_fd < 0) {
        if (g_swm.input != NULL) srapi_input_destroy(g_swm.input);
        swm_output_destroy(g_swm.output);
        g_swm.output = NULL;
        return 1;
    }

    g_swm.de = de_create(&g_swm);
    if (g_swm.de == NULL) {
        logf_("de: failed to initialize (continuing without DE)");
    }

    if (record_path != NULL) {
        if (swm_output_record_start(g_swm.output, record_path, record_fps * 1000u) != SRAPI_OK) {
            logf_("record: %s (continuing without recording)", srapi_last_error());
        } else {
            logf_("recording to %s @ %u fps (BGRA8888 srvid)", record_path, record_fps);
        }
    }

    logf_("ready. press Esc to quit.");
    swm_profile_t profile = {0};
    if (profile_enabled) profile.start_ns = monotonic_ns();
    int needs_repaint = 1;
    const uint64_t de_repaint_interval_ns = 500000000ull;
    uint64_t next_de_repaint_ns = monotonic_ns() + de_repaint_interval_ns;
    while (!g_swm.should_quit && !g_signal_quit) {
        if (profile_enabled) profile.loops++;
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

        uint64_t t0 = profile_enabled ? monotonic_ns() : 0;
        int pr = poll(pfds, (nfds_t)nfds, 8);
        if (profile_enabled) profile.poll_ns += monotonic_ns() - t0;
        if (pr < 0) {
            if (errno == EINTR) continue;
            logf_("poll: %s", strerror(errno));
            break;
        }
        if (pr > 0) {
            if (pfds[0].revents & POLLIN) {
                int cfd = accept4(g_swm.listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (cfd >= 0) {
                    swm_client_t *c = swm_protocol_alloc_client(&g_swm, cfd);
                    if (profile_enabled) profile.accepts++;
                    needs_repaint = 1;
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
                            t0 = profile_enabled ? monotonic_ns() : 0;
                            swm_protocol_dispatch(&g_swm, c);
                            if (profile_enabled) {
                                profile.protocol_ns += monotonic_ns() - t0;
                                profile.protocol_msgs++;
                            }
                            needs_repaint = 1;
                        } else {
                            swm_protocol_drop_client(&g_swm, c, "hup");
                            needs_repaint = 1;
                        }
                    }
                }
            }
        }

        if (g_swm.input != NULL) {
            srapi_input_event_t ev;
            t0 = profile_enabled ? monotonic_ns() : 0;
            while (srapi_input_poll(g_swm.input, &ev) == 1) {
                if (profile_enabled) profile.input_events++;
                needs_repaint = 1;
                swm_interaction_forward_input(&g_swm, &ev);
                if (g_swm.should_quit) break;
            }
            if (profile_enabled) profile.input_ns += monotonic_ns() - t0;
        }
        if (g_swm.should_quit) break;

        uint64_t now_ns = monotonic_ns();
        if (!needs_repaint && g_swm.de != NULL && now_ns >= next_de_repaint_ns) {
            needs_repaint = 1;
        }
        if (!needs_repaint) {
            if (profile_enabled) {
                profile.skipped++;
                profile_report_if_due(&profile);
            }
            continue;
        }

        srapi_framebuffer_t *fb = swm_output_backbuffer(g_swm.output);
        if (g_swm.de != NULL) {
            t0 = profile_enabled ? monotonic_ns() : 0;
            de_tick(g_swm.de, g_swm.frame_count);
            if (profile_enabled) profile.de_ns += monotonic_ns() - t0;
            next_de_repaint_ns = now_ns + de_repaint_interval_ns;
        }
        t0 = profile_enabled ? monotonic_ns() : 0;
        swm_render_composite(&g_swm, fb, bg_color);
        if (profile_enabled) profile.render_ns += monotonic_ns() - t0;
        t0 = profile_enabled ? monotonic_ns() : 0;
        swm_output_present(g_swm.output);
        if (profile_enabled) profile.present_ns += monotonic_ns() - t0;
        g_swm.frame_count++;
        t0 = profile_enabled ? monotonic_ns() : 0;
        frame_delivery_t frame_delivery = deliver_frame_callbacks();
        if (profile_enabled) {
            profile.frame_cb_ns += monotonic_ns() - t0;
            profile.frame_sent += frame_delivery.sent;
            profile.frame_dropped += frame_delivery.dropped;
            profile.frames++;
            profile_report_if_due(&profile);
        }
        needs_repaint = 0;
    }

    logf_("shutting down");
    if (swm_output_is_recording(g_swm.output)) {
        logf_("record: flushing");
        swm_output_record_stop(g_swm.output);
    }
    if (g_swm.de != NULL) {
        de_destroy(g_swm.de);
        g_swm.de = NULL;
    }
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (g_swm.clients[i].in_use) {
            swm_protocol_drop_client(&g_swm, &g_swm.clients[i], "shutdown");
        }
    }
    if (g_swm.listen_fd >= 0) close(g_swm.listen_fd);
    if (g_swm.socket_path[0] != '\0') unlink(g_swm.socket_path);
    if (g_swm.input != NULL) srapi_input_destroy(g_swm.input);
    swm_output_destroy(g_swm.output);
    return 0;
}
