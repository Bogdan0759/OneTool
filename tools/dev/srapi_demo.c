#include <srapi/srapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void help(const char *tool) {
    printf("srapi_demo - render a test frame or simple animation with SRAPI\n");
    printf("usage: %s [-o output.ppm] [-w width] [-h height] [--frames n] [--gpu] [--probe-gpu] [--smoke-low] [--smoke-gpu] [--drm [card]] [--hold seconds] [--debug] [--list-displays]\n", tool);
}

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);

    if (text[0] == '\0' || end == NULL || *end != '\0' || value == 0 || value > 8192) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static float wave_u32(uint32_t frame, uint32_t period, float lo, float hi) {
    uint32_t pos = period > 0 ? frame % period : 0;
    float t = period > 1 ? (float)pos / (float)(period - 1) : 0.0f;

    if (t > 0.5f) {
        t = 1.0f - t;
    }
    t *= 2.0f;
    return lo + (hi - lo) * t;
}

static void frame_path(char *buf, size_t buf_size, const char *output, uint32_t frame, uint32_t frames) {
    const char *dot;
    size_t prefix_len;

    if (frames <= 1) {
        snprintf(buf, buf_size, "%s", output);
        return;
    }

    dot = strrchr(output, '.');
    if (dot == NULL) {
        snprintf(buf, buf_size, "%s_%03u.ppm", output, frame);
        return;
    }

    prefix_len = (size_t)(dot - output);
    if (prefix_len > 400) {
        prefix_len = 400;
    }
    snprintf(buf, buf_size, "%.*s_%03u%s", (int)prefix_len, output, frame, dot);
}

static int low_level_smoke(void) {
    srapi_device_t *device = NULL;
    srapi_buffer_t *buffer = NULL;
    srapi_buffer_t *readback_buffer = NULL;
    srapi_image_t *linear_image = NULL;
    srapi_image_t *optimal_image = NULL;
    srapi_queue_t *queue = NULL;
    srapi_context_t *ctx = NULL;
    srapi_framebuffer_t *fb = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    uint32_t value = 0x12345678u;
    uint32_t upload_pixels[16];
    uint32_t readback = 0;
    uint32_t *pixels;
    uint32_t pitch = 0;
    srapi_result_t r;

    r = srapi_create_device(&(srapi_device_desc_t){ .backend = SRAPI_BACKEND_CPU }, &device);
    if (r != SRAPI_OK) goto fail;

    for (size_t i = 0; i < sizeof(upload_pixels) / sizeof(upload_pixels[0]); i++) {
        upload_pixels[i] = srapi_rgba((uint8_t)i, 20, 30, 255);
    }
    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(upload_pixels),
            .usage = SRAPI_BUFFER_TRANSFER_SRC | SRAPI_BUFFER_TRANSFER_DST,
            .initial_data = upload_pixels,
        },
        &buffer
    );
    if (r != SRAPI_OK) goto fail;

    value = 0xaabbccddu;
    r = srapi_buffer_write(buffer, 0, &value, sizeof(value));
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_read(buffer, 0, &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != value) {
        fprintf(stderr, "low-level smoke failed: readback=0x%08x expected=0x%08x\n", readback, value);
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_create_queue(&(srapi_queue_desc_t){ .device = device }, &queue);
    if (r != SRAPI_OK) goto fail;

    r = srapi_buffer_write(buffer, 0, upload_pixels, sizeof(upload_pixels));
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(upload_pixels),
            .usage = SRAPI_BUFFER_TRANSFER_DST,
        },
        &readback_buffer
    );
    if (r != SRAPI_OK) goto fail;

    r = srapi_create_image(
        device,
        &(srapi_image_desc_t){
            .width = 4,
            .height = 4,
            .tiling = SRAPI_IMAGE_LINEAR,
            .usage = SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_SAMPLED,
        },
        &linear_image
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(linear_image, (void **)&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    pixels[0] = srapi_rgba(9, 8, 7, 255);
    srapi_image_unmap(linear_image);

    r = srapi_queue_copy_buffer_to_image(
        queue,
        buffer,
        linear_image,
        &(srapi_buffer_image_copy_t){ .width = 4, .height = 4 }
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_copy_image_to_buffer(
        queue,
        linear_image,
        readback_buffer,
        &(srapi_buffer_image_copy_t){ .width = 4, .height = 4 }
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_read(readback_buffer, sizeof(uint32_t) * 5, &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != upload_pixels[5]) {
        fprintf(stderr, "low-level copy failed: 0x%08x != 0x%08x\n", readback, upload_pixels[5]);
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_create_image(
        device,
        &(srapi_image_desc_t){
            .width = 4,
            .height = 4,
            .tiling = SRAPI_IMAGE_OPTIMAL,
            .usage = SRAPI_IMAGE_COLOR_TARGET,
        },
        &optimal_image
    );
    if (r != SRAPI_OK) goto fail;
    if (srapi_image_map(optimal_image, (void **)&pixels, &pitch) != SRAPI_ERROR_UNSUPPORTED) {
        fprintf(stderr, "low-level smoke failed: optimal image should not map\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = 2,
        .height = 2,
        .backend = SRAPI_BACKEND_CPU,
    }, &ctx);
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_framebuffer(ctx, &(srapi_framebuffer_desc_t){ 2, 2 }, &fb);
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_cmd_buffer(ctx, &cmd);
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_clear(cmd, srapi_rgba(1, 2, 3, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit(queue, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL || pixels[0] != srapi_rgba(1, 2, 3, 255)) {
        fprintf(stderr, "low-level queue submit failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    printf("low-level cpu smoke ok: buffer_size=%zu queue_device=%s\n",
           srapi_buffer_size(buffer), srapi_device_path(srapi_queue_device(queue)));

    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_framebuffer(fb);
    srapi_destroy_context(ctx);
    srapi_destroy_queue(queue);
    srapi_destroy_image(optimal_image);
    srapi_destroy_image(linear_image);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 0;

fail:
    fprintf(stderr, "low-level smoke failed: %s\n", srapi_last_error());
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_framebuffer(fb);
    srapi_destroy_context(ctx);
    srapi_destroy_queue(queue);
    srapi_destroy_image(optimal_image);
    srapi_destroy_image(linear_image);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 1;
}

static int gpu_smoke(void) {
    srapi_device_t *device = NULL;
    srapi_buffer_t *buffer = NULL;
    srapi_buffer_t *readback_buffer = NULL;
    srapi_image_t *image = NULL;
    srapi_queue_t *queue = NULL;
    uint32_t value = 0x55667788u;
    uint32_t upload_pixels[16];
    uint32_t readback = 0;
    uint32_t *pixels = NULL;
    uint32_t pitch = 0;
    srapi_result_t r;

    r = srapi_create_device(&(srapi_device_desc_t){ .backend = SRAPI_BACKEND_GPU }, &device);
    if (r != SRAPI_OK) {
        fprintf(stderr, "gpu smoke unavailable: %s\n", srapi_last_error());
        return 1;
    }

    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = 4096,
            .usage = SRAPI_BUFFER_TRANSFER_SRC | SRAPI_BUFFER_TRANSFER_DST,
            .initial_data = NULL,
        },
        &buffer
    );
    if (r != SRAPI_OK) goto fail;

    r = srapi_buffer_write(buffer, 0, &value, sizeof(value));
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_read(buffer, 0, &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != value) {
        fprintf(stderr, "gpu smoke readback failed: 0x%08x != 0x%08x\n", readback, value);
        r = SRAPI_ERROR;
        goto fail;
    }

    for (size_t i = 0; i < sizeof(upload_pixels) / sizeof(upload_pixels[0]); i++) {
        upload_pixels[i] = srapi_rgba(40, (uint8_t)i, 70, 255);
    }
    r = srapi_buffer_write(buffer, 0, upload_pixels, sizeof(upload_pixels));
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(upload_pixels),
            .usage = SRAPI_BUFFER_TRANSFER_DST,
        },
        &readback_buffer
    );
    if (r != SRAPI_OK) goto fail;

    r = srapi_create_image(
        device,
        &(srapi_image_desc_t){
            .width = 4,
            .height = 4,
            .tiling = SRAPI_IMAGE_LINEAR,
            .usage = SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_PRESENT,
        },
        &image
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(image, (void **)&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    pixels[0] = srapi_rgba(11, 22, 33, 255);
    srapi_image_unmap(image);

    r = srapi_create_queue(&(srapi_queue_desc_t){ .device = device }, &queue);
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_copy_buffer_to_image(
        queue,
        buffer,
        image,
        &(srapi_buffer_image_copy_t){ .width = 4, .height = 4 }
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_copy_image_to_buffer(
        queue,
        image,
        readback_buffer,
        &(srapi_buffer_image_copy_t){ .width = 4, .height = 4 }
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_read(readback_buffer, sizeof(uint32_t) * 10, &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != upload_pixels[10]) {
        fprintf(stderr, "gpu copy readback failed: 0x%08x != 0x%08x\n", readback, upload_pixels[10]);
        r = SRAPI_ERROR;
        goto fail;
    }

    printf("gpu smoke ok: device=%s buffer_size=%zu image=%ux%u pitch=%u backend=%s\n",
           srapi_device_path(device),
           srapi_buffer_size(buffer),
           srapi_image_width(image),
           srapi_image_height(image),
           srapi_image_pitch(image),
           srapi_backend_name(srapi_buffer_backend(buffer)));

    srapi_destroy_queue(queue);
    srapi_destroy_image(image);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 0;

fail:
    fprintf(stderr, "gpu smoke failed: %s\n", srapi_last_error());
    srapi_destroy_queue(queue);
    srapi_destroy_image(image);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 1;
}

int main(int argc, char *argv[]) {
    const char *output = "srapi_demo.ppm";
    const char *drm_device = NULL;
    uint32_t width = 640;
    uint32_t height = 360;
    uint32_t frames = 1;
    int use_gpu = 0;
    int probe_gpu = 0;
    int smoke_low_level = 0;
    int smoke_gpu = 0;
    int use_drm = 0;
    int list_displays = 0;
    int hold_seconds = 5;
    srapi_context_t *ctx = NULL;
    srapi_framebuffer_t *fb = NULL;
    srapi_drm_display_t *drm = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    srapi_shader_t *shader = NULL;
    srapi_result_t r;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &width)) {
                fprintf(stderr, "bad width\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &height)) {
                fprintf(stderr, "bad height\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &frames) || frames > 10000) {
                fprintf(stderr, "bad frames\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--gpu") == 0) {
            use_gpu = 1;
        } else if (strcmp(argv[i], "--probe-gpu") == 0) {
            probe_gpu = 1;
        } else if (strcmp(argv[i], "--smoke-low") == 0) {
            smoke_low_level = 1;
        } else if (strcmp(argv[i], "--smoke-gpu") == 0) {
            smoke_gpu = 1;
        } else if (strcmp(argv[i], "--drm") == 0) {
            use_drm = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                drm_device = argv[++i];
            }
        } else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            uint32_t parsed;
            if (!parse_u32(argv[++i], &parsed) || parsed > 3600) {
                fprintf(stderr, "bad hold seconds\n");
                return 1;
            }
            hold_seconds = (int)parsed;
        } else if (strcmp(argv[i], "--debug") == 0) {
            srapi = 1;
        } else if (strcmp(argv[i], "--list-displays") == 0 || strcmp(argv[i], "--list_displays") == 0) {
            list_displays = 1;
        } else {
            help(argv[0]);
            return 1;
        }
    }

    if (smoke_low_level) {
        return low_level_smoke();
    }

    if (smoke_gpu) {
        return gpu_smoke();
    }

    if (probe_gpu) {
        srapi_device_info_t info;

        r = srapi_probe_device(SRAPI_BACKEND_GPU, &info);
        printf("gpu available=%u backend=%s path=%s message=%s\n",
               info.available,
               srapi_backend_name(info.backend),
               info.path,
               info.message);
        if (r != SRAPI_OK) {
            fprintf(stderr, "gpu probe failed: %s\n", srapi_last_error());
            return 1;
        }
        return 0;
    }

    if (list_displays) {
        srapi_display_info_t displays[16];
        size_t written = 0;

        r = srapi_display_probe_drm(drm_device, displays, 16, &written);
        if (r != SRAPI_OK) {
            fprintf(stderr, "display probe failed: %s\n", srapi_last_error());
            return 1;
        }
        for (size_t i = 0; i < written && i < 16; i++) {
            printf("%s connector=%u type=%u %s modes=%u preferred=%ux%u %s\n",
                   displays[i].device,
                   displays[i].connector_id,
                   displays[i].connector_type,
                   displays[i].connected ? "connected" : "disconnected",
                   displays[i].mode_count,
                   displays[i].preferred_width,
                   displays[i].preferred_height,
                   displays[i].preferred_mode);
        }
        return 0;
    }

    if (use_drm && frames == 1) {
        frames = (uint32_t)hold_seconds * 30;
        if (frames == 0) frames = 1;
    }
    if (use_gpu && !use_drm) {
        fprintf(stderr, "gpu high-level rendering currently needs --drm; falling back to cpu framebuffer\n");
        use_gpu = 0;
    }

    if (use_drm) {
        r = srapi_drm_open(drm_device, &drm);
        if (r != SRAPI_OK) {
            fprintf(stderr, "drm open failed: %s\n", srapi_last_error());
            fprintf(stderr, "hint: run with --debug for full DRM trace\n");
            goto fail;
        }
        width = srapi_drm_width(drm);
        height = srapi_drm_height(drm);
        fb = srapi_drm_backbuffer(drm);
    }

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = width,
        .height = height,
        .backend = use_gpu ? SRAPI_BACKEND_GPU : SRAPI_BACKEND_CPU,
    }, &ctx);
    if (r == SRAPI_ERROR_UNSUPPORTED && use_gpu) {
        fprintf(stderr, "gpu backend unavailable: %s\n", srapi_last_error());
        fprintf(stderr, "falling back to cpu backend\n");
        r = srapi_create_context(&(srapi_context_desc_t){
            .width = width,
            .height = height,
            .backend = SRAPI_BACKEND_CPU,
        }, &ctx);
    }
    if (r != SRAPI_OK) goto fail;
    if (!use_drm) {
        r = srapi_create_framebuffer(ctx, &(srapi_framebuffer_desc_t){ width, height }, &fb);
        if (r != SRAPI_OK) goto fail;
    }
    r = srapi_create_cmd_buffer(ctx, &cmd);
    if (r != SRAPI_OK) goto fail;

    const uint32_t shader_code[] = {
        SRAPI_VM_LOAD_INPUT, 0, SRAPI_VM_INPUT_U,
        SRAPI_VM_LOAD_INPUT, 1, SRAPI_VM_INPUT_V,
        SRAPI_VM_LOAD_UNIFORM, 2, 0,
        SRAPI_VM_ADD, 0, 0, 2,
        SRAPI_VM_FRACT, 0, 0,
        SRAPI_VM_LOAD_UNIFORM, 3, 1,
        SRAPI_VM_LOAD_UNIFORM, 4, 2,
        SRAPI_VM_OUT_COLOR, 0, 1, 3, 4,
        SRAPI_VM_END,
    };
    const float shader_uniforms[] = { 0.0f, 0.85f, 1.0f };
    r = srapi_create_shader(
        shader_code,
        sizeof(shader_code) / sizeof(shader_code[0]),
        shader_uniforms,
        sizeof(shader_uniforms) / sizeof(shader_uniforms[0]),
        &shader
    );
    if (r != SRAPI_OK) goto fail;

    srapi_cmd_emit(cmd, &(srapi_command_t){
        .kind = SRAPI_COMMAND_CLEAR,
        .color = srapi_rgba(10, 12, 16, 255),
    });
    srapi_cmd_shade_rect(cmd, 0, 0, width, height, shader);
    srapi_cmd_fill_rect(cmd, 40, 40, width / 2, height / 3, srapi_rgba(220, 80, 60, 255));
    srapi_cmd_fill_rect(cmd, (int32_t)(width / 3), (int32_t)(height / 3), width / 2, height / 3, srapi_rgba(55, 160, 230, 255));
    srapi_cmd_fill_triangle(
        cmd,
        (int32_t)(width / 2), 24,
        32, (int32_t)height - 32,
        (int32_t)width - 32, (int32_t)height - 48,
        srapi_rgba(180, 230, 80, 255)
    );
    srapi_cmd_draw_line(cmd, 0, 0, (int32_t)width - 1, (int32_t)height - 1, srapi_rgba(245, 220, 90, 255));
    srapi_cmd_draw_line(cmd, 0, (int32_t)height - 1, (int32_t)width - 1, 0, srapi_rgba(90, 245, 170, 255));

    for (uint32_t frame = 0; frame < frames; frame++) {
        float shift = wave_u32(frame, 24, 0.0f, 0.45f);
        float blue = wave_u32(frame + 8, 32, 0.45f, 1.0f);
        float uniforms[] = { shift, blue, 1.0f };

        r = srapi_shader_set_uniforms(shader, 0, uniforms, sizeof(uniforms) / sizeof(uniforms[0]));
        if (r != SRAPI_OK) goto fail;

        if (use_drm) {
            fb = srapi_drm_backbuffer(drm);
        }

        r = srapi_submit(ctx, fb, cmd);
        if (r != SRAPI_OK) goto fail;

        if (use_drm) {
            r = srapi_drm_present(drm);
            if (r != SRAPI_OK) goto fail;
            //usleep(16000);
        } else {
            char path[512];

            frame_path(path, sizeof(path), output, frame, frames);
            r = srapi_save_ppm(fb, path);
            if (r != SRAPI_OK) goto fail;
        }
    }

    if (use_drm) {
        printf("rendered %u frames to DRM (%ux%u)\n", frames, width, height);
    } else if (frames == 1) {
        printf("wrote %s (%ux%u)\n", output, width, height);
    } else {
        printf("wrote %u frames from %s (%ux%u)\n", frames, output, width, height);
    }

    srapi_destroy_shader(shader);
    srapi_destroy_cmd_buffer(cmd);
    if (!use_drm) {
        srapi_destroy_framebuffer(fb);
    }
    srapi_drm_close(drm);
    srapi_destroy_context(ctx);
    return 0;

fail:
    fprintf(stderr, "srapi_demo failed: %d\n", r);
    srapi_destroy_shader(shader);
    srapi_destroy_cmd_buffer(cmd);
    if (!use_drm) {
        srapi_destroy_framebuffer(fb);
    }
    srapi_drm_close(drm);
    srapi_destroy_context(ctx);
    return 1;
}
