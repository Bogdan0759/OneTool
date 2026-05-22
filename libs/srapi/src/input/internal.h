#ifndef ONETOOL_LIBS_SRAPI_INPUT_INTERNAL_H
#define ONETOOL_LIBS_SRAPI_INPUT_INTERNAL_H
#include <srapi/srapi.h>
#include <stdint.h>
#include <stddef.h>
#define SRAPI_INPUT_MAX_DEVICES 32
#define SRAPI_INPUT_QUEUE_INITIAL 256
#define SRAPI_INPUT_QUEUE_MAX 4096
#define SRAPI_INPUT_KEY_STATE_SIZE 256
enum {
    SRAPI_INPUT_CAP_KEYBOARD = 1u << 0,
    SRAPI_INPUT_CAP_MOUSE    = 1u << 1,
    SRAPI_INPUT_CAP_REL      = 1u << 2,
    SRAPI_INPUT_CAP_ABS      = 1u << 3,
};

typedef struct {
    int fd;
    uint32_t id;
    char path[64];
    char name[64];
    uint32_t caps;
    int grabbed;
    int32_t pending_dx;
    int32_t pending_dy;
    int32_t pending_wheel_x;
    int32_t pending_wheel_y;
    int has_motion;
    int has_wheel;
} srapi_input_device_t;

struct srapi_input_context {
    srapi_input_device_t *devices;
    size_t device_count;
    size_t device_capacity;
    uint32_t next_device_id;

    srapi_input_event_t *queue;
    size_t queue_capacity;
    size_t queue_head;
    size_t queue_tail;
    size_t queue_count;

    uint8_t key_state[SRAPI_INPUT_KEY_STATE_SIZE];
    uint32_t modifiers;
    int32_t mouse_x;
    int32_t mouse_y;
    uint32_t mouse_buttons;

    int has_bounds;
    int32_t bounds_w;
    int32_t bounds_h;

    int grab;
};

srapi_scancode_t srapi_input_evdev_to_scancode(uint16_t evdev_code);
uint32_t srapi_input_scancode_to_keymod(srapi_scancode_t sc);
const char *srapi_input_scancode_name_table(srapi_scancode_t sc);

srapi_result_t srapi_input_open_evdev(
    const char *path,
    int grab,
    srapi_input_device_t *out
);
void srapi_input_close_evdev(srapi_input_device_t *dev);
srapi_result_t srapi_input_scan_evdev(srapi_input_context_t *ctx);
int srapi_input_pump_evdev(srapi_input_context_t *ctx, int timeout_ms);

int srapi_input_queue_push(srapi_input_context_t *ctx, const srapi_input_event_t *ev);
int srapi_input_queue_pop(srapi_input_context_t *ctx, srapi_input_event_t *out);

uint64_t srapi_input_now_us(void);

#endif
