#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void srapi_set_error(const char *fmt, ...);
void srapi_debugf(const char *fmt, ...);

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x) - 1) / BITS_PER_LONG) + 1)

static int test_bit(const unsigned long *bits, int bit) {
    return (bits[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1ul;
}

static int ev_ioctl(int fd, unsigned long request, void *arg) {
    int rc;

    do {
        rc = ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

uint64_t srapi_input_now_us(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static uint32_t probe_capabilities(int fd) {
    unsigned long ev_bits[NBITS(EV_MAX)] = { 0 };
    unsigned long key_bits[NBITS(KEY_MAX)] = { 0 };
    unsigned long rel_bits[NBITS(REL_MAX)] = { 0 };
    unsigned long abs_bits[NBITS(ABS_MAX)] = { 0 };
    uint32_t caps = 0;

    if (ev_ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return 0;
    }

    if (test_bit(ev_bits, EV_KEY)) {
        if (ev_ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
            if (test_bit(key_bits, KEY_A) || test_bit(key_bits, KEY_SPACE) ||
                test_bit(key_bits, KEY_ENTER)) {
                caps |= SRAPI_INPUT_CAP_KEYBOARD;
            }
            if (test_bit(key_bits, BTN_LEFT) || test_bit(key_bits, BTN_RIGHT) ||
                test_bit(key_bits, BTN_MIDDLE)) {
                caps |= SRAPI_INPUT_CAP_MOUSE;
            }
        }
    }

    if (test_bit(ev_bits, EV_REL)) {
        if (ev_ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) >= 0) {
            if (test_bit(rel_bits, REL_X) || test_bit(rel_bits, REL_Y) ||
                test_bit(rel_bits, REL_WHEEL) || test_bit(rel_bits, REL_HWHEEL)) {
                caps |= SRAPI_INPUT_CAP_REL;
            }
        }
    }

    if (test_bit(ev_bits, EV_ABS)) {
        if (ev_ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
            caps |= SRAPI_INPUT_CAP_ABS;
        }
    }

    return caps;
}

srapi_result_t srapi_input_open_evdev(
    const char *path,
    int grab,
    srapi_input_device_t *out
) {
    int fd;
    uint32_t caps;

    if (path == NULL || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        srapi_set_error("input: open %s failed: %s", path, strerror(errno));
        return SRAPI_ERROR_UNSUPPORTED;
    }

    caps = probe_capabilities(fd);
    if ((caps & (SRAPI_INPUT_CAP_KEYBOARD | SRAPI_INPUT_CAP_MOUSE)) == 0) {
        close(fd);
        srapi_set_error("input: %s is neither keyboard nor mouse", path);
        return SRAPI_ERROR_UNSUPPORTED;
    }

    memset(out, 0, sizeof(*out));
    out->fd = fd;
    out->caps = caps;
    snprintf(out->path, sizeof(out->path), "%s", path);

    if (ev_ioctl(fd, EVIOCGNAME(sizeof(out->name)), out->name) < 0) {
        snprintf(out->name, sizeof(out->name), "evdev");
    }
    out->name[sizeof(out->name) - 1] = '\0';

    if (grab) {
        int on = 1;
        if (ev_ioctl(fd, EVIOCGRAB, &on) == 0) {
            out->grabbed = 1;
        } else {
            srapi_debugf("input: EVIOCGRAB failed on %s: %s", path, strerror(errno));
        }
    }

    srapi_debugf("input: opened %s name='%s' caps=0x%x grab=%d",
                 path, out->name, caps, out->grabbed);
    return SRAPI_OK;
}

void srapi_input_close_evdev(srapi_input_device_t *dev) {
    if (dev == NULL || dev->fd < 0) {
        return;
    }
    if (dev->grabbed) {
        int off = 0;
        ev_ioctl(dev->fd, EVIOCGRAB, &off);
    }
    srapi_debugf("input: close fd=%d path=%s", dev->fd, dev->path);
    close(dev->fd);
    dev->fd = -1;
    dev->grabbed = 0;
}

static srapi_result_t append_device(srapi_input_context_t *ctx, const srapi_input_device_t *dev, uint32_t *out_id) {
    if (ctx->device_count == ctx->device_capacity) {
        size_t new_cap = ctx->device_capacity == 0 ? 4 : ctx->device_capacity * 2;
        srapi_input_device_t *grown;

        if (new_cap > SRAPI_INPUT_MAX_DEVICES) {
            new_cap = SRAPI_INPUT_MAX_DEVICES;
        }
        if (ctx->device_count >= new_cap) {
            srapi_set_error("input: max device limit (%d) reached", SRAPI_INPUT_MAX_DEVICES);
            return SRAPI_ERROR_OVERFLOW;
        }
        grown = realloc(ctx->devices, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return SRAPI_ERROR_OOM;
        }
        ctx->devices = grown;
        ctx->device_capacity = new_cap;
    }

    srapi_input_device_t *slot = &ctx->devices[ctx->device_count++];
    *slot = *dev;
    slot->id = ++ctx->next_device_id;
    if (out_id != NULL) {
        *out_id = slot->id;
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

srapi_result_t srapi_input_scan_evdev(srapi_input_context_t *ctx) {
    char path[64];

    if (ctx == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    for (int i = 0; i < SRAPI_INPUT_MAX_DEVICES; i++) {
        srapi_input_device_t dev;
        snprintf(path, sizeof(path), "/dev/input/event%d", i);

        if (access(path, R_OK) != 0) {
            continue;
        }
        if (srapi_input_open_evdev(path, ctx->grab, &dev) != SRAPI_OK) {
            continue;
        }
        if (append_device(ctx, &dev, NULL) != SRAPI_OK) {
            srapi_input_close_evdev(&dev);
            break;
        }
    }
    srapi_debugf("input: scan found %zu device(s)", ctx->device_count);
    return SRAPI_OK;
}

static void apply_mouse_bounds(srapi_input_context_t *ctx) {
    if (!ctx->has_bounds) {
        return;
    }
    if (ctx->mouse_x < 0) ctx->mouse_x = 0;
    if (ctx->mouse_y < 0) ctx->mouse_y = 0;
    if (ctx->mouse_x >= ctx->bounds_w) ctx->mouse_x = ctx->bounds_w - 1;
    if (ctx->mouse_y >= ctx->bounds_h) ctx->mouse_y = ctx->bounds_h - 1;
}

static uint32_t evdev_btn_to_button(uint16_t code) {
    switch (code) {
        case BTN_LEFT:    return SRAPI_MOUSE_BUTTON_LEFT;
        case BTN_RIGHT:   return SRAPI_MOUSE_BUTTON_RIGHT;
        case BTN_MIDDLE:  return SRAPI_MOUSE_BUTTON_MIDDLE;
        case BTN_SIDE:    return SRAPI_MOUSE_BUTTON_X1;
        case BTN_BACK:    return SRAPI_MOUSE_BUTTON_X1;
        case BTN_EXTRA:   return SRAPI_MOUSE_BUTTON_X2;
        case BTN_FORWARD: return SRAPI_MOUSE_BUTTON_X2;
        default:          return 0;
    }
}

static void handle_key_event(
    srapi_input_context_t *ctx,
    srapi_input_device_t *dev,
    const struct input_event *ev,
    uint64_t ts_us
) {
    uint32_t btn = evdev_btn_to_button(ev->code);

    if (btn != 0) {
        uint32_t mask = SRAPI_MOUSE_BUTTON_MASK(btn);
        srapi_input_event_t out;
        memset(&out, 0, sizeof(out));
        out.timestamp_us = ts_us;
        out.device_id = dev->id;
        out.mouse_button.button = btn;
        out.mouse_button.x = ctx->mouse_x;
        out.mouse_button.y = ctx->mouse_y;
        out.mouse_button.modifiers = ctx->modifiers;
        if (ev->value != 0) {
            ctx->mouse_buttons |= mask;
            out.type = SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN;
        } else {
            ctx->mouse_buttons &= ~mask;
            out.type = SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP;
        }
        srapi_input_queue_push(ctx, &out);
        return;
    }

    srapi_scancode_t sc = srapi_input_evdev_to_scancode(ev->code);
    if (sc == SRAPI_SCANCODE_UNKNOWN) {
        return;
    }

    uint32_t mod_bit = srapi_input_scancode_to_keymod(sc);
    if (ev->value != 0) {
        ctx->key_state[sc & 0xff] = 1;
        ctx->modifiers |= mod_bit;
    } else {
        ctx->key_state[sc & 0xff] = 0;
        ctx->modifiers &= ~mod_bit;
    }

    if (sc == SRAPI_SCANCODE_CAPSLOCK && ev->value == 1) {
        ctx->modifiers ^= SRAPI_KMOD_CAPS;
    }
    if (sc == SRAPI_SCANCODE_NUMLOCK && ev->value == 1) {
        ctx->modifiers ^= SRAPI_KMOD_NUM;
    }

    srapi_input_event_t out;
    memset(&out, 0, sizeof(out));
    out.timestamp_us = ts_us;
    out.device_id = dev->id;
    out.key.scancode = sc;
    out.key.modifiers = ctx->modifiers;
    out.key.repeat = (ev->value == 2) ? 1u : 0u;
    out.type = (ev->value != 0) ? SRAPI_INPUT_EVENT_KEY_DOWN : SRAPI_INPUT_EVENT_KEY_UP;
    srapi_input_queue_push(ctx, &out);
}

static void emit_tilt_click(
    srapi_input_context_t *ctx,
    srapi_input_device_t *dev,
    uint32_t button,
    uint64_t ts_us
) {
    uint32_t mask = SRAPI_MOUSE_BUTTON_MASK(button);
    srapi_input_event_t down;
    srapi_input_event_t up;

    memset(&down, 0, sizeof(down));
    down.type = SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN;
    down.timestamp_us = ts_us;
    down.device_id = dev->id;
    down.mouse_button.button = button;
    down.mouse_button.x = ctx->mouse_x;
    down.mouse_button.y = ctx->mouse_y;
    down.mouse_button.modifiers = ctx->modifiers;
    ctx->mouse_buttons |= mask;
    srapi_input_queue_push(ctx, &down);

    up = down;
    up.type = SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP;
    ctx->mouse_buttons &= ~mask;
    srapi_input_queue_push(ctx, &up);
}

static void handle_rel_event(
    srapi_input_context_t *ctx,
    srapi_input_device_t *dev,
    const struct input_event *ev,
    uint64_t ts_us
) {
    switch (ev->code) {
        case REL_X:
            dev->pending_dx += ev->value;
            dev->has_motion = 1;
            break;
        case REL_Y:
            dev->pending_dy += ev->value;
            dev->has_motion = 1;
            break;
        case REL_WHEEL:
            dev->pending_wheel_y += ev->value;
            dev->has_wheel = 1;
            break;
        case REL_HWHEEL: {
            int32_t v = ev->value;
            uint32_t btn = v < 0 ? SRAPI_MOUSE_BUTTON_WHEEL_LEFT
                                 : SRAPI_MOUSE_BUTTON_WHEEL_RIGHT;
            int32_t ticks = v < 0 ? -v : v;
            for (int32_t i = 0; i < ticks; i++) {
                emit_tilt_click(ctx, dev, btn, ts_us);
            }
            break;
        }
        default:
            break;
    }
}

static void flush_syn(
    srapi_input_context_t *ctx,
    srapi_input_device_t *dev,
    uint64_t ts_us
) {
    if (dev->has_motion) {
        int32_t dx = dev->pending_dx;
        int32_t dy = dev->pending_dy;

        ctx->mouse_x += dx;
        ctx->mouse_y += dy;
        apply_mouse_bounds(ctx);

        srapi_input_event_t out;
        memset(&out, 0, sizeof(out));
        out.type = SRAPI_INPUT_EVENT_MOUSE_MOTION;
        out.timestamp_us = ts_us;
        out.device_id = dev->id;
        out.mouse_motion.x = ctx->mouse_x;
        out.mouse_motion.y = ctx->mouse_y;
        out.mouse_motion.dx = dx;
        out.mouse_motion.dy = dy;
        out.mouse_motion.buttons = ctx->mouse_buttons;
        srapi_input_queue_push(ctx, &out);

        dev->pending_dx = 0;
        dev->pending_dy = 0;
        dev->has_motion = 0;
    }
    if (dev->has_wheel) {
        srapi_input_event_t out;
        memset(&out, 0, sizeof(out));
        out.type = SRAPI_INPUT_EVENT_MOUSE_WHEEL;
        out.timestamp_us = ts_us;
        out.device_id = dev->id;
        out.mouse_wheel.dx = dev->pending_wheel_x;
        out.mouse_wheel.dy = dev->pending_wheel_y;
        out.mouse_wheel.x = ctx->mouse_x;
        out.mouse_wheel.y = ctx->mouse_y;
        srapi_input_queue_push(ctx, &out);

        dev->pending_wheel_x = 0;
        dev->pending_wheel_y = 0;
        dev->has_wheel = 0;
    }
}

static int read_device(srapi_input_context_t *ctx, srapi_input_device_t *dev) {
    struct input_event events[32];
    int handled = 0;

    for (;;) {
        ssize_t n = read(dev->fd, events, sizeof(events));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            srapi_debugf("input: read %s failed: %s", dev->path, strerror(errno));
            return -1;
        }
        if (n == 0) {
            break;
        }
        size_t count = (size_t)n / sizeof(events[0]);
        for (size_t i = 0; i < count; i++) {
            const struct input_event *ev = &events[i];
            uint64_t ts_us = (uint64_t)ev->input_event_sec * 1000000ull +
                             (uint64_t)ev->input_event_usec;

            switch (ev->type) {
                case EV_KEY:
                    handle_key_event(ctx, dev, ev, ts_us);
                    handled = 1;
                    break;
                case EV_REL:
                    handle_rel_event(ctx, dev, ev, ts_us);
                    handled = 1;
                    break;
                case EV_SYN:
                    if (ev->code == SYN_REPORT) {
                        flush_syn(ctx, dev, ts_us);
                        handled = 1;
                    }
                    break;
                default:
                    break;
            }
        }
        if ((size_t)n < sizeof(events)) {
            break;
        }
    }
    return handled;
}

int srapi_input_pump_evdev(srapi_input_context_t *ctx, int timeout_ms) {
    struct pollfd fds[SRAPI_INPUT_MAX_DEVICES];

    if (ctx == NULL || ctx->device_count == 0) {
        return 0;
    }

    nfds_t n = 0;
    for (size_t i = 0; i < ctx->device_count && n < SRAPI_INPUT_MAX_DEVICES; i++) {
        if (ctx->devices[i].fd >= 0) {
            fds[n].fd = ctx->devices[i].fd;
            fds[n].events = POLLIN;
            fds[n].revents = 0;
            n++;
        }
    }
    if (n == 0) {
        return 0;
    }

    int rc;
    do {
        rc = poll(fds, n, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc <= 0) {
        return rc < 0 ? -1 : 0;
    }

    int produced = 0;
    nfds_t fi = 0;
    for (size_t i = 0; i < ctx->device_count && fi < n; i++) {
        if (ctx->devices[i].fd < 0) {
            continue;
        }
        if (fds[fi].revents & (POLLIN | POLLHUP | POLLERR)) {
            int r = read_device(ctx, &ctx->devices[i]);
            if (r > 0) {
                produced = 1;
            }
        }
        fi++;
    }
    return produced;
}
