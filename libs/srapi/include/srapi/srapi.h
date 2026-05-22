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
    SRAPI_VM_SIN = 16,
    SRAPI_VM_COS = 17,
    SRAPI_VM_ABS = 18,
    SRAPI_VM_SQRT = 19,
    SRAPI_VM_POW = 20,
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

typedef enum {
    SRAPI_I915_DOMAIN_CPU = 0x00000001u,
    SRAPI_I915_DOMAIN_RENDER = 0x00000002u,
    SRAPI_I915_DOMAIN_SAMPLER = 0x00000004u,
    SRAPI_I915_DOMAIN_COMMAND = 0x00000008u,
    SRAPI_I915_DOMAIN_INSTRUCTION = 0x00000010u,
    SRAPI_I915_DOMAIN_VERTEX = 0x00000020u,
    SRAPI_I915_DOMAIN_GTT = 0x00000040u,
    SRAPI_I915_DOMAIN_WC = 0x00000080u,
} srapi_i915_domain_t;

typedef enum {
    SRAPI_I915_EXEC_DEFAULT = 0u << 0,
    SRAPI_I915_EXEC_RENDER = 1u << 0,
    SRAPI_I915_EXEC_BSD = 2u << 0,
    SRAPI_I915_EXEC_BLT = 3u << 0,
    SRAPI_I915_EXEC_VEBOX = 4u << 0,
} srapi_i915_engine_t;

typedef enum {
    SRAPI_I915_OBJECT_NEEDS_FENCE = 1u << 0,
    SRAPI_I915_OBJECT_NEEDS_GTT = 1u << 1,
    SRAPI_I915_OBJECT_WRITE = 1u << 2,
    SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS = 1u << 3,
    SRAPI_I915_OBJECT_PINNED = 1u << 4,
    SRAPI_I915_OBJECT_PAD_TO_SIZE = 1u << 5,
    SRAPI_I915_OBJECT_ASYNC = 1u << 6,
    SRAPI_I915_OBJECT_CAPTURE = 1u << 7,
} srapi_i915_object_flag_t;

typedef struct {
    uint64_t offset;
    int32_t delta;
    uint64_t target_offset;
    uint32_t target_index;
    uint32_t read_domains;
    uint32_t write_domain;
    uint64_t presumed_offset;
} srapi_i915_relocation_t;

typedef struct {
    srapi_buffer_t *buffer;
    const srapi_i915_relocation_t *relocations;
    uint32_t relocation_count;
    uint64_t alignment;
    uint64_t flags;
    uint64_t offset;
} srapi_i915_exec_object_t;

typedef struct {
    srapi_buffer_t *batch;
    size_t batch_start_offset;
    size_t batch_len;
    uint64_t flags;
    srapi_i915_exec_object_t *objects;
    uint32_t object_count;
} srapi_i915_exec_desc_t;

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

typedef struct {
    int simd_enabled;
    int threads_enabled;
} srapi_shade_config_t;

void srapi_set_shade_config(const srapi_shade_config_t *config);
srapi_shade_config_t srapi_get_shade_config(void);

srapi_result_t srapi_probe_device(srapi_backend_t backend, srapi_device_info_t *out);
srapi_result_t srapi_probe_i915(const char *device_path, srapi_i915_info_t *out);
srapi_result_t srapi_i915_buffer_handle(const srapi_buffer_t *buffer, uint32_t *out);
srapi_result_t srapi_i915_buffer_gpu_offset(const srapi_buffer_t *buffer, uint64_t *out);
srapi_result_t srapi_i915_set_domain(
    srapi_device_t *device,
    srapi_buffer_t *buffer,
    uint32_t read_domains,
    uint32_t write_domain
);
srapi_result_t srapi_i915_wait(srapi_device_t *device, srapi_buffer_t *buffer, int64_t timeout_ns);
srapi_result_t srapi_i915_exec(srapi_device_t *device, const srapi_i915_exec_desc_t *desc);
srapi_result_t srapi_i915_submit_noop(srapi_device_t *device);
srapi_result_t srapi_device_set_tile_cache_enabled(srapi_device_t *device, uint32_t enabled);
uint32_t srapi_device_tile_cache_enabled(const srapi_device_t *device);
srapi_result_t srapi_device_set_tile_cache_config(srapi_device_t *device, uint32_t cols, uint32_t rows);
srapi_result_t srapi_device_get_tile_cache_config(const srapi_device_t *device, uint32_t *out_cols, uint32_t *out_rows);
srapi_result_t srapi_i915_set_tile_cache_enabled(srapi_device_t *device, uint32_t enabled);
uint32_t srapi_i915_tile_cache_enabled(const srapi_device_t *device);
srapi_result_t srapi_i915_fill_buffer(
    srapi_device_t *device,
    srapi_buffer_t *dst,
    uint32_t x,
    uint32_t y,
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
srapi_result_t srapi_queue_submit_image(
    srapi_queue_t *queue,
    srapi_image_t *target,
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
srapi_result_t srapi_fbdev_set_tile_cache_enabled(srapi_fbdev_display_t *display, uint32_t enabled);
uint32_t srapi_fbdev_tile_cache_enabled(const srapi_fbdev_display_t *display);
srapi_result_t srapi_fbdev_set_tile_cache_config(srapi_fbdev_display_t *display, uint32_t cols, uint32_t rows);
srapi_result_t srapi_fbdev_get_tile_cache_config(const srapi_fbdev_display_t *display, uint32_t *out_cols, uint32_t *out_rows);

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

typedef struct {
    char path[64];
    char message[128];
    uint32_t has_display;
    uint32_t supports_gpu;
    uint32_t supports_i915;
    uint32_t score;
} srapi_drm_recommendation_t;

srapi_result_t srapi_drm_recommend(srapi_drm_recommendation_t *out);

typedef struct srapi_input_context srapi_input_context_t;

typedef enum {
    SRAPI_INPUT_EVENT_NONE = 0,
    SRAPI_INPUT_EVENT_KEY_DOWN = 1,
    SRAPI_INPUT_EVENT_KEY_UP = 2,
    SRAPI_INPUT_EVENT_MOUSE_MOTION = 3,
    SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN = 4,
    SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP = 5,
    SRAPI_INPUT_EVENT_MOUSE_WHEEL = 6,
    SRAPI_INPUT_EVENT_DEVICE_ADDED = 7,
    SRAPI_INPUT_EVENT_DEVICE_REMOVED = 8,
} srapi_input_event_type_t;

typedef enum {
    SRAPI_SCANCODE_UNKNOWN = 0,
    SRAPI_SCANCODE_A = 4,
    SRAPI_SCANCODE_B = 5,
    SRAPI_SCANCODE_C = 6,
    SRAPI_SCANCODE_D = 7,
    SRAPI_SCANCODE_E = 8,
    SRAPI_SCANCODE_F = 9,
    SRAPI_SCANCODE_G = 10,
    SRAPI_SCANCODE_H = 11,
    SRAPI_SCANCODE_I = 12,
    SRAPI_SCANCODE_J = 13,
    SRAPI_SCANCODE_K = 14,
    SRAPI_SCANCODE_L = 15,
    SRAPI_SCANCODE_M = 16,
    SRAPI_SCANCODE_N = 17,
    SRAPI_SCANCODE_O = 18,
    SRAPI_SCANCODE_P = 19,
    SRAPI_SCANCODE_Q = 20,
    SRAPI_SCANCODE_R = 21,
    SRAPI_SCANCODE_S = 22,
    SRAPI_SCANCODE_T = 23,
    SRAPI_SCANCODE_U = 24,
    SRAPI_SCANCODE_V = 25,
    SRAPI_SCANCODE_W = 26,
    SRAPI_SCANCODE_X = 27,
    SRAPI_SCANCODE_Y = 28,
    SRAPI_SCANCODE_Z = 29,
    SRAPI_SCANCODE_1 = 30,
    SRAPI_SCANCODE_2 = 31,
    SRAPI_SCANCODE_3 = 32,
    SRAPI_SCANCODE_4 = 33,
    SRAPI_SCANCODE_5 = 34,
    SRAPI_SCANCODE_6 = 35,
    SRAPI_SCANCODE_7 = 36,
    SRAPI_SCANCODE_8 = 37,
    SRAPI_SCANCODE_9 = 38,
    SRAPI_SCANCODE_0 = 39,
    SRAPI_SCANCODE_RETURN = 40,
    SRAPI_SCANCODE_ESCAPE = 41,
    SRAPI_SCANCODE_BACKSPACE = 42,
    SRAPI_SCANCODE_TAB = 43,
    SRAPI_SCANCODE_SPACE = 44,
    SRAPI_SCANCODE_MINUS = 45,
    SRAPI_SCANCODE_EQUALS = 46,
    SRAPI_SCANCODE_LEFTBRACKET = 47,
    SRAPI_SCANCODE_RIGHTBRACKET = 48,
    SRAPI_SCANCODE_BACKSLASH = 49,
    SRAPI_SCANCODE_SEMICOLON = 51,
    SRAPI_SCANCODE_APOSTROPHE = 52,
    SRAPI_SCANCODE_GRAVE = 53,
    SRAPI_SCANCODE_COMMA = 54,
    SRAPI_SCANCODE_PERIOD = 55,
    SRAPI_SCANCODE_SLASH = 56,
    SRAPI_SCANCODE_CAPSLOCK = 57,
    SRAPI_SCANCODE_F1 = 58,
    SRAPI_SCANCODE_F2 = 59,
    SRAPI_SCANCODE_F3 = 60,
    SRAPI_SCANCODE_F4 = 61,
    SRAPI_SCANCODE_F5 = 62,
    SRAPI_SCANCODE_F6 = 63,
    SRAPI_SCANCODE_F7 = 64,
    SRAPI_SCANCODE_F8 = 65,
    SRAPI_SCANCODE_F9 = 66,
    SRAPI_SCANCODE_F10 = 67,
    SRAPI_SCANCODE_F11 = 68,
    SRAPI_SCANCODE_F12 = 69,
    SRAPI_SCANCODE_PRINTSCREEN = 70,
    SRAPI_SCANCODE_SCROLLLOCK = 71,
    SRAPI_SCANCODE_PAUSE = 72,
    SRAPI_SCANCODE_INSERT = 73,
    SRAPI_SCANCODE_HOME = 74,
    SRAPI_SCANCODE_PAGEUP = 75,
    SRAPI_SCANCODE_DELETE = 76,
    SRAPI_SCANCODE_END = 77,
    SRAPI_SCANCODE_PAGEDOWN = 78,
    SRAPI_SCANCODE_RIGHT = 79,
    SRAPI_SCANCODE_LEFT = 80,
    SRAPI_SCANCODE_DOWN = 81,
    SRAPI_SCANCODE_UP = 82,
    SRAPI_SCANCODE_NUMLOCK = 83,
    SRAPI_SCANCODE_KP_DIVIDE = 84,
    SRAPI_SCANCODE_KP_MULTIPLY = 85,
    SRAPI_SCANCODE_KP_MINUS = 86,
    SRAPI_SCANCODE_KP_PLUS = 87,
    SRAPI_SCANCODE_KP_ENTER = 88,
    SRAPI_SCANCODE_KP_1 = 89,
    SRAPI_SCANCODE_KP_2 = 90,
    SRAPI_SCANCODE_KP_3 = 91,
    SRAPI_SCANCODE_KP_4 = 92,
    SRAPI_SCANCODE_KP_5 = 93,
    SRAPI_SCANCODE_KP_6 = 94,
    SRAPI_SCANCODE_KP_7 = 95,
    SRAPI_SCANCODE_KP_8 = 96,
    SRAPI_SCANCODE_KP_9 = 97,
    SRAPI_SCANCODE_KP_0 = 98,
    SRAPI_SCANCODE_KP_PERIOD = 99,
    SRAPI_SCANCODE_LCTRL = 224,
    SRAPI_SCANCODE_LSHIFT = 225,
    SRAPI_SCANCODE_LALT = 226,
    SRAPI_SCANCODE_LGUI = 227,
    SRAPI_SCANCODE_RCTRL = 228,
    SRAPI_SCANCODE_RSHIFT = 229,
    SRAPI_SCANCODE_RALT = 230,
    SRAPI_SCANCODE_RGUI = 231,
    SRAPI_SCANCODE_COUNT = 256,
} srapi_scancode_t;

typedef enum {
    SRAPI_KMOD_NONE = 0,
    SRAPI_KMOD_LSHIFT = 1u << 0,
    SRAPI_KMOD_RSHIFT = 1u << 1,
    SRAPI_KMOD_LCTRL  = 1u << 2,
    SRAPI_KMOD_RCTRL  = 1u << 3,
    SRAPI_KMOD_LALT   = 1u << 4,
    SRAPI_KMOD_RALT   = 1u << 5,
    SRAPI_KMOD_LGUI   = 1u << 6,
    SRAPI_KMOD_RGUI   = 1u << 7,
    SRAPI_KMOD_CAPS   = 1u << 8,
    SRAPI_KMOD_NUM    = 1u << 9,
    SRAPI_KMOD_SHIFT  = SRAPI_KMOD_LSHIFT | SRAPI_KMOD_RSHIFT,
    SRAPI_KMOD_CTRL   = SRAPI_KMOD_LCTRL  | SRAPI_KMOD_RCTRL,
    SRAPI_KMOD_ALT    = SRAPI_KMOD_LALT   | SRAPI_KMOD_RALT,
    SRAPI_KMOD_GUI    = SRAPI_KMOD_LGUI   | SRAPI_KMOD_RGUI,
} srapi_keymod_t;

typedef enum {
    SRAPI_MOUSE_BUTTON_LEFT        = 1,
    SRAPI_MOUSE_BUTTON_MIDDLE      = 2,
    SRAPI_MOUSE_BUTTON_RIGHT       = 3,
    SRAPI_MOUSE_BUTTON_X1          = 4,
    SRAPI_MOUSE_BUTTON_X2          = 5,
    SRAPI_MOUSE_BUTTON_WHEEL_LEFT  = 6,
    SRAPI_MOUSE_BUTTON_WHEEL_RIGHT = 7,
} srapi_mouse_button_t;

#define SRAPI_MOUSE_BUTTON_MASK(n) (1u << ((n) - 1))

typedef struct {
    srapi_scancode_t scancode;
    uint32_t modifiers;
    uint32_t repeat;
} srapi_input_key_data_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint32_t buttons;
} srapi_input_mouse_motion_data_t;

typedef struct {
    uint32_t button;
    int32_t x;
    int32_t y;
    uint32_t modifiers;
} srapi_input_mouse_button_data_t;

typedef struct {
    int32_t dx;
    int32_t dy;
    int32_t x;
    int32_t y;
} srapi_input_mouse_wheel_data_t;

typedef struct {
    char path[64];
    char name[64];
} srapi_input_device_data_t;

typedef struct {
    srapi_input_event_type_t type;
    uint64_t timestamp_us;
    uint32_t device_id;
    union {
        srapi_input_key_data_t key;
        srapi_input_mouse_motion_data_t mouse_motion;
        srapi_input_mouse_button_data_t mouse_button;
        srapi_input_mouse_wheel_data_t mouse_wheel;
        srapi_input_device_data_t device;
    };
} srapi_input_event_t;

typedef struct {
    int auto_discover;
    int grab;
    int32_t initial_mouse_x;
    int32_t initial_mouse_y;
} srapi_input_desc_t;

typedef struct {
    char path[64];
    char name[64];
    uint32_t has_keyboard;
    uint32_t has_mouse;
} srapi_input_device_info_t;

srapi_result_t srapi_input_create(const srapi_input_desc_t *desc, srapi_input_context_t **out);
void srapi_input_destroy(srapi_input_context_t *ctx);

srapi_result_t srapi_input_add_device(srapi_input_context_t *ctx, const char *path, uint32_t *out_device_id);
srapi_result_t srapi_input_remove_device(srapi_input_context_t *ctx, uint32_t device_id);
srapi_result_t srapi_input_probe(srapi_input_device_info_t *out, size_t out_count, size_t *written);

int srapi_input_poll(srapi_input_context_t *ctx, srapi_input_event_t *out_event);
int srapi_input_wait(srapi_input_context_t *ctx, srapi_input_event_t *out_event, int timeout_ms);
void srapi_input_flush(srapi_input_context_t *ctx);

uint32_t srapi_input_modifiers(const srapi_input_context_t *ctx);
int srapi_input_key_pressed(const srapi_input_context_t *ctx, srapi_scancode_t scancode);
uint32_t srapi_input_mouse_buttons(const srapi_input_context_t *ctx);
void srapi_input_mouse_position(const srapi_input_context_t *ctx, int32_t *out_x, int32_t *out_y);
srapi_result_t srapi_input_set_bounds(srapi_input_context_t *ctx, int32_t width, int32_t height);
const char *srapi_scancode_name(srapi_scancode_t scancode);

typedef struct {
    uint64_t start_us;
    uint64_t last_us;
} srapi_clock_t;

uint64_t srapi_time_now_us(void);
void srapi_clock_init(srapi_clock_t *clock);
float srapi_clock_tick(srapi_clock_t *clock);
float srapi_clock_elapsed(const srapi_clock_t *clock);

#endif
