#include <srapi/srapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_CUBES          16
#define CUBE_SIZE          70
#define GAME_DURATION_SEC  15.0f
#define CUBE_LIFETIME_SEC  3.0f
#define SPAWN_INTERVAL_SEC 0.6f

typedef struct {
    int active;
    int32_t x;
    int32_t y;
    float age_sec;
} cube_t;

static int aabb_hit(const cube_t *c, int32_t mx, int32_t my) {
    return mx >= c->x && mx < c->x + CUBE_SIZE &&
           my >= c->y && my < c->y + CUBE_SIZE;
}

static srapi_color_t cube_color(float remaining) {
    if (remaining > 2.0f) {
        return srapi_rgba(80, 220, 100, 255);
    }
    if (remaining > 1.0f) {
        return srapi_rgba(240, 220, 80, 255);
    }
    return srapi_rgba(240, 80, 60, 255);
}

static void spawn_cube(cube_t *cubes, uint32_t width, uint32_t height) {
    int slot = -1;

    for (int i = 0; i < MAX_CUBES; i++) {
        if (!cubes[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return;
    }

    int32_t margin = 16;
    int32_t max_x = (int32_t)width  - CUBE_SIZE - margin;
    int32_t max_y = (int32_t)height - CUBE_SIZE - margin - 40;

    if (max_x <= margin) max_x = margin + 1;
    if (max_y <= margin + 40) max_y = margin + 41;

    cubes[slot].active = 1;
    cubes[slot].x = margin + (rand() % (max_x - margin));
    cubes[slot].y = margin + 40 + (rand() % (max_y - margin - 40));
    cubes[slot].age_sec = 0.0f;
}

static int try_hit(cube_t *cubes, int32_t mx, int32_t my) {
    int best = -1;
    float best_age = -1.0f;

    for (int i = 0; i < MAX_CUBES; i++) {
        if (!cubes[i].active) {
            continue;
        }
        if (!aabb_hit(&cubes[i], mx, my)) {
            continue;
        }
        if (cubes[i].age_sec > best_age) {
            best_age = cubes[i].age_sec;
            best = i;
        }
    }
    if (best < 0) {
        return 0;
    }
    cubes[best].active = 0;
    return 1;
}

static void draw_cube(srapi_cmd_buffer_t *cmd, const cube_t *c) {
    float remaining = CUBE_LIFETIME_SEC - c->age_sec;
    srapi_color_t fill = cube_color(remaining);
    srapi_color_t border = srapi_rgba(20, 20, 20, 255);
    int32_t bw = 4;

    srapi_cmd_fill_rect(cmd, c->x, c->y, CUBE_SIZE, CUBE_SIZE, border);
    srapi_cmd_fill_rect(cmd, c->x + bw, c->y + bw,
                        CUBE_SIZE - 2 * bw, CUBE_SIZE - 2 * bw, fill);
}

static void draw_time_bar(srapi_cmd_buffer_t *cmd, uint32_t width, float remaining) {
    int32_t bar_x = 16;
    int32_t bar_y = 12;
    int32_t bar_w = (int32_t)width - 32;
    int32_t bar_h = 14;
    float ratio = remaining / GAME_DURATION_SEC;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    int32_t filled = (int32_t)((float)bar_w * ratio);

    srapi_cmd_fill_rect(cmd, bar_x - 2, bar_y - 2, (uint32_t)(bar_w + 4), (uint32_t)(bar_h + 4),
                        srapi_rgba(40, 40, 50, 255));
    srapi_cmd_fill_rect(cmd, bar_x, bar_y, (uint32_t)bar_w, (uint32_t)bar_h,
                        srapi_rgba(20, 20, 28, 255));
    if (filled > 0) {
        srapi_color_t fill;
        if (ratio > 0.5f) {
            fill = srapi_rgba(90, 220, 120, 255);
        } else if (ratio > 0.2f) {
            fill = srapi_rgba(240, 220, 70, 255);
        } else {
            fill = srapi_rgba(240, 80, 70, 255);
        }
        srapi_cmd_fill_rect(cmd, bar_x, bar_y, (uint32_t)filled, (uint32_t)bar_h, fill);
    }
}

static void draw_score_pips(srapi_cmd_buffer_t *cmd, uint32_t width, int score) {
    int32_t pip = 10;
    int32_t gap = 4;
    int show = score > 50 ? 50 : score;
    int32_t total_w = show * (pip + gap);
    int32_t x = (int32_t)width - total_w - 16;
    int32_t y = 32;

    for (int i = 0; i < show; i++) {
        srapi_cmd_fill_rect(cmd,
            x + i * (pip + gap), y,
            (uint32_t)pip, (uint32_t)pip,
            srapi_rgba(120, 240, 160, 255));
    }
}

static void draw_cursor(srapi_cmd_buffer_t *cmd, int32_t x, int32_t y) {
    srapi_cmd_fill_rect(cmd, x - 1, y - 8, 3, 17, srapi_rgba(255, 255, 255, 255));
    srapi_cmd_fill_rect(cmd, x - 8, y - 1, 17, 3, srapi_rgba(255, 255, 255, 255));
}

int main(int argc, char *argv[]) {
    const char *drm_device = NULL;
    const char *record_path = NULL;
    uint32_t record_fps = 30;
    int debug = 0;
    int use_gpu = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("click - SRAPI click-the-cube minigame\n");
            printf("usage: %s [--drm /dev/dri/cardN] [--gpu] [--debug] [--record file.srvid] [--record-fps n]\n", argv[0]);
            printf("you have 15 seconds. cubes spawn and live for 3 seconds.\n");
            printf("click them with the left mouse button. Esc to abort.\n");
            printf("--gpu  use GPU/i915 BLT path when available (otherwise CPU rasterizer).\n");
            return 0;
        } else if (strcmp(argv[i], "--drm") == 0 && i + 1 < argc) {
            drm_device = argv[++i];
        } else if (strcmp(argv[i], "--gpu") == 0) {
            use_gpu = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = 1;
        } else if (strcmp(argv[i], "--record") == 0 && i + 1 < argc) {
            record_path = argv[++i];
        } else if (strcmp(argv[i], "--record-fps") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (end == NULL || *end != '\0' || v == 0 || v > 240) {
                fprintf(stderr, "bad --record-fps\n");
                return 1;
            }
            record_fps = (uint32_t)v;
        }
    }
    if (debug) {
        srapi = 1;
    }

    srapi_drm_display_t *drm = NULL;
    srapi_context_t *ctx = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    srapi_input_context_t *input = NULL;
    srapi_device_t *gpu_device = NULL;
    srapi_queue_t *gpu_queue = NULL;
    srapi_result_t r;
    srapi_drm_recommendation_t rec;

    if (drm_device == NULL) {
        if (srapi_drm_recommend(&rec) == SRAPI_OK) {
            drm_device = rec.path;
            printf("click: auto-selected %s (%s, score=%u)\n",
                   rec.path, rec.message, rec.score);
            if (use_gpu && !rec.supports_i915) {
                printf("click: note: --gpu requested but %s has no i915 acceleration; using basic GPU dumb-buffer path\n", rec.path);
            }
        } else {
            fprintf(stderr, "click: %s\n", srapi_last_error());
            fprintf(stderr, "hint: pass --drm /dev/dri/cardN explicitly\n");
            return 1;
        }
    }

    r = srapi_drm_open_display(&(srapi_drm_display_desc_t){
        .device_path = drm_device,
    }, &drm);
    if (r != SRAPI_OK) {
        fprintf(stderr, "click: drm open failed: %s\n", srapi_last_error());
        fprintf(stderr, "hint: run from a TTY, or pass --drm /dev/dri/cardN\n");
        return 1;
    }
    uint32_t width = srapi_drm_width(drm);
    uint32_t height = srapi_drm_height(drm);

    if (record_path != NULL) {
        r = srapi_drm_record_start(drm, record_path, record_fps * 1000);
        if (r != SRAPI_OK) {
            fprintf(stderr, "click: drm record start failed: %s\n", srapi_last_error());
            srapi_drm_close(drm);
            return 1;
        }
        fprintf(stderr, "click: recording -> %s @ %u fps\n", record_path, record_fps);
    }

    srapi_backend_t render_backend = SRAPI_BACKEND_GPU;
    if (use_gpu) {
        r = srapi_create_device(&(srapi_device_desc_t){
            .backend = SRAPI_BACKEND_GPU,
            .device_path = srapi_drm_device_path(drm),
        }, &gpu_device);
        if (r == SRAPI_OK) {
            r = srapi_create_queue(&(srapi_queue_desc_t){
                .device = gpu_device,
                .family_index = 0,
            }, &gpu_queue);
        }
        if (r != SRAPI_OK) {
            fprintf(stderr, "click: gpu queue init failed: %s — using CPU rasterizer on GPU framebuffer\n",
                    srapi_last_error());
            srapi_destroy_queue(gpu_queue);
            srapi_destroy_device(gpu_device);
            gpu_queue = NULL;
            gpu_device = NULL;
            use_gpu = 0;
        } else {
            printf("click: gpu queue enabled (device=%s)\n", srapi_device_path(gpu_device));
        }
    }

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = width,
        .height = height,
        .backend = render_backend,
    }, &ctx);
    if (r != SRAPI_OK) {
        fprintf(stderr, "click: context: %s\n", srapi_last_error());
        srapi_destroy_queue(gpu_queue);
        srapi_destroy_device(gpu_device);
        srapi_drm_close(drm);
        return 1;
    }

    r = srapi_create_cmd_buffer(ctx, &cmd);
    if (r != SRAPI_OK) {
        fprintf(stderr, "click: cmd buffer: %s\n", srapi_last_error());
        srapi_destroy_context(ctx);
        srapi_destroy_queue(gpu_queue);
        srapi_destroy_device(gpu_device);
        srapi_drm_close(drm);
        return 1;
    }

    srapi_input_desc_t in_desc = {
        .auto_discover = 1,
        .initial_mouse_x = (int32_t)(width / 2),
        .initial_mouse_y = (int32_t)(height / 2),
    };
    r = srapi_input_create(&in_desc, &input);
    if (r != SRAPI_OK) {
        fprintf(stderr, "click: input: %s\n", srapi_last_error());
        srapi_destroy_cmd_buffer(cmd);
        srapi_destroy_context(ctx);
        srapi_destroy_queue(gpu_queue);
        srapi_destroy_device(gpu_device);
        srapi_drm_close(drm);
        return 1;
    }
    srapi_input_set_bounds(input, (int32_t)width, (int32_t)height);

    srand((unsigned)time(NULL));

    cube_t cubes[MAX_CUBES] = { 0 };
    int score = 0;
    int spawned = 0;
    int misclicks = 0;
    float spawn_timer = 0.0f;
    float elapsed = 0.0f;
    int quit = 0;
    int32_t mouse_x = (int32_t)(width / 2);
    int32_t mouse_y = (int32_t)(height / 2);

    srapi_clock_t clock;
    srapi_clock_init(&clock);

    printf("click: %ux%u — click cubes for 15s. Esc to abort.\n", width, height);

    while (!quit && elapsed < GAME_DURATION_SEC) {
        float dt = srapi_clock_tick(&clock);
        elapsed += dt;
        spawn_timer += dt;

        srapi_input_event_t ev;
        while (srapi_input_poll(input, &ev) == 1) {
            switch (ev.type) {
                case SRAPI_INPUT_EVENT_KEY_DOWN:
                    if (ev.key.scancode == SRAPI_SCANCODE_ESCAPE) {
                        quit = 1;
                    }
                    break;
                case SRAPI_INPUT_EVENT_MOUSE_MOTION:
                    mouse_x = ev.mouse_motion.x;
                    mouse_y = ev.mouse_motion.y;
                    break;
                case SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT) {
                        if (try_hit(cubes, ev.mouse_button.x, ev.mouse_button.y)) {
                            score++;
                        } else {
                            misclicks++;
                        }
                    }
                    break;
                default:
                    break;
            }
        }

        while (spawn_timer >= SPAWN_INTERVAL_SEC) {
            spawn_timer -= SPAWN_INTERVAL_SEC;
            spawn_cube(cubes, width, height);
            spawned++;
        }

        for (int i = 0; i < MAX_CUBES; i++) {
            if (!cubes[i].active) {
                continue;
            }
            cubes[i].age_sec += dt;
            if (cubes[i].age_sec >= CUBE_LIFETIME_SEC) {
                cubes[i].active = 0;
            }
        }

        srapi_cmd_reset(cmd);
        srapi_cmd_emit(cmd, &(srapi_command_t){
            .kind = SRAPI_COMMAND_CLEAR,
            .color = srapi_rgba(18, 22, 32, 255),
        });

        draw_time_bar(cmd, width, GAME_DURATION_SEC - elapsed);
        draw_score_pips(cmd, width, score);

        for (int i = 0; i < MAX_CUBES; i++) {
            if (cubes[i].active) {
                draw_cube(cmd, &cubes[i]);
            }
        }

        draw_cursor(cmd, mouse_x, mouse_y);

        srapi_framebuffer_t *fb = srapi_drm_backbuffer(drm);
        if (gpu_queue != NULL) {
            r = srapi_queue_submit(gpu_queue, fb, cmd);
        } else {
            r = srapi_submit(ctx, fb, cmd);
        }
        if (r != SRAPI_OK) {
            fprintf(stderr, "click: submit: %s\n", srapi_last_error());
            break;
        }
        r = srapi_drm_present(drm);
        if (r != SRAPI_OK) {
            fprintf(stderr, "click: present: %s\n", srapi_last_error());
            break;
        }
    }

    int alive_at_end = 0;
    for (int i = 0; i < MAX_CUBES; i++) {
        if (cubes[i].active) {
            alive_at_end++;
        }
    }
    int missed = spawned - score - alive_at_end;
    if (missed < 0) missed = 0;
    int total_clicks = score + misclicks;
    float accuracy = total_clicks > 0 ? (100.0f * (float)score / (float)total_clicks) : 0.0f;

    srapi_input_destroy(input);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_context(ctx);
    srapi_destroy_queue(gpu_queue);
    srapi_destroy_device(gpu_device);
    srapi_drm_close(drm);

    printf("\nresults\n");
    if (quit && elapsed < GAME_DURATION_SEC) {
        printf("status:    aborted after %.1fs\n", elapsed);
    } else {
        printf("status:    finished (%.1fs)\n", elapsed);
    }
    printf("score:     %d\n", score);
    printf("spawned:   %d\n", spawned);
    printf("missed:    %d (expired unclicked)\n", missed);
    printf("misclicks: %d\n", misclicks);
    printf("accuracy:  %.1f%% (%d hits / %d clicks)\n", accuracy, score, total_clicks);
    return 0;
}
