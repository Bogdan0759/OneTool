#ifndef ONETOOL_LIBS_GUI_RANAL_INTERNAL_H
#define ONETOOL_LIBS_GUI_RANAL_INTERNAL_H

#include <ranal/ranal.h>
#include <srapi/srapi.h>

#include <stddef.h>
#include <stdint.h>

#define RANAL_FONT_W 6
#define RANAL_FONT_H 8
#define RANAL_GLYPH_W 5
#define RANAL_GLYPH_H 7

#define RANAL_MAX_CALLBACKS 4

typedef union {
    struct {
        ranal_color_t bg;
    } panel;
    struct {
        char *text;
        ranal_color_t color;
    } label;
    struct {
        char *text;
        int pressed;
        int hover;
        ranal_on_click_fn on_click;
        void *user;
    } button;
    struct {
        char *text;
        int value;
        int hover;
        ranal_on_toggle_fn on_change;
        void *user;
    } checkbox;
    struct {
        float min;
        float max;
        float value;
        int dragging;
        int hover;
        ranal_on_slide_fn on_change;
        void *user;
    } slider;
    struct {
        char *buffer;
        size_t capacity;
        size_t length;
        size_t cursor;
        int focused;
        int hover;
        ranal_on_text_fn on_change;
        void *user;
    } textbox;
} ranal_widget_data_t;

struct ranal_widget {
    ranal_widget_kind_t kind;
    int32_t x, y;
    int32_t width, height;
    int32_t computed_x, computed_y;
    int32_t computed_w, computed_h;
    int visible;
    int enabled;
    struct ranal_widget *parent;
    struct ranal_widget *first_child;
    struct ranal_widget *next_sibling;
    ranal_layout_kind_t layout;
    int32_t padding;
    int32_t spacing;
    ranal_align_t align;
    int auto_w;
    int auto_h;
    int center_x_in_parent;
    int center_y_in_parent;
    int fill_x_in_parent;
    int fill_y_in_parent;
    ranal_role_t role;
    ranal_color_t bg_color;
    ranal_color_t fg_color;
    int has_bg;
    int has_fg;
    void *sprot_popup_surface;
    struct ranal_surface *popup_target;
    ranal_widget_data_t data;
};

struct ranal_surface {
    int32_t width;
    int32_t height;
    int32_t pitch;
    uint32_t *pixels;
    srapi_framebuffer_t *fb;
    int owns_fb;
};

struct ranal_context {
    int initialized;
    srapi_drm_display_t *drm;
    srapi_context_t *ctx;
    srapi_cmd_buffer_t *cmd;
    srapi_input_context_t *input;
    int targets_drm;
    struct ranal_surface *target;
    void *sprot_conn;
    void *sprot_surface;
    int sprot_pending_frame;
    int32_t width;
    int32_t height;
    ranal_widget_t *root;
    ranal_widget_t *popups;
    ranal_widget_t *focused;
    int32_t mouse_x;
    int32_t mouse_y;
    int prev_mouse_left;
    int curr_mouse_left;
    srapi_clock_t frame_clock;
    srapi_clock_t total_clock;
    float dt;
    int should_close;
    int dirty;
    int presented;
    const ranal_theme_t *theme;
    ranal_key_hook_fn key_hook;
    void *key_hook_user;
    ranal_mouse_hook_fn mouse_hook;
    void *mouse_hook_user;
    char error_buffer[256];
};

typedef struct ranal_context ranal_state_t;
extern ranal_state_t *g_ranal;
void ranal_set_error_(const char *fmt, ...);
ranal_widget_t *ranal_widget_alloc_(ranal_widget_kind_t kind, ranal_widget_t *parent);
void ranal_widget_attach_(ranal_widget_t *parent, ranal_widget_t *child);
void ranal_widget_free_recursive_(ranal_widget_t *widget);
const uint8_t *ranal_font_glyph_(char c);
void ranal_text_render_(int32_t x, int32_t y, const char *text, ranal_color_t color);
void ranal_layout_pass_(ranal_widget_t *root);
void ranal_event_pass_(void);
void ranal_render_pass_(ranal_widget_t *root);
ranal_widget_t *ranal_hit_test_(ranal_widget_t *root, int32_t mx, int32_t my);
char ranal_scancode_to_char_(srapi_scancode_t sc, uint32_t modifiers);
void ranal_swm_pump_events_(void);
void ranal_swm_present_(void);

#endif
