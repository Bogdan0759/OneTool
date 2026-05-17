#include <srapi/srapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void help(const char *tool) {
    printf("srapi_demo - render a test frame or simple animation with SRAPI\n");
    printf("usage: %s [-o output.ppm] [-w width] [-h height] [--frames n] [--gpu] [--tile-cache] [--probe-gpu] [--probe-i915 [node]] [--probe-fbdev] [--smoke-low] [--smoke-gpu] [--smoke-i915 [node]] [--smoke-fbdev] [--drm [card]] [--fbdev [fb]] [--hold seconds] [--debug] [--list-displays] [--list-modes [connector]]\n", tool);
    printf("i915 screen rendering: %s --gpu --drm [/dev/dri/cardN] [--tile-cache]\n", tool);
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

static int has_ext(const char *path, const char *ext) {
    size_t path_len = strlen(path);
    size_t ext_len = strlen(ext);

    if (path_len < ext_len) {
        return 0;
    }
    return strcmp(path + path_len - ext_len, ext) == 0;
}

static srapi_result_t save_framebuffer(srapi_framebuffer_t *fb, const char *path) {
    if (has_ext(path, ".bmp") || has_ext(path, ".BMP")) {
        return srapi_save_bmp(fb, path);
    }
    return srapi_save_ppm(fb, path);
}

static int low_level_smoke(void) {
    srapi_device_t *device = NULL;
    srapi_buffer_t *buffer = NULL;
    srapi_buffer_t *readback_buffer = NULL;
    srapi_buffer_t *vertex_buffer = NULL;
    srapi_buffer_t *index_buffer = NULL;
    srapi_image_t *linear_image = NULL;
    srapi_image_t *optimal_image = NULL;
    srapi_image_view_t *view = NULL;
    srapi_queue_t *queue = NULL;
    srapi_context_t *ctx = NULL;
    srapi_framebuffer_t *fb = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    srapi_shader_t *vertex_shader = NULL;
    uint32_t value = 0x12345678u;
    uint32_t upload_pixels[16];
    srapi_vertex_t triangle_vertices[3] = {
        { 0.0f, 0.0f, 0 },
        { 2.0f, 0.0f, 0 },
        { 0.0f, 2.0f, 0 },
    };
    uint32_t triangle_indices[3] = { 0, 1, 2 };
    srapi_color_t triangle_color = srapi_rgba(220, 70, 40, 255);
    uint32_t readback = 0;
    uint32_t *pixels;
    uint32_t pitch = 0;
    srapi_backend_config_t backend_config;
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
            .usage = SRAPI_IMAGE_TRANSFER_SRC | SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_SAMPLED,
        },
        &linear_image
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(linear_image, (void **)&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    pixels[0] = srapi_rgba(9, 8, 7, 255);
    srapi_image_unmap(linear_image);

    r = srapi_create_image_view(&(srapi_image_view_desc_t){
        .image = linear_image,
        .x = 1,
        .y = 1,
        .width = 2,
        .height = 2,
    }, &view);
    if (r != SRAPI_OK) goto fail;
    if (srapi_image_view_width(view) != 2 ||
        srapi_image_view_height(view) != 2 ||
        srapi_image_view_backend(view) != SRAPI_BACKEND_CPU) {
        fprintf(stderr, "low-level image view metadata failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }
    r = srapi_image_view_map(view, (void **)&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    pixels[0] = srapi_rgba(88, 77, 66, 255);
    srapi_image_view_unmap(view);
    srapi_destroy_image_view(view);
    view = NULL;

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

    r = srapi_get_backend_config(SRAPI_BACKEND_CPU, &backend_config);
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_write(readback_buffer, 0, upload_pixels, sizeof(upload_pixels));
    if (r != SRAPI_OK) goto fail;
    r = srapi_set_backend_config(&(srapi_backend_config_t){
        .backend = SRAPI_BACKEND_CPU,
        .checks = backend_config.checks & ~SRAPI_BACKEND_CHECK_USAGE,
    });
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_copy_buffer_to_image(
        queue,
        readback_buffer,
        linear_image,
        &(srapi_buffer_image_copy_t){ .width = 4, .height = 4 }
    );
    srapi_set_backend_config(&backend_config);
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(linear_image, (void **)&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    readback = pixels[7];
    srapi_image_unmap(linear_image);
    if (readback != upload_pixels[7]) {
        fprintf(stderr, "low-level backend config failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_create_image(
        device,
        &(srapi_image_desc_t){
            .width = 4,
            .height = 4,
            .tiling = SRAPI_IMAGE_OPTIMAL,
            .usage = SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_COLOR_TARGET,
        },
        &optimal_image
    );
    if (r != SRAPI_OK) goto fail;
    if (srapi_image_map(optimal_image, (void **)&pixels, &pitch) != SRAPI_ERROR_UNSUPPORTED) {
        fprintf(stderr, "low-level smoke failed: optimal image should not map\n");
        r = SRAPI_ERROR;
        goto fail;
    }
    r = srapi_queue_copy_buffer_to_image(
        queue,
        buffer,
        optimal_image,
        &(srapi_buffer_image_copy_t){ .width = 4, .height = 4 }
    );
    if (r != SRAPI_OK) goto fail;

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = 4,
        .height = 4,
        .backend = SRAPI_BACKEND_CPU,
    }, &ctx);
    if (r != SRAPI_OK) goto fail;
    r = srapi_context_get_backend_config(ctx, &backend_config);
    if (r != SRAPI_OK) goto fail;
    r = srapi_context_set_backend_config(ctx, &(srapi_backend_config_t){
        .backend = SRAPI_BACKEND_CPU,
        .checks = backend_config.checks & ~SRAPI_BACKEND_CHECK_USAGE,
    });
    if (r != SRAPI_OK) goto fail;
    r = srapi_context_set_backend_config(ctx, &backend_config);
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_framebuffer(ctx, &(srapi_framebuffer_desc_t){ 4, 4 }, &fb);
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_cmd_buffer(ctx, &cmd);
    if (r != SRAPI_OK) goto fail;
    for (size_t i = 0; i < 3; i++) {
        triangle_vertices[i].color = triangle_color;
    }
    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(triangle_vertices),
            .usage = SRAPI_BUFFER_VERTEX,
            .initial_data = triangle_vertices,
        },
        &vertex_buffer
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(triangle_indices),
            .usage = SRAPI_BUFFER_INDEX,
            .initial_data = triangle_indices,
        },
        &index_buffer
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_clear(cmd, srapi_rgba(1, 2, 3, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_draw_vertices(
        cmd,
        SRAPI_PRIMITIVE_TRIANGLES,
        vertex_buffer, 0, 3,
        index_buffer, 0, 3
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit(queue, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL || pixels[0] != triangle_color) {
        fprintf(stderr, "low-level queue submit failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    const uint32_t vertex_shader_code[] = {
        SRAPI_VM_LOAD_INPUT, 0, SRAPI_VM_INPUT_VERTEX_R,
        SRAPI_VM_LOAD_INPUT, 1, SRAPI_VM_INPUT_VERTEX_G,
        SRAPI_VM_LOAD_INPUT, 2, SRAPI_VM_INPUT_VERTEX_B,
        SRAPI_VM_LOAD_INPUT, 3, SRAPI_VM_INPUT_VERTEX_A,
        SRAPI_VM_OUT_COLOR, 0, 1, 2, 3,
        SRAPI_VM_END,
    };
    r = srapi_create_shader(
        vertex_shader_code,
        sizeof(vertex_shader_code) / sizeof(vertex_shader_code[0]),
        NULL,
        0,
        &vertex_shader
    );
    if (r != SRAPI_OK) goto fail;

    srapi_vertex_t point = { 1.0f, 1.0f, srapi_rgba(128, 64, 32, 255) };
    r = srapi_buffer_write(vertex_buffer, 0, &point, sizeof(point));
    if (r != SRAPI_OK) goto fail;
    srapi_cmd_reset(cmd);
    r = srapi_cmd_clear(cmd, srapi_rgba(0, 0, 0, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_draw_vertices_shader(
        cmd,
        SRAPI_PRIMITIVE_POINTS,
        vertex_buffer, 0, 1,
        NULL, 0, 0,
        vertex_shader
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit(queue, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL ||
        pixels[1 + (srapi_framebuffer_pitch(fb) / sizeof(uint32_t))] != point.color) {
        fprintf(stderr, "low-level vertex shader color failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    srapi_cmd_reset(cmd);
    r = srapi_cmd_clear(cmd, srapi_rgba(0, 0, 0, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_set_scissor(cmd, 1, 1, 1, 1);
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_fill_rect(cmd, 0, 0, 4, 4, srapi_rgba(0, 200, 0, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit(queue, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL ||
        pixels[0] != srapi_rgba(0, 0, 0, 255) ||
        pixels[1 + (srapi_framebuffer_pitch(fb) / sizeof(uint32_t))] != srapi_rgba(0, 200, 0, 255)) {
        fprintf(stderr, "low-level scissor failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    srapi_cmd_reset(cmd);
    r = srapi_cmd_clear(cmd, srapi_rgba(0, 0, 0, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_set_blend(cmd, SRAPI_BLEND_ALPHA);
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_fill_rect(cmd, 0, 0, 1, 1, srapi_rgba(255, 0, 0, 128));
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit(queue, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL || pixels[0] != srapi_rgba(128, 0, 0, 255)) {
        fprintf(stderr, "low-level alpha blend failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    point.x = 0.0f;
    point.y = 0.0f;
    point.color = srapi_rgba(40, 50, 60, 255);
    r = srapi_buffer_write(vertex_buffer, 0, &point, sizeof(point));
    if (r != SRAPI_OK) goto fail;
    srapi_cmd_reset(cmd);
    r = srapi_cmd_clear(cmd, srapi_rgba(0, 0, 0, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_set_viewport(cmd, 0, 0, 4, 4);
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_draw_vertices(cmd, SRAPI_PRIMITIVE_POINTS, vertex_buffer, 0, 1, NULL, 0, 0);
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit(queue, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL ||
        pixels[2 + 2 * (srapi_framebuffer_pitch(fb) / sizeof(uint32_t))] != point.color) {
        fprintf(stderr, "low-level viewport failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }
    r = srapi_save_bmp(fb, "/tmp/srapi_smoke.bmp");
    if (r != SRAPI_OK) goto fail;

    printf("low-level cpu smoke ok: buffer_size=%zu queue_device=%s\n",
           srapi_buffer_size(buffer), srapi_device_path(srapi_queue_device(queue)));

    srapi_destroy_shader(vertex_shader);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_framebuffer(fb);
    srapi_destroy_context(ctx);
    srapi_destroy_queue(queue);
    srapi_destroy_image_view(view);
    srapi_destroy_image(optimal_image);
    srapi_destroy_image(linear_image);
    srapi_destroy_buffer(index_buffer);
    srapi_destroy_buffer(vertex_buffer);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 0;

fail:
    fprintf(stderr, "low-level smoke failed: %s\n", srapi_last_error());
    srapi_destroy_shader(vertex_shader);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_framebuffer(fb);
    srapi_destroy_context(ctx);
    srapi_destroy_queue(queue);
    srapi_destroy_image_view(view);
    srapi_destroy_image(optimal_image);
    srapi_destroy_image(linear_image);
    srapi_destroy_buffer(index_buffer);
    srapi_destroy_buffer(vertex_buffer);
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
    srapi_image_view_t *view = NULL;
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
            .usage = SRAPI_IMAGE_TRANSFER_SRC | SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_PRESENT,
        },
        &image
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(image, (void **)&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    pixels[0] = srapi_rgba(11, 22, 33, 255);
    srapi_image_unmap(image);

    r = srapi_create_image_view(&(srapi_image_view_desc_t){
        .image = image,
        .x = 1,
        .y = 1,
        .width = 2,
        .height = 2,
    }, &view);
    if (r != SRAPI_OK) goto fail;
    if (srapi_image_view_backend(view) != SRAPI_BACKEND_GPU ||
        srapi_image_view_width(view) != 2 ||
        srapi_image_view_height(view) != 2) {
        fprintf(stderr, "gpu image view metadata failed\n");
        r = SRAPI_ERROR;
        goto fail;
    }
    r = srapi_image_view_map(view, (void ** )&pixels, &pitch);
    if (r != SRAPI_OK) goto fail;
    pixels[0] = srapi_rgba(99, 44, 22, 255);
    srapi_image_view_unmap(view);

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
    srapi_destroy_image_view(view);
    srapi_destroy_image(image);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 0;

fail:
    fprintf(stderr, "gpu smoke failed: %s\n", srapi_last_error());
    srapi_destroy_queue(queue);
    srapi_destroy_image_view(view);
    srapi_destroy_image(image);
    srapi_destroy_buffer(readback_buffer);
    srapi_destroy_buffer(buffer);
    srapi_destroy_device(device);
    return 1;
}

static int i915_smoke(const char *device_path) {
    srapi_i915_info_t info;
    srapi_device_t *device = NULL;
    srapi_queue_t *queue = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    srapi_buffer_t *buffer = NULL;
    srapi_image_t *image = NULL;
    uint32_t values[4] = { 0x10203040u, 0x55667788u, 0xaabbccddu, 0x13572468u };
    uint32_t fill_color = 0xff3366ccu;
    uint32_t image_color = 0xffcc6633u;
    uint32_t readback = 0;
    uint32_t *mapped = NULL;
    uint32_t pitch = 0;
    srapi_result_t r;

    r = srapi_probe_i915(device_path, &info);
    if (r != SRAPI_OK) {
        fprintf(stderr, "i915 smoke unavailable: %s\n", srapi_last_error());
        return 1;
    }

    r = srapi_create_device(&(srapi_device_desc_t){
        .backend = SRAPI_BACKEND_GPU,
        .device_path = info.path,
    }, &device);
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_queue(&(srapi_queue_desc_t){ .device = device }, &queue);
    if (r != SRAPI_OK) goto fail;
    r = srapi_create_cmd_buffer(NULL, &cmd);
    if (r != SRAPI_OK) goto fail;

    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = 4096,
            .usage = SRAPI_BUFFER_TRANSFER_SRC | SRAPI_BUFFER_TRANSFER_DST | SRAPI_BUFFER_STORAGE,
        },
        &buffer
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_write(buffer, 0, values, sizeof(values));
    if (r != SRAPI_OK) goto fail;

    r = srapi_buffer_read(buffer, sizeof(uint32_t), &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != values[1]) {
        fprintf(stderr, "i915 smoke readback failed: 0x%08x != 0x%08x\n", readback, values[1]);
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_buffer_map(buffer, (void **)&mapped);
    if (r != SRAPI_OK) goto fail;
    mapped[2] = values[3];
    srapi_buffer_unmap(buffer);

    r = srapi_buffer_read(buffer, sizeof(uint32_t) * 2, &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != values[3]) {
        fprintf(stderr, "i915 smoke map/read failed: 0x%08x != 0x%08x\n", readback, values[3]);
        r = SRAPI_ERROR;
        goto fail;
    }

    srapi_destroy_buffer(buffer);
    buffer = NULL;

    r = srapi_i915_submit_noop(device);
    if (r != SRAPI_OK) goto fail;

    {
        uint32_t raw_batch_words[2] = { 0x05000000u, 0x00000000u };
        srapi_i915_exec_object_t raw_obj;
        srapi_i915_exec_desc_t raw_exec;

        r = srapi_create_buffer(
            device,
            &(srapi_buffer_desc_t){
                .size = sizeof(raw_batch_words),
                .usage = SRAPI_BUFFER_STORAGE,
                .initial_data = raw_batch_words,
            },
            &buffer
        );
        if (r != SRAPI_OK) goto fail;
        r = srapi_i915_set_domain(device, buffer, SRAPI_I915_DOMAIN_CPU, SRAPI_I915_DOMAIN_CPU);
        if (r != SRAPI_OK) goto fail;

        memset(&raw_obj, 0, sizeof(raw_obj));
        raw_obj.buffer = buffer;
        raw_obj.flags = SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

        memset(&raw_exec, 0, sizeof(raw_exec));
        raw_exec.batch = buffer;
        raw_exec.batch_len = sizeof(raw_batch_words);
        raw_exec.flags = SRAPI_I915_EXEC_RENDER;
        raw_exec.objects = &raw_obj;
        raw_exec.object_count = 1;
        r = srapi_i915_exec(device, &raw_exec);
        if (r != SRAPI_OK) goto fail;

        srapi_destroy_buffer(buffer);
        buffer = NULL;
    }

    r = srapi_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = 64 * 64 * sizeof(uint32_t),
            .usage = SRAPI_BUFFER_TRANSFER_SRC | SRAPI_BUFFER_TRANSFER_DST | SRAPI_BUFFER_STORAGE,
        },
        &buffer
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_i915_fill_buffer(device, buffer, 0, 0, 64, 64, 64 * sizeof(uint32_t), fill_color);
    if (r != SRAPI_OK) goto fail;
    r = srapi_buffer_read(buffer, 17 * sizeof(uint32_t), &readback, sizeof(readback));
    if (r != SRAPI_OK) goto fail;
    if (readback != fill_color) {
        fprintf(stderr, "i915 smoke fill failed: 0x%08x != 0x%08x\n", readback, fill_color);
        r = SRAPI_ERROR;
        goto fail;
    }

    srapi_destroy_buffer(buffer);
    buffer = NULL;

    r = srapi_create_image(
        device,
        &(srapi_image_desc_t){
            .width = 32,
            .height = 32,
            .tiling = SRAPI_IMAGE_LINEAR,
            .usage = SRAPI_IMAGE_TRANSFER_SRC | SRAPI_IMAGE_TRANSFER_DST | SRAPI_IMAGE_COLOR_TARGET,
        },
        &image
    );
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_fill_image(queue, image, image_color);
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(image, (void **)&mapped, &pitch);
    if (r != SRAPI_OK) goto fail;
    readback = mapped[7 + 9 * (pitch / sizeof(uint32_t))];
    srapi_image_unmap(image);
    if (readback != image_color) {
        fprintf(stderr, "i915 smoke image fill failed: 0x%08x != 0x%08x\n", readback, image_color);
        r = SRAPI_ERROR;
        goto fail;
    }

    srapi_cmd_reset(cmd);
    r = srapi_cmd_clear(cmd, srapi_rgba(10, 20, 30, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_set_scissor(cmd, 4, 4, 16, 16);
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_fill_rect(cmd, 0, 0, 32, 32, srapi_rgba(90, 110, 130, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_queue_submit_image(queue, image, cmd);
    if (r != SRAPI_OK) goto fail;
    r = srapi_image_map(image, (void **)&mapped, &pitch);
    if (r != SRAPI_OK) goto fail;
    readback = mapped[0];
    if (readback != srapi_rgba(10, 20, 30, 255)) {
        fprintf(stderr, "i915 render clear failed: 0x%08x\n", readback);
        srapi_image_unmap(image);
        r = SRAPI_ERROR;
        goto fail;
    }
    readback = mapped[8 + 8 * (pitch / sizeof(uint32_t))];
    srapi_image_unmap(image);
    if (readback != srapi_rgba(90, 110, 130, 255)) {
        fprintf(stderr, "i915 render fill_rect failed: 0x%08x\n", readback);
        r = SRAPI_ERROR;
        goto fail;
    }

    printf("i915 smoke ok: path=%s chipset=0x%x gem=%u execbuf2=%u blt=%u buffer_size=%u noop_batch=%zu fill_batch=%zu raw_exec=1 noop_submit=1 blt_fill=1 image_fill=1 render_clear=1 render_fill_rect=1\n",
           info.path,
           info.chipset_id,
           info.has_gem,
           info.has_execbuf2,
           info.has_blt,
           4096u,
           sizeof(uint32_t) * 2,
           sizeof(uint32_t) * 8);

    srapi_destroy_image(image);
    srapi_destroy_buffer(buffer);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_queue(queue);
    srapi_destroy_device(device);
    return 0;

fail:
    fprintf(stderr, "i915 smoke failed: %s\n", srapi_last_error());
    srapi_destroy_image(image);
    srapi_destroy_buffer(buffer);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_queue(queue);
    srapi_destroy_device(device);
    return 1;
}

static int fbdev_smoke(void) {
    srapi_context_t *ctx = NULL;
    srapi_framebuffer_t *fb = NULL;
    srapi_cmd_buffer_t *cmd = NULL;
    uint32_t *pixels;
    srapi_result_t r;

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = 3,
        .height = 2,
        .backend = SRAPI_BACKEND_FBDEV,
    }, &ctx);
    if (r != SRAPI_OK) goto fail;
    if (srapi_context_backend(ctx) != SRAPI_BACKEND_FBDEV) {
        fprintf(stderr, "fbdev smoke failed: bad context backend\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_create_framebuffer(ctx, &(srapi_framebuffer_desc_t){ 3, 2 }, &fb);
    if (r != SRAPI_OK) goto fail;
    if (srapi_framebuffer_pitch(fb) != 3 * sizeof(uint32_t)) {
        fprintf(stderr, "fbdev smoke failed: bad pitch\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    r = srapi_create_cmd_buffer(ctx, &cmd);
    if (r != SRAPI_OK) goto fail;
    r = srapi_cmd_clear(cmd, srapi_rgba(4, 5, 6, 255));
    if (r != SRAPI_OK) goto fail;
    r = srapi_submit(ctx, fb, cmd);
    if (r != SRAPI_OK) goto fail;

    pixels = srapi_framebuffer_pixels(fb);
    if (pixels == NULL || pixels[0] != srapi_rgba(4, 5, 6, 255)) {
        fprintf(stderr, "fbdev smoke failed: render mismatch\n");
        r = SRAPI_ERROR;
        goto fail;
    }

    printf("fbdev smoke ok: backend=%s test_framebuffer=%ux%u\n",
           srapi_backend_name(srapi_context_backend(ctx)),
           srapi_framebuffer_width(fb),
           srapi_framebuffer_height(fb));

    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_framebuffer(fb);
    srapi_destroy_context(ctx);
    return 0;

fail:
    fprintf(stderr, "fbdev smoke failed: %s\n", srapi_last_error());
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_framebuffer(fb);
    srapi_destroy_context(ctx);
    return 1;
}

int main(int argc, char *argv[]) {
    const char *output = "srapi_demo.ppm";
    const char *drm_device = NULL;
    const char *fbdev_device = NULL;
    uint32_t width = 640;
    uint32_t height = 360;
    uint32_t frames = 1;
    int width_set = 0;
    int height_set = 0;
    int use_gpu = 0;
    int probe_gpu = 0;
    int probe_i915 = 0;
    int probe_fbdev = 0;
    int smoke_low_level = 0;
    int smoke_gpu = 0;
    int smoke_i915 = 0;
    int smoke_fbdev = 0;
    int use_drm = 0;
    int use_fbdev = 0;
    int use_i915_tile_cache = 0;
    uint32_t tile_cols = 4;
    uint32_t tile_rows = 4;
    int list_displays = 0;
    int list_modes = 0;
    uint32_t list_modes_connector = 0;
    const char *i915_device = NULL;
    int hold_seconds = 5;
    srapi_context_t *ctx = NULL;
    srapi_framebuffer_t *fb = NULL;
    srapi_drm_display_t *drm = NULL;
    srapi_fbdev_display_t *fbdev = NULL;
    srapi_device_t *gpu_device = NULL;
    srapi_queue_t *gpu_queue = NULL;
    int gpu_i915 = 0;
    srapi_cmd_buffer_t *cmd = NULL;
    srapi_shader_t *shader = NULL;
    srapi_backend_t render_backend = SRAPI_BACKEND_CPU;
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
            width_set = 1;
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &height)) {
                fprintf(stderr, "bad height\n");
                return 1;
            }
            height_set = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &frames) || frames > 10000) {
                fprintf(stderr, "bad frames\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--gpu") == 0) {
            use_gpu = 1;
        } else if (strcmp(argv[i], "--tile-cache") == 0) {
            use_i915_tile_cache = 1;
        } else if (strcmp(argv[i], "--tile-cols") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &tile_cols)) {
                fprintf(stderr, "bad tile cols\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--tile-rows") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &tile_rows)) {
                fprintf(stderr, "bad tile rows\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--probe-gpu") == 0) {
            probe_gpu = 1;
        } else if (strcmp(argv[i], "--probe-i915") == 0) {
            probe_i915 = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i915_device = argv[++i];
            }
        } else if (strcmp(argv[i], "--probe-fbdev") == 0 || strcmp(argv[i], "--probe_fbdev") == 0) {
            probe_fbdev = 1;
        } else if (strcmp(argv[i], "--smoke-low") == 0) {
            smoke_low_level = 1;
        } else if (strcmp(argv[i], "--smoke-gpu") == 0) {
            smoke_gpu = 1;
        } else if (strcmp(argv[i], "--smoke-i915") == 0) {
            smoke_i915 = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                i915_device = argv[++i];
            }
        } else if (strcmp(argv[i], "--smoke-fbdev") == 0 || strcmp(argv[i], "--smoke_fbdev") == 0) {
            smoke_fbdev = 1;
        } else if (strcmp(argv[i], "--drm") == 0) {
            use_drm = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                drm_device = argv[++i];
            }
        } else if (strcmp(argv[i], "--fbdev") == 0) {
            use_fbdev = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                fbdev_device = argv[++i];
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
        } else if (strcmp(argv[i], "--list-modes") == 0 || strcmp(argv[i], "--list_modes") == 0) {
            list_modes = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!parse_u32(argv[++i], &list_modes_connector)) {
                    fprintf(stderr, "bad connector id\n");
                    return 1;
                }
            }
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

    if (smoke_i915) {
        return i915_smoke(i915_device);
    }

    if (smoke_fbdev) {
        return fbdev_smoke();
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

    if (probe_i915) {
        srapi_i915_info_t info;

        r = srapi_probe_i915(i915_device, &info);
        printf("i915 available=%u usable=%u path=%s chipset=0x%x gem=%u execbuf2=%u blt=%u fence=%u cs_timestamp_frequency=%u\n",
               info.available,
               info.available && info.has_gem && info.has_execbuf2 && info.has_blt,
               info.path,
               info.chipset_id,
               info.has_gem,
               info.has_execbuf2,
               info.has_blt,
               info.has_exec_fence,
               info.cs_timestamp_frequency);
        if (r != SRAPI_OK) {
            fprintf(stderr, "i915 probe failed: %s\n", srapi_last_error());
            return 1;
        }
        return 0;
    }

    if (probe_fbdev) {
        srapi_device_info_t info;

        r = srapi_probe_device(SRAPI_BACKEND_FBDEV, &info);
        printf("fbdev available=%u backend=%s path=%s message=%s\n",
               info.available,
               srapi_backend_name(info.backend),
               info.path,
               info.message);
        if (r != SRAPI_OK) {
            fprintf(stderr, "fbdev probe failed: %s\n", srapi_last_error());
            return 1;
        }
        return 0;
    }

    if (list_modes) {
        srapi_display_mode_t modes[64];
        size_t written = 0;

        r = srapi_display_list_modes_drm(drm_device, list_modes_connector, modes, 64, &written);
        if (r != SRAPI_OK) {
            fprintf(stderr, "mode list failed: %s\n", srapi_last_error());
            return 1;
        }
        for (size_t i = 0; i < written && i < 64; i++) {
            printf("%ux%u %.3fHz %s flags=0x%x type=0x%x\n",
                   modes[i].width,
                   modes[i].height,
                   (double)modes[i].refresh_millihz / 1000.0,
                   modes[i].name,
                   modes[i].flags,
                   modes[i].type);
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

    if (use_drm && use_fbdev) {
        fprintf(stderr, "--drm and --fbdev cannot be used together\n");
        return 1;
    }
    if (use_i915_tile_cache && !use_fbdev && (!use_gpu || !use_drm)) {
        fprintf(stderr, "--tile-cache requires --gpu --drm or --fbdev\n");
        return 1;
    }

    if ((use_drm || use_fbdev) && frames == 1) {
        frames = (uint32_t)hold_seconds * 30;
        if (frames == 0) frames = 1;
    }
    if (use_gpu && !use_drm) {
        fprintf(stderr, "gpu high-level rendering currently needs --drm; falling back to cpu framebuffer\n");
        use_gpu = 0;
    }

    if (use_drm) {
        r = srapi_drm_open_display(&(srapi_drm_display_desc_t){
            .device_path = drm_device,
            .width = width_set && height_set ? width : 0,
            .height = width_set && height_set ? height : 0,
        }, &drm);
        if (r != SRAPI_OK) {
            fprintf(stderr, "drm open failed: %s\n", srapi_last_error());
            fprintf(stderr, "hint: run with --debug for full DRM trace\n");
            goto fail;
        }
        width = srapi_drm_width(drm);
        height = srapi_drm_height(drm);
        fb = srapi_drm_backbuffer(drm);
    }
    if (use_gpu && use_drm) {
        const char *gpu_path = drm_device != NULL ? drm_device : srapi_drm_device_path(drm);

        r = srapi_create_device(&(srapi_device_desc_t){
            .backend = SRAPI_BACKEND_GPU,
            .device_path = gpu_path,
        }, &gpu_device);
        if (r == SRAPI_OK) {
            srapi_i915_info_t gpu_i915_info;

            gpu_i915 = srapi_probe_i915(gpu_path, &gpu_i915_info) == SRAPI_OK &&
                       gpu_i915_info.available &&
                       gpu_i915_info.has_gem &&
                       gpu_i915_info.has_execbuf2 &&
                       gpu_i915_info.has_blt;
            if (use_i915_tile_cache) {
                if (!gpu_i915) {
                    fprintf(stderr, "--tile-cache requires i915 with GEM/EXECBUF2/BLT\n");
                    goto fail;
                }
                r = srapi_device_set_tile_cache_config(gpu_device, tile_cols, tile_rows);
                if (r != SRAPI_OK) {
                    fprintf(stderr, "tile cache config failed: %s\n", srapi_last_error());
                    goto fail;
                }
                r = srapi_i915_set_tile_cache_enabled(gpu_device, 1);
                if (r != SRAPI_OK) {
                    fprintf(stderr, "tile cache enable failed: %s\n", srapi_last_error());
                    goto fail;
                }
            }
            r = srapi_create_queue(&(srapi_queue_desc_t){
                .device = gpu_device,
                .family_index = 0,
            }, &gpu_queue);
        }
        if (r != SRAPI_OK) {
            fprintf(stderr, "gpu queue unavailable: %s\n", srapi_last_error());
            srapi_destroy_queue(gpu_queue);
            srapi_destroy_device(gpu_device);
            gpu_queue = NULL;
            gpu_device = NULL;
            gpu_i915 = 0;
            goto fail;
        }
    }
    if (use_fbdev) {
        r = srapi_fbdev_open_display(&(srapi_fbdev_display_desc_t){
            .device_path = fbdev_device,
        }, &fbdev);
        if (r != SRAPI_OK) {
            fprintf(stderr, "fbdev open failed: %s\n", srapi_last_error());
            fprintf(stderr, "hint: fbdev only supports mapped 32bpp /dev/fb* for now\n");
            goto fail;
        }
        width = srapi_fbdev_width(fbdev);
        height = srapi_fbdev_height(fbdev);
        fb = srapi_fbdev_framebuffer(fbdev);
        if (use_i915_tile_cache) {
            r = srapi_fbdev_set_tile_cache_config(fbdev, tile_cols, tile_rows);
            if (r != SRAPI_OK) {
                fprintf(stderr, "fbdev tile cache config failed: %s\n", srapi_last_error());
                goto fail;
            }
            r = srapi_fbdev_set_tile_cache_enabled(fbdev, 1);
            if (r != SRAPI_OK) {
                fprintf(stderr, "fbdev tile cache enable failed: %s\n", srapi_last_error());
                goto fail;
            }
        }
    }

    if (use_drm) {
        render_backend = SRAPI_BACKEND_GPU;
    } else if (use_fbdev) {
        render_backend = SRAPI_BACKEND_FBDEV;
    } else if (use_gpu) {
        render_backend = SRAPI_BACKEND_GPU;
    }

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = width,
        .height = height,
        .backend = render_backend,
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
    if (!use_drm && !use_fbdev) {
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

    if (gpu_i915) {
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
    } else {
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
    }

    for (uint32_t frame = 0; frame < frames; frame++) {
        float shift = wave_u32(frame, 24, 0.0f, 0.45f);
        float blue = wave_u32(frame + 8, 32, 0.45f, 1.0f);
        float uniforms[] = { shift, blue, 1.0f };

        r = srapi_shader_set_uniforms(shader, 0, uniforms, sizeof(uniforms) / sizeof(uniforms[0]));
        if (r != SRAPI_OK) goto fail;

        if (use_drm) {
            fb = srapi_drm_backbuffer(drm);
        } else if (use_fbdev) {
            fb = srapi_fbdev_framebuffer(fbdev);
        }

        if (gpu_queue != NULL) {
            r = srapi_queue_submit(gpu_queue, fb, cmd);
        } else {
            r = srapi_submit(ctx, fb, cmd);
        }
        if (r != SRAPI_OK) goto fail;

        if (use_drm) {
            r = srapi_drm_present(drm);
            if (r != SRAPI_OK) goto fail;
            //usleep(16000);
        } else if (use_fbdev) {
            r = srapi_fbdev_present(fbdev);
            if (r != SRAPI_OK) goto fail;
        } else {
            char path[512];

            frame_path(path, sizeof(path), output, frame, frames);
            r = save_framebuffer(fb, path);
            if (r != SRAPI_OK) goto fail;
        }
    }

    if (use_drm) {
        srapi_display_mode_t mode;

        if (srapi_drm_current_mode(drm, &mode) == SRAPI_OK) {
            printf("rendered %u frames to DRM (%ux%u %.3fHz %s)\n",
                   frames,
                   mode.width,
                   mode.height,
                   (double)mode.refresh_millihz / 1000.0,
                   mode.name);
        } else {
            printf("rendered %u frames to DRM (%ux%u)\n", frames, width, height);
        }
    } else if (use_fbdev) {
        printf("rendered %u frames to fbdev (%ux%u)\n", frames, width, height);
    } else if (frames == 1) {
        printf("wrote %s (%ux%u)\n", output, width, height);
    } else {
        printf("wrote %u frames from %s (%ux%u)\n", frames, output, width, height);
    }

    srapi_destroy_shader(shader);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_queue(gpu_queue);
    srapi_destroy_device(gpu_device);
    if (!use_drm && !use_fbdev) {
        srapi_destroy_framebuffer(fb);
    }
    srapi_drm_close(drm);
    srapi_fbdev_close(fbdev);
    srapi_destroy_context(ctx);
    return 0;

fail:
    fprintf(stderr, "srapi_demo failed: %d\n", r);
    srapi_destroy_shader(shader);
    srapi_destroy_cmd_buffer(cmd);
    srapi_destroy_queue(gpu_queue);
    srapi_destroy_device(gpu_device);
    if (!use_drm && !use_fbdev) {
        srapi_destroy_framebuffer(fb);
    }
    srapi_drm_close(drm);
    srapi_fbdev_close(fbdev);
    srapi_destroy_context(ctx);
    return 1;
}
