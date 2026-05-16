#ifndef ONETOOL_LIBS_SRAPI_H
#define ONETOOL_LIBS_SRAPI_H

#include <stddef.h>
#include <stdint.h>

typedef struct srapi_context srapi_context_t;
typedef struct srapi_device srapi_device_t;
typedef struct srapi_buffer srapi_buffer_t;
typedef struct srapi_image srapi_image_t;
typedef struct srapi_image_view srapi_image_view_t;
typedef struct srapi_queue srapi_queue_t;
typedef struct srapi_framebuffer srapi_framebuffer_t;
typedef struct srapi_cmd_buffer srapi_cmd_buffer_t;
typedef struct srapi_drm_display srapi_drm_display_t;
typedef struct srapi_fbdev_display srapi_fbdev_display_t;
typedef struct srapi_shader srapi_shader_t;

typedef uint32_t srapi_color_t;

extern int srapi;
const char *srapi_last_error(void);

typedef enum {
    SRAPI_OK = 0,
    SRAPI_ERROR = -1,
    SRAPI_ERROR_OOM = -2,
    SRAPI_ERROR_BAD_ARG = -3,
    SRAPI_ERROR_OVERFLOW = -4,
    SRAPI_ERROR_UNSUPPORTED = -5,
} srapi_result_t;

typedef enum {
    SRAPI_BACKEND_AUTO = 0,
    SRAPI_BACKEND_CPU = 1,
    SRAPI_BACKEND_GPU = 2,
    SRAPI_BACKEND_FBDEV = 3,
} srapi_backend_t;

typedef enum {
    SRAPI_BACKEND_CHECK_OVERFLOW = 1u << 0,
    SRAPI_BACKEND_CHECK_USAGE = 1u << 1,
    SRAPI_BACKEND_CHECK_OWNERSHIP = 1u << 2,
    SRAPI_BACKEND_CHECK_BACKEND_MATCH = 1u << 3,
    SRAPI_BACKEND_CHECK_ALL =
        SRAPI_BACKEND_CHECK_OVERFLOW |
        SRAPI_BACKEND_CHECK_USAGE |
        SRAPI_BACKEND_CHECK_OWNERSHIP |
        SRAPI_BACKEND_CHECK_BACKEND_MATCH,
} srapi_backend_check_t;

typedef struct {
    srapi_backend_t backend;
    uint32_t checks;
} srapi_backend_config_t;

typedef enum {
    SRAPI_BUFFER_TRANSFER_SRC = 1u << 0,
    SRAPI_BUFFER_TRANSFER_DST = 1u << 1,
    SRAPI_BUFFER_VERTEX = 1u << 2,
    SRAPI_BUFFER_INDEX = 1u << 3,
    SRAPI_BUFFER_UNIFORM = 1u << 4,
    SRAPI_BUFFER_STORAGE = 1u << 5,
} srapi_buffer_usage_t;

typedef enum {
    SRAPI_IMAGE_LINEAR = 1,
    SRAPI_IMAGE_OPTIMAL = 2,
} srapi_image_tiling_t;

typedef enum {
    SRAPI_IMAGE_TRANSFER_SRC = 1u << 0,
    SRAPI_IMAGE_TRANSFER_DST = 1u << 1,
    SRAPI_IMAGE_SAMPLED = 1u << 2,
    SRAPI_IMAGE_COLOR_TARGET = 1u << 3,
    SRAPI_IMAGE_PRESENT = 1u << 4,
} srapi_image_usage_t;

typedef enum {
    SRAPI_COMMAND_CLEAR = 1,
    SRAPI_COMMAND_FILL_RECT = 2,
    SRAPI_COMMAND_DRAW_LINE = 3,
    SRAPI_COMMAND_FILL_TRIANGLE = 4,
    SRAPI_COMMAND_SHADE_RECT = 5,
    SRAPI_COMMAND_DRAW_VERTICES = 6,
    SRAPI_COMMAND_SET_SCISSOR = 7,
    SRAPI_COMMAND_SET_VIEWPORT = 8,
    SRAPI_COMMAND_SET_BLEND = 9,
} srapi_command_kind_t;

typedef enum {
    SRAPI_PRIMITIVE_POINTS = 1,
    SRAPI_PRIMITIVE_LINES = 2,
    SRAPI_PRIMITIVE_TRIANGLES = 3,
} srapi_primitive_topology_t;

typedef enum {
    SRAPI_BLEND_NONE = 0,
    SRAPI_BLEND_ALPHA = 1,
} srapi_blend_mode_t;

typedef enum {
    SRAPI_VM_END = 0,
    SRAPI_VM_MOV = 1,
    SRAPI_VM_ADD = 2,
    SRAPI_VM_SUB = 3,
    SRAPI_VM_MUL = 4,
    SRAPI_VM_DIV = 5,
    SRAPI_VM_MIN = 6,
    SRAPI_VM_MAX = 7,
    SRAPI_VM_LOAD_INPUT = 8,
    SRAPI_VM_LOAD_UNIFORM = 9,
    SRAPI_VM_OUT_COLOR = 10,
    SRAPI_VM_LOAD_CONST = 11,
    SRAPI_VM_FRACT = 12,
    SRAPI_VM_CLAMP01 = 13,
    SRAPI_VM_MIX = 14,
    SRAPI_VM_OUT_POSITION = 15,
} srapi_vm_opcode_t;

enum {
    SRAPI_VM_INPUT_X = 0,
    SRAPI_VM_INPUT_Y = 1,
    SRAPI_VM_INPUT_U = 2,
    SRAPI_VM_INPUT_V = 3,
    SRAPI_VM_INPUT_WIDTH = 4,
    SRAPI_VM_INPUT_HEIGHT = 5,
    SRAPI_VM_INPUT_VERTEX_X = 0,
    SRAPI_VM_INPUT_VERTEX_Y = 1,
    SRAPI_VM_INPUT_VERTEX_R = 2,
    SRAPI_VM_INPUT_VERTEX_G = 3,
    SRAPI_VM_INPUT_VERTEX_B = 4,
    SRAPI_VM_INPUT_VERTEX_A = 5,
    SRAPI_VM_INPUT_VERTEX_INDEX = 6,
};

typedef struct {
    uint32_t width;
    uint32_t height;
    srapi_backend_t backend;
    const srapi_backend_config_t *backend_config;
} srapi_context_desc_t;

typedef struct {
    srapi_backend_t backend;
    const char *device_path;
} srapi_device_desc_t;

typedef struct {
    srapi_backend_t backend;
    uint32_t available;
    char path[64];
    char message[128];
} srapi_device_info_t;

typedef struct {
    uint32_t available;
    char path[64];
    uint32_t chipset_id;
    uint32_t has_gem;
    uint32_t has_execbuf2;
    uint32_t has_blt;
    uint32_t has_exec_fence;
    uint32_t cs_timestamp_frequency;
} srapi_i915_info_t;

typedef struct {
    size_t size;
    uint32_t usage;
    const void *initial_data;
} srapi_buffer_desc_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    srapi_image_tiling_t tiling;
    uint32_t usage;
    const void *initial_pixels;
} srapi_image_desc_t;

typedef struct {
    srapi_image_t *image;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} srapi_image_view_desc_t;

typedef struct {
    srapi_device_t *device;
    uint32_t family_index;
} srapi_queue_desc_t;

typedef struct {
    size_t buffer_offset;
    uint32_t image_x;
    uint32_t image_y;
    uint32_t width;
    uint32_t height;
} srapi_buffer_image_copy_t;

typedef struct {
    uint32_t width;
    uint32_t height;
} srapi_framebuffer_desc_t;

typedef struct {
    float x;
    float y;
    srapi_color_t color;
} srapi_vertex_t;

typedef struct {
    srapi_command_kind_t kind;
    srapi_color_t color;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t width;
    uint32_t height;
    srapi_shader_t *shader;
    srapi_primitive_topology_t topology;
    srapi_buffer_t *vertex_buffer;
    srapi_buffer_t *index_buffer;
    size_t vertex_offset;
    size_t vertex_count;
    size_t index_offset;
    size_t index_count;
    srapi_blend_mode_t blend_mode;
} srapi_command_t;

typedef struct {
    char device[64];
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connected;
    uint32_t mode_count;
    uint32_t preferred_width;
    uint32_t preferred_height;
    char preferred_mode[32];
} srapi_display_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz;
    uint32_t flags;
    uint32_t type;
    char name[32];
} srapi_display_mode_t;

typedef struct {
    const char *device_path;
    uint32_t connector_id;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz;
} srapi_drm_display_desc_t;

typedef struct {
    const char *device_path;
} srapi_fbdev_display_desc_t;

srapi_color_t srapi_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

srapi_result_t srapi_create_context(const srapi_context_desc_t *desc, srapi_context_t **out);
void srapi_destroy_context(srapi_context_t *ctx);
srapi_backend_t srapi_context_backend(const srapi_context_t *ctx);
const char *srapi_backend_name(srapi_backend_t backend);
srapi_result_t srapi_get_backend_config(srapi_backend_t backend, srapi_backend_config_t *out);
srapi_result_t srapi_set_backend_config(const srapi_backend_config_t *config);
void srapi_reset_backend_config(srapi_backend_t backend);
uint32_t srapi_backend_enabled_checks(srapi_backend_t backend);
srapi_result_t srapi_context_get_backend_config(
    const srapi_context_t *ctx,
    srapi_backend_config_t *out
);
srapi_result_t srapi_context_set_backend_config(
    srapi_context_t *ctx,
    const srapi_backend_config_t *config
);
uint32_t srapi_context_enabled_checks(const srapi_context_t *ctx);

srapi_result_t srapi_probe_device(srapi_backend_t backend, srapi_device_info_t *out);
srapi_result_t srapi_probe_i915(const char *device_path, srapi_i915_info_t *out);
srapi_result_t srapi_i915_submit_noop(srapi_device_t *device);
srapi_result_t srapi_i915_fill_buffer(
    srapi_device_t *device,
    srapi_buffer_t *dst,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t color
);
srapi_result_t srapi_i915_fill_image(srapi_device_t *device, srapi_image_t *image, uint32_t color);
srapi_result_t srapi_create_device(const srapi_device_desc_t *desc, srapi_device_t **out);
void srapi_destroy_device(srapi_device_t *device);
srapi_backend_t srapi_device_backend(const srapi_device_t *device);
const char *srapi_device_path(const srapi_device_t *device);

srapi_result_t srapi_create_buffer(
    srapi_device_t *device,
    const srapi_buffer_desc_t *desc,
    srapi_buffer_t **out
);
void srapi_destroy_buffer(srapi_buffer_t *buffer);
size_t srapi_buffer_size(const srapi_buffer_t *buffer);
uint32_t srapi_buffer_usage(const srapi_buffer_t *buffer);
srapi_backend_t srapi_buffer_backend(const srapi_buffer_t *buffer);
srapi_result_t srapi_buffer_write(srapi_buffer_t *buffer, size_t offset, const void *data, size_t size);
srapi_result_t srapi_buffer_read(const srapi_buffer_t *buffer, size_t offset, void *out, size_t size);
srapi_result_t srapi_buffer_map(srapi_buffer_t *buffer, void **out);
void srapi_buffer_unmap(srapi_buffer_t *buffer);

srapi_result_t srapi_create_image(
    srapi_device_t *device,
    const srapi_image_desc_t *desc,
    srapi_image_t **out
);
void srapi_destroy_image(srapi_image_t *image);
uint32_t srapi_image_width(const srapi_image_t *image);
uint32_t srapi_image_height(const srapi_image_t *image);
uint32_t srapi_image_pitch(const srapi_image_t *image);
srapi_image_tiling_t srapi_image_tiling(const srapi_image_t *image);
uint32_t srapi_image_usage(const srapi_image_t *image);
srapi_backend_t srapi_image_backend(const srapi_image_t *image);
srapi_result_t srapi_image_map(srapi_image_t *image, void **out, uint32_t *pitch);
void srapi_image_unmap(srapi_image_t *image);

srapi_result_t srapi_create_image_view(
    const srapi_image_view_desc_t *desc,
    srapi_image_view_t **out
);
void srapi_destroy_image_view(srapi_image_view_t *view);
srapi_image_t *srapi_image_view_image(srapi_image_view_t *view);
uint32_t srapi_image_view_x(const srapi_image_view_t *view);
uint32_t srapi_image_view_y(const srapi_image_view_t *view);
uint32_t srapi_image_view_width(const srapi_image_view_t *view);
uint32_t srapi_image_view_height(const srapi_image_view_t *view);
srapi_backend_t srapi_image_view_backend(const srapi_image_view_t *view);
srapi_result_t srapi_image_view_map(srapi_image_view_t *view, void **out, uint32_t *pitch);
void srapi_image_view_unmap(srapi_image_view_t *view);

srapi_result_t srapi_create_queue(const srapi_queue_desc_t *desc, srapi_queue_t **out);
void srapi_destroy_queue(srapi_queue_t *queue);
srapi_device_t *srapi_queue_device(srapi_queue_t *queue);
srapi_result_t srapi_queue_submit(
    srapi_queue_t *queue,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
);
srapi_result_t srapi_queue_copy_buffer_to_image(
    srapi_queue_t *queue,
    srapi_buffer_t *src,
    srapi_image_t *dst,
    const srapi_buffer_image_copy_t *region
);
srapi_result_t srapi_queue_copy_image_to_buffer(
    srapi_queue_t *queue,
    srapi_image_t *src,
    srapi_buffer_t *dst,
    const srapi_buffer_image_copy_t *region
);
srapi_result_t srapi_queue_fill_image(
    srapi_queue_t *queue,
    srapi_image_t *dst,
    srapi_color_t color
);

srapi_result_t srapi_create_framebuffer(
    srapi_context_t *ctx,
    const srapi_framebuffer_desc_t *desc,
    srapi_framebuffer_t **out
);
void srapi_destroy_framebuffer(srapi_framebuffer_t *fb);
uint32_t srapi_framebuffer_width(const srapi_framebuffer_t *fb);
uint32_t srapi_framebuffer_height(const srapi_framebuffer_t *fb);
uint32_t srapi_framebuffer_pitch(const srapi_framebuffer_t *fb);
uint32_t *srapi_framebuffer_pixels(srapi_framebuffer_t *fb);

srapi_result_t srapi_create_cmd_buffer(srapi_context_t *ctx, srapi_cmd_buffer_t **out);
void srapi_destroy_cmd_buffer(srapi_cmd_buffer_t *cmd);
void srapi_cmd_reset(srapi_cmd_buffer_t *cmd);
srapi_result_t srapi_cmd_emit(srapi_cmd_buffer_t *cmd, const srapi_command_t *command);
srapi_result_t srapi_cmd_clear(srapi_cmd_buffer_t *cmd, srapi_color_t color);
srapi_result_t srapi_cmd_fill_rect(
    srapi_cmd_buffer_t *cmd,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_color_t color
);
srapi_result_t srapi_cmd_draw_line(
    srapi_cmd_buffer_t *cmd,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    srapi_color_t color
);
srapi_result_t srapi_cmd_fill_triangle(
    srapi_cmd_buffer_t *cmd,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    int32_t x2,
    int32_t y2,
    srapi_color_t color
);
srapi_result_t srapi_cmd_shade_rect(
    srapi_cmd_buffer_t *cmd,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height,
    srapi_shader_t *shader
);
srapi_result_t srapi_cmd_draw_vertices(
    srapi_cmd_buffer_t *cmd,
    srapi_primitive_topology_t topology,
    srapi_buffer_t *vertex_buffer,
    size_t vertex_offset,
    size_t vertex_count,
    srapi_buffer_t *index_buffer,
    size_t index_offset,
    size_t index_count
);
srapi_result_t srapi_cmd_draw_vertices_shader(
    srapi_cmd_buffer_t *cmd,
    srapi_primitive_topology_t topology,
    srapi_buffer_t *vertex_buffer,
    size_t vertex_offset,
    size_t vertex_count,
    srapi_buffer_t *index_buffer,
    size_t index_offset,
    size_t index_count,
    srapi_shader_t *vertex_shader
);
srapi_result_t srapi_cmd_set_scissor(
    srapi_cmd_buffer_t *cmd,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
);
srapi_result_t srapi_cmd_set_viewport(
    srapi_cmd_buffer_t *cmd,
    int32_t x,
    int32_t y,
    uint32_t width,
    uint32_t height
);
srapi_result_t srapi_cmd_set_blend(srapi_cmd_buffer_t *cmd, srapi_blend_mode_t mode);

srapi_result_t srapi_submit(
    srapi_context_t *ctx,
    srapi_framebuffer_t *target,
    const srapi_cmd_buffer_t *cmd
);

srapi_result_t srapi_save_ppm(const srapi_framebuffer_t *fb, const char *path);
srapi_result_t srapi_save_bmp(const srapi_framebuffer_t *fb, const char *path);

srapi_result_t srapi_create_shader(
    const uint32_t *bytecode,
    size_t word_count,
    const float *uniforms,
    size_t uniform_count,
    srapi_shader_t **out
);
srapi_result_t srapi_shader_set_uniform(srapi_shader_t *shader, size_t index, float value);
srapi_result_t srapi_shader_set_uniforms(
    srapi_shader_t *shader,
    size_t first,
    const float *values,
    size_t count
);
void srapi_destroy_shader(srapi_shader_t *shader);

srapi_result_t srapi_drm_open(const char *device_path, srapi_drm_display_t **out);
srapi_result_t srapi_drm_open_display(
    const srapi_drm_display_desc_t *desc,
    srapi_drm_display_t **out
);
void srapi_drm_close(srapi_drm_display_t *display);
srapi_framebuffer_t *srapi_drm_framebuffer(srapi_drm_display_t *display);
srapi_framebuffer_t *srapi_drm_backbuffer(srapi_drm_display_t *display);
srapi_result_t srapi_drm_present(srapi_drm_display_t *display);
uint32_t srapi_drm_width(const srapi_drm_display_t *display);
uint32_t srapi_drm_height(const srapi_drm_display_t *display);
const char *srapi_drm_device_path(const srapi_drm_display_t *display);
srapi_result_t srapi_drm_current_mode(
    const srapi_drm_display_t *display,
    srapi_display_mode_t *out
);
srapi_result_t srapi_drm_set_mode(
    srapi_drm_display_t *display,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_millihz
);

srapi_result_t srapi_fbdev_open(const char *device_path, srapi_fbdev_display_t **out);
srapi_result_t srapi_fbdev_open_display(
    const srapi_fbdev_display_desc_t *desc,
    srapi_fbdev_display_t **out
);
void srapi_fbdev_close(srapi_fbdev_display_t *display);
srapi_framebuffer_t *srapi_fbdev_framebuffer(srapi_fbdev_display_t *display);
srapi_result_t srapi_fbdev_present(srapi_fbdev_display_t *display);
uint32_t srapi_fbdev_width(const srapi_fbdev_display_t *display);
uint32_t srapi_fbdev_height(const srapi_fbdev_display_t *display);

srapi_result_t srapi_display_probe_drm(
    const char *device_path,
    srapi_display_info_t *out,
    size_t out_count,
    size_t *written
);
srapi_result_t srapi_display_list_modes_drm(
    const char *device_path,
    uint32_t connector_id,
    srapi_display_mode_t *out,
    size_t out_count,
    size_t *written
);

#endif
