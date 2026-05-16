#ifndef ONETOOL_LIBS_SRAPI_INTERNAL_H
#define ONETOOL_LIBS_SRAPI_INTERNAL_H

#include <srapi/srapi.h>

struct srapi_context {
    uint32_t width;
    uint32_t height;
    srapi_backend_t backend;
};

struct srapi_device {
    srapi_backend_t backend;
    char path[64];
    int fd;
};

struct srapi_buffer {
    srapi_device_t *device;
    srapi_backend_t backend;
    size_t size;
    uint32_t usage;
    void *data;
    int mapped;
    uint32_t gpu_handle;
    uint64_t gpu_size;
};

struct srapi_image {
    srapi_device_t *device;
    srapi_backend_t backend;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    srapi_image_tiling_t tiling;
    uint32_t usage;
    void *data;
    int mapped;
    uint32_t gpu_handle;
    uint64_t gpu_size;
};

struct srapi_queue {
    srapi_device_t *device;
    srapi_backend_t backend;
    uint32_t family_index;
};

struct srapi_framebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t *pixels;
    int owns_pixels;
    srapi_backend_t backend;
};

struct srapi_cmd_buffer {
    srapi_command_t *items;
    size_t count;
    size_t capacity;
};

typedef struct {
    uint8_t op;
    uint8_t dst;
    uint8_t a;
    uint8_t b;
    uint8_t c;
    uint8_t d;
    uint32_t imm;
} srapi_vm_inst_t;

struct srapi_shader {
    uint32_t *bytecode;
    size_t word_count;
    srapi_vm_inst_t *insts;
    size_t inst_count;
    float *uniforms;
    size_t uniform_count;
    size_t run_count;
};

srapi_result_t srapi_cmd_push(srapi_cmd_buffer_t *cmd, const srapi_command_t *item);
void srapi_debugf(const char *fmt, ...);
void srapi_set_error(const char *fmt, ...);
srapi_result_t srapi_gpu_probe(srapi_device_info_t *out);
srapi_result_t srapi_gpu_open_device(const srapi_device_desc_t *desc, srapi_device_t **out);
void srapi_gpu_close_device(srapi_device_t *device);
srapi_result_t srapi_gpu_create_buffer(
    srapi_device_t *device,
    const srapi_buffer_desc_t *desc,
    srapi_buffer_t **out
);
srapi_result_t srapi_gpu_create_image(
    srapi_device_t *device,
    const srapi_image_desc_t *desc,
    srapi_image_t **out
);
srapi_result_t srapi_gpu_create_queue(const srapi_queue_desc_t *desc, srapi_queue_t **out);

void srapi_render_clear(srapi_framebuffer_t *fb, srapi_color_t color);
void srapi_render_fill_rect(
    srapi_framebuffer_t *fb,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_color_t color
);
void srapi_render_draw_line(
    srapi_framebuffer_t *fb,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    srapi_color_t color
);
void srapi_render_fill_triangle(
    srapi_framebuffer_t *fb,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2,
    srapi_color_t color
);
void srapi_render_shade_rect(
    srapi_framebuffer_t *fb,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_shader_t *shader
);

srapi_result_t srapi_vm_run_fragment(
    srapi_shader_t *shader,
    const float inputs[6],
    srapi_color_t *out_color
);

#endif
