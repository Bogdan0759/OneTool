#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void srapi_set_error(const char *fmt, ...);
void srapi_debugf(const char *fmt, ...);

static int queue_grow(srapi_input_context_t *ctx, size_t target_capacity) {
    srapi_input_event_t *grown;
    size_t copy_count;

    if (target_capacity <= ctx->queue_capacity) {
        return 1;
    }
    if (target_capacity > SRAPI_INPUT_QUEUE_MAX) {
        target_capacity = SRAPI_INPUT_QUEUE_MAX;
        if (target_capacity <= ctx->queue_capacity) {
            return 0;
        }
    }

    grown = calloc(target_capacity, sizeof(*grown));
    if (grown == NULL) {
        return 0;
    }
    copy_count = ctx->queue_count;
    for (size_t i = 0; i < copy_count; i++) {
        grown[i] = ctx->queue[(ctx->queue_head + i) % ctx->queue_capacity];
    }
    free(ctx->queue);
    ctx->queue = grown;
    ctx->queue_capacity = target_capacity;
    ctx->queue_head = 0;
    ctx->queue_tail = copy_count;
    return 1;
}

int srapi_input_queue_push(srapi_input_context_t *ctx, const srapi_input_event_t *ev) {
    if (ctx == NULL || ev == NULL) {
        return 0;
    }
    if (ctx->queue_count == ctx->queue_capacity) {
        if (!queue_grow(ctx, ctx->queue_capacity == 0 ? SRAPI_INPUT_QUEUE_INITIAL : ctx->queue_capacity * 2)) {
            srapi_debugf("input: queue overflow, dropping event type=%d", (int)ev->type);
            return 0;
        }
    }
    ctx->queue[ctx->queue_tail] = *ev;
    ctx->queue_tail = (ctx->queue_tail + 1) % ctx->queue_capacity;
    ctx->queue_count++;
    return 1;
}

int srapi_input_queue_pop(srapi_input_context_t *ctx, srapi_input_event_t *out) {
    if (ctx == NULL || out == NULL || ctx->queue_count == 0) {
        return 0;
    }
    *out = ctx->queue[ctx->queue_head];
    ctx->queue_head = (ctx->queue_head + 1) % ctx->queue_capacity;
    ctx->queue_count--;
    return 1;
}

srapi_result_t srapi_input_create(const srapi_input_desc_t *desc, srapi_input_context_t **out) {
    srapi_input_context_t *ctx;

    if (out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out = NULL;

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return SRAPI_ERROR_OOM;
    }

    if (!queue_grow(ctx, SRAPI_INPUT_QUEUE_INITIAL)) {
        free(ctx);
        return SRAPI_ERROR_OOM;
    }

    if (desc != NULL) {
        ctx->grab = desc->grab;
        ctx->mouse_x = desc->initial_mouse_x;
        ctx->mouse_y = desc->initial_mouse_y;
        if (desc->auto_discover) {
            srapi_input_scan_evdev(ctx);
        }
    }

    *out = ctx;
    srapi_debugf("input: context created devices=%zu grab=%d",
                 ctx->device_count, ctx->grab);
    return SRAPI_OK;
}

void srapi_input_destroy(srapi_input_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    srapi_debugf("input: context destroy devices=%zu queue=%zu",
                 ctx->device_count, ctx->queue_count);
    for (size_t i = 0; i < ctx->device_count; i++) {
        srapi_input_close_evdev(&ctx->devices[i]);
    }
    free(ctx->devices);
    free(ctx->queue);
    free(ctx);
}

srapi_result_t srapi_input_add_device(srapi_input_context_t *ctx, const char *path, uint32_t *out_device_id) {
    srapi_input_device_t dev;
    srapi_result_t rc;

    if (ctx == NULL || path == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (ctx->device_count >= SRAPI_INPUT_MAX_DEVICES) {
        srapi_set_error("input: max device limit (%d) reached", SRAPI_INPUT_MAX_DEVICES);
        return SRAPI_ERROR_OVERFLOW;
    }

    rc = srapi_input_open_evdev(path, ctx->grab, &dev);
    if (rc != SRAPI_OK) {
        return rc;
    }

    if (ctx->device_count == ctx->device_capacity) {
        size_t new_cap = ctx->device_capacity == 0 ? 4 : ctx->device_capacity * 2;
        srapi_input_device_t *grown;

        if (new_cap > SRAPI_INPUT_MAX_DEVICES) {
            new_cap = SRAPI_INPUT_MAX_DEVICES;
        }
        grown = realloc(ctx->devices, new_cap * sizeof(*grown));
        if (grown == NULL) {
            srapi_input_close_evdev(&dev);
            return SRAPI_ERROR_OOM;
        }
        ctx->devices = grown;
        ctx->device_capacity = new_cap;
    }

    srapi_input_device_t *slot = &ctx->devices[ctx->device_count++];
    *slot = dev;
    slot->id = ++ctx->next_device_id;
    if (out_device_id != NULL) {
        *out_device_id = slot->id;
    }

    srapi_input_event_t added;
    memset(&added, 0, sizeof(added));
    added.type = SRAPI_INPUT_EVENT_DEVICE_ADDED;
    added.timestamp_us = srapi_input_now_us();
    added.device_id = slot->id;
    snprintf(added.device.path, sizeof(added.device.path), "%s", slot->path);
    snprintf(added.device.name, sizeof(added.device.name), "%s", slot->name);
    srapi_input_queue_push(ctx, &added);
    return SRAPI_OK;
}

srapi_result_t srapi_input_remove_device(srapi_input_context_t *ctx, uint32_t device_id) {
    if (ctx == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    for (size_t i = 0; i < ctx->device_count; i++) {
        if (ctx->devices[i].id != device_id) {
            continue;
        }

        srapi_input_event_t removed;
        memset(&removed, 0, sizeof(removed));
        removed.type = SRAPI_INPUT_EVENT_DEVICE_REMOVED;
        removed.timestamp_us = srapi_input_now_us();
        removed.device_id = device_id;
        snprintf(removed.device.path, sizeof(removed.device.path), "%s", ctx->devices[i].path);
        snprintf(removed.device.name, sizeof(removed.device.name), "%s", ctx->devices[i].name);
        srapi_input_queue_push(ctx, &removed);

        srapi_input_close_evdev(&ctx->devices[i]);
        for (size_t j = i + 1; j < ctx->device_count; j++) {
            ctx->devices[j - 1] = ctx->devices[j];
        }
        ctx->device_count--;
        return SRAPI_OK;
    }
    srapi_set_error("input: device_id %u not found", device_id);
    return SRAPI_ERROR_BAD_ARG;
}

srapi_result_t srapi_input_probe(srapi_input_device_info_t *out, size_t out_count, size_t *written) {
    char path[64];
    size_t n = 0;

    if (written != NULL) {
        *written = 0;
    }
    for (int i = 0; i < SRAPI_INPUT_MAX_DEVICES; i++) {
        srapi_input_device_t dev;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (access(path, R_OK) != 0) {
            continue;
        }
        if (srapi_input_open_evdev(path, 0, &dev) != SRAPI_OK) {
            continue;
        }
        if (out != NULL && n < out_count) {
            memset(&out[n], 0, sizeof(out[n]));
            snprintf(out[n].path, sizeof(out[n].path), "%s", dev.path);
            snprintf(out[n].name, sizeof(out[n].name), "%s", dev.name);
            out[n].has_keyboard = (dev.caps & SRAPI_INPUT_CAP_KEYBOARD) ? 1u : 0u;
            out[n].has_mouse = (dev.caps & SRAPI_INPUT_CAP_MOUSE) ? 1u : 0u;
        }
        srapi_input_close_evdev(&dev);
        n++;
    }
    if (written != NULL) {
        *written = n;
    }
    return SRAPI_OK;
}

int srapi_input_poll(srapi_input_context_t *ctx, srapi_input_event_t *out_event) {
    if (ctx == NULL || out_event == NULL) {
        return -1;
    }
    if (ctx->queue_count == 0) {
        srapi_input_pump_evdev(ctx, 0);
    }
    return srapi_input_queue_pop(ctx, out_event);
}

int srapi_input_wait(srapi_input_context_t *ctx, srapi_input_event_t *out_event, int timeout_ms) {
    if (ctx == NULL || out_event == NULL) {
        return -1;
    }
    if (ctx->queue_count == 0) {
        int rc = srapi_input_pump_evdev(ctx, timeout_ms);
        if (rc < 0) {
            return -1;
        }
    }
    return srapi_input_queue_pop(ctx, out_event);
}

void srapi_input_flush(srapi_input_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->queue_head = 0;
    ctx->queue_tail = 0;
    ctx->queue_count = 0;
}

uint32_t srapi_input_modifiers(const srapi_input_context_t *ctx) {
    return ctx != NULL ? ctx->modifiers : 0u;
}

int srapi_input_key_pressed(const srapi_input_context_t *ctx, srapi_scancode_t scancode) {
    if (ctx == NULL || (int)scancode < 0 || scancode >= SRAPI_SCANCODE_COUNT) {
        return 0;
    }
    return ctx->key_state[scancode & 0xff] ? 1 : 0;
}

uint32_t srapi_input_mouse_buttons(const srapi_input_context_t *ctx) {
    return ctx != NULL ? ctx->mouse_buttons : 0u;
}

void srapi_input_mouse_position(const srapi_input_context_t *ctx, int32_t *out_x, int32_t *out_y) {
    if (ctx == NULL) {
        if (out_x != NULL) *out_x = 0;
        if (out_y != NULL) *out_y = 0;
        return;
    }
    if (out_x != NULL) *out_x = ctx->mouse_x;
    if (out_y != NULL) *out_y = ctx->mouse_y;
}

srapi_result_t srapi_input_set_bounds(srapi_input_context_t *ctx, int32_t width, int32_t height) {
    if (ctx == NULL || width <= 0 || height <= 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    ctx->has_bounds = 1;
    ctx->bounds_w = width;
    ctx->bounds_h = height;
    if (ctx->mouse_x < 0) ctx->mouse_x = 0;
    if (ctx->mouse_y < 0) ctx->mouse_y = 0;
    if (ctx->mouse_x >= width) ctx->mouse_x = width - 1;
    if (ctx->mouse_y >= height) ctx->mouse_y = height - 1;
    return SRAPI_OK;
}
