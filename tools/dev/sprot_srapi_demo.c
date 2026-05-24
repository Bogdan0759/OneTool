#include <sprot/srapi_bridge.h>
#include <srapi/srapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *argv0) {
    printf("sprot_srapi_demo - render via SRAPI and present inside swm\n");
    printf("usage: %s [--socket path] [--title text] [-w width] [-h height] [--seconds n] [--gpu] [--device /dev/dri/cardN]\n", argv0);
}

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint8_t wave(int frame, int period, int lo, int hi) {
    int pos = period > 0 ? frame % period : 0;
    float t = period > 1 ? (float)pos / (float)(period - 1) : 0.0f;
    if (t > 0.5f) t = 1.0f - t;
    t *= 2.0f;
    return (uint8_t)(lo + (int)((hi - lo) * t));
}

static int pump_disconnect(sprot_connection_t *conn) {
    sprot_event_t ev;
    for (;;) {
        int r = sprot_poll_event(conn, &ev, 0);
        if (r <= 0) return 0;
        if (ev.kind == SPROT_EVENT_SURFACE_CLOSE || ev.kind == SPROT_EVENT_DISCONNECT) {
            return 1;
        }
    }
}

int main(int argc, char **argv) {
    const char *socket_path = NULL;
    const char *title = "srapi bridge";
    const char *device_path = NULL;
    uint32_t width = 480;
    uint32_t height = 300;
    int seconds = 10;
    int use_gpu = 0;

    sprot_connection_t *conn = NULL;
    sprot_surface_t *surface = NULL;
    srapi_context_t *ctx = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    srapi_framebuffer_t *fb = NULL;
    srapi_device_t *device = NULL;
    srapi_queue_t *queue = NULL;
    srapi_image_t *image = NULL;
    int rc = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            device_path = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gpu") == 0) {
            use_gpu = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }

    conn = sprot_connect(socket_path);
    if (conn == NULL) {
        fprintf(stderr, "sprot_srapi_demo: %s\n", sprot_last_error());
        goto done;
    }

    if (use_gpu) {
        if (srapi_create_device(&(srapi_device_desc_t){
                .backend = SRAPI_BACKEND_GPU,
                .device_path = device_path,
            }, &device) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: gpu device: %s\n", srapi_last_error());
            goto done;
        }
        if (srapi_create_queue(&(srapi_queue_desc_t){ .device = device }, &queue) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: gpu queue: %s\n", srapi_last_error());
            goto done;
        }
        if (srapi_create_context(&(srapi_context_desc_t){
                .width = width,
                .height = height,
                .backend = SRAPI_BACKEND_GPU,
            }, &ctx) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: gpu context: %s\n", srapi_last_error());
            goto done;
        }
        if (srapi_create_image(device, &(srapi_image_desc_t){
                .width = width,
                .height = height,
                .tiling = SRAPI_IMAGE_LINEAR,
                .usage = SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_COLOR_TARGET,
            }, &image) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: gpu image: %s\n", srapi_last_error());
            goto done;
        }
        if (srapi_create_cmd_buffer(ctx, &cmd) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: cmd: %s\n", srapi_last_error());
            goto done;
        }
        surface = sprot_create_surface_for_image(conn, image, title);
    } else {
        if (srapi_create_context(&(srapi_context_desc_t){
                .width = width,
                .height = height,
                .backend = SRAPI_BACKEND_CPU,
            }, &ctx) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: cpu context: %s\n", srapi_last_error());
            goto done;
        }
        if (srapi_create_framebuffer(ctx, &(srapi_framebuffer_desc_t){ .width = width, .height = height }, &fb) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: framebuffer: %s\n", srapi_last_error());
            goto done;
        }
        if (srapi_create_cmd_buffer(ctx, &cmd) != SRAPI_OK) {
            fprintf(stderr, "sprot_srapi_demo: cmd: %s\n", srapi_last_error());
            goto done;
        }
        surface = sprot_create_surface_for_framebuffer(conn, fb, title);
    }

    if (surface == NULL) {
        fprintf(stderr, "sprot_srapi_demo: create surface failed\n");
        goto done;
    }

    double start = mono_seconds();
    for (int frame = 0; mono_seconds() - start < (double)seconds; frame++) {
        uint8_t r = wave(frame, 120, 40, 220);
        uint8_t g = wave(frame + 30, 160, 60, 240);
        uint8_t b = wave(frame + 70, 180, 50, 210);
        srapi_color_t bg = srapi_rgba(r, g, b, 255);
        srapi_color_t box = srapi_rgba(255 - r / 2, 255 - g / 3, 255 - b / 2, 255);
        int32_t box_x = 20 + (frame * 3) % (int)(width > 140 ? width - 120 : 1);
        int32_t box_y = 20 + (frame * 2) % (int)(height > 120 ? height - 100 : 1);

        srapi_cmd_reset(cmd);
        srapi_cmd_clear(cmd, bg);
        srapi_cmd_fill_rect(cmd, box_x, box_y, width / 4, height / 4, box);
        srapi_cmd_draw_line(cmd, 0, 0, (int32_t)width - 1, (int32_t)height - 1, 0xFFFFFFFFu);
        srapi_cmd_draw_line(cmd, (int32_t)width - 1, 0, 0, (int32_t)height - 1, 0xFFFFFFFFu);

        if (image != NULL) {
            if (srapi_queue_submit_image(queue, image, cmd) != SRAPI_OK) {
                fprintf(stderr, "sprot_srapi_demo: submit image: %s\n", srapi_last_error());
                goto done;
            }
            if (sprot_present_image(surface, image) != 0) {
                fprintf(stderr, "sprot_srapi_demo: present image: %s\n", sprot_last_error());
                goto done;
            }
        } else {
            if (srapi_submit(ctx, fb, cmd) != SRAPI_OK) {
                fprintf(stderr, "sprot_srapi_demo: submit framebuffer: %s\n", srapi_last_error());
                goto done;
            }
            if (sprot_present_framebuffer(surface, fb) != 0) {
                fprintf(stderr, "sprot_srapi_demo: present framebuffer: %s\n", sprot_last_error());
                goto done;
            }
        }

        if (pump_disconnect(conn)) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 16 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    rc = 0;

done:
    if (surface != NULL) sprot_destroy_surface(surface);
    if (cmd != NULL) srapi_destroy_cmd_buffer(cmd);
    if (fb != NULL) srapi_destroy_framebuffer(fb);
    if (image != NULL) srapi_destroy_image(image);
    if (queue != NULL) srapi_destroy_queue(queue);
    if (device != NULL) srapi_destroy_device(device);
    if (ctx != NULL) srapi_destroy_context(ctx);
    if (conn != NULL) sprot_disconnect(conn);
    return rc;
}
