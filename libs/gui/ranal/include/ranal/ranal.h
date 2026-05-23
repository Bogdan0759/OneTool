#ifndef ONETOOL_LIBS_GUI_RANAL_H
#define ONETOOL_LIBS_GUI_RANAL_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t ranal_color_t;

typedef enum {
    RANAL_OK = 0,
    RANAL_ERROR = -1,
    RANAL_ERROR_OOM = -2,
    RANAL_ERROR_BAD_ARG = -3,
    RANAL_ERROR_BACKEND = -4,
} ranal_result_t;

typedef enum {
    RANAL_WIDGET_PANEL,
    RANAL_WIDGET_LABEL,
    RANAL_WIDGET_BUTTON,
    RANAL_WIDGET_CHECKBOX,
    RANAL_WIDGET_SLIDER,
    RANAL_WIDGET_TEXTBOX,
} ranal_widget_kind_t;

typedef enum {
    RANAL_LAYOUT_ABSOLUTE = 0,
    RANAL_LAYOUT_VERT     = 1,
    RANAL_LAYOUT_HORIZ    = 2,
} ranal_layout_kind_t;

typedef enum {
    RANAL_ALIGN_START  = 0,
    RANAL_ALIGN_CENTER = 1,
    RANAL_ALIGN_END    = 2,
} ranal_align_t;

typedef struct ranal_widget ranal_widget_t;
typedef struct ranal_context ranal_context_t;
typedef struct ranal_surface ranal_surface_t;
typedef void (*ranal_on_click_fn)(ranal_widget_t *widget, void *user);
typedef void (*ranal_on_toggle_fn)(ranal_widget_t *widget, int value, void *user);
typedef void (*ranal_on_slide_fn)(ranal_widget_t *widget, float value, void *user);
typedef void (*ranal_on_text_fn)(ranal_widget_t *widget, const char *text, void *user);
ranal_color_t ranal_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
const char *ranal_last_error(void);
#define RANAL_COLOR(r, g, b)  (((uint32_t)(0xFFu) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RANAL_BG          RANAL_COLOR(22, 24, 30)
#define RANAL_PANEL_BG    RANAL_COLOR(34, 38, 48)
#define RANAL_BORDER      RANAL_COLOR(80, 90, 110)
#define RANAL_TEXT        RANAL_COLOR(220, 222, 230)
#define RANAL_TEXT_DIM    RANAL_COLOR(150, 155, 170)
#define RANAL_ACCENT      RANAL_COLOR(90, 170, 240)
#define RANAL_ACCENT_HOT  RANAL_COLOR(120, 200, 255)
#define RANAL_DANGER      RANAL_COLOR(240, 90, 90)

typedef struct {
    ranal_color_t bg;
    ranal_color_t panel_bg;
    ranal_color_t border;
    ranal_color_t text;
    ranal_color_t text_dim;
    ranal_color_t accent;
    ranal_color_t accent_hot;
    ranal_color_t danger;
    ranal_color_t button_bg;
    ranal_color_t button_bg_hover;
    ranal_color_t button_bg_pressed;
    ranal_color_t button_bg_disabled;
    ranal_color_t textbox_bg;
} ranal_theme_t;

extern const ranal_theme_t ranal_theme_dark;
extern const ranal_theme_t ranal_theme_light;

void                 ranal_set_theme(const ranal_theme_t *theme);
const ranal_theme_t *ranal_get_theme(void);

typedef enum {
    RANAL_ROLE_DEFAULT = 0,
    RANAL_ROLE_ACCENT  = 1,
    RANAL_ROLE_DIM     = 2,
    RANAL_ROLE_DANGER  = 3,
} ranal_role_t;

void ranal_set_role(ranal_widget_t *widget, ranal_role_t role);
typedef struct {
    int32_t width;
    int32_t height;
    const char *title;
} ranal_window_desc_t;
ranal_result_t ranal_init(const ranal_window_desc_t *desc);
void ranal_shutdown(void);
int  ranal_should_close(void);
void ranal_request_close(void);
void ranal_run(void);
int  ranal_frame(void);
int  ranal_render(void);
int  ranal_present(void);
int32_t ranal_window_width(void);
int32_t ranal_window_height(void);
float   ranal_frame_time(void);
double  ranal_time_elapsed(void);
ranal_result_t ranal_record_start(const char *path, uint32_t fps);
void ranal_record_stop(void);
ranal_widget_t *ranal_root(void);
ranal_widget_t *ranal_panel(ranal_widget_t *parent);
ranal_widget_t *ranal_label(ranal_widget_t *parent, const char *text);
ranal_widget_t *ranal_button(ranal_widget_t *parent, const char *text);
ranal_widget_t *ranal_checkbox(ranal_widget_t *parent, const char *text, int initial);
ranal_widget_t *ranal_slider(ranal_widget_t *parent, float min, float max, float initial);
ranal_widget_t *ranal_textbox(ranal_widget_t *parent, char *buffer, size_t capacity);
void ranal_destroy_widget(ranal_widget_t *widget);
void ranal_set_pos(ranal_widget_t *widget, int32_t x, int32_t y);
void ranal_set_size(ranal_widget_t *widget, int32_t width, int32_t height);
void ranal_set_visible(ranal_widget_t *widget, int visible);
void ranal_set_enabled(ranal_widget_t *widget, int enabled);
void ranal_set_text(ranal_widget_t *widget, const char *text);
const char *ranal_get_text(const ranal_widget_t *widget);
void ranal_set_layout(ranal_widget_t *panel, ranal_layout_kind_t kind);
void ranal_set_padding(ranal_widget_t *panel, int32_t padding);
void ranal_set_spacing(ranal_widget_t *panel, int32_t spacing);
void ranal_set_align(ranal_widget_t *panel, ranal_align_t align);
void ranal_set_background(ranal_widget_t *widget, ranal_color_t color);
void ranal_set_foreground(ranal_widget_t *widget, ranal_color_t color);
void ranal_set_auto_size(ranal_widget_t *widget, int auto_w, int auto_h);
void ranal_set_center_in_parent(ranal_widget_t *widget, int center_x, int center_y);
void ranal_set_fill_parent(ranal_widget_t *widget, int fill_x, int fill_y);
void ranal_invalidate(void);
int  ranal_checkbox_value(const ranal_widget_t *checkbox);
void ranal_checkbox_set_value(ranal_widget_t *checkbox, int value);
float ranal_slider_value(const ranal_widget_t *slider);
void  ranal_slider_set_value(ranal_widget_t *slider, float value);
void ranal_on_click(ranal_widget_t *button, ranal_on_click_fn fn, void *user);
void ranal_on_toggle(ranal_widget_t *checkbox, ranal_on_toggle_fn fn, void *user);
void ranal_on_slide(ranal_widget_t *slider, ranal_on_slide_fn fn, void *user);
void ranal_on_text(ranal_widget_t *textbox, ranal_on_text_fn fn, void *user);
void ranal_draw_pixel(int32_t x, int32_t y, ranal_color_t color);
void ranal_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, ranal_color_t color);
void ranal_draw_rect_lines(int32_t x, int32_t y, int32_t w, int32_t h, int32_t thickness, ranal_color_t color);
void ranal_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, ranal_color_t color);
void ranal_draw_text(int32_t x, int32_t y, const char *text, ranal_color_t color);
int32_t ranal_text_width(const char *text);
int32_t ranal_text_height(void);

#define RANAL_GLYPH_WIDTH    5
#define RANAL_GLYPH_HEIGHT   7
#define RANAL_FONT_ADVANCE_X 6
#define RANAL_FONT_ADVANCE_Y 8
const uint8_t *ranal_font_glyph(char c);

ranal_surface_t *ranal_surface_create(int32_t width, int32_t height);
void ranal_surface_destroy(ranal_surface_t *surface);
uint32_t *ranal_surface_pixels(ranal_surface_t *surface);
int32_t ranal_surface_width(const ranal_surface_t *surface);
int32_t ranal_surface_height(const ranal_surface_t *surface);
int32_t ranal_surface_pitch(const ranal_surface_t *surface);
void ranal_blit_surface(const ranal_surface_t *src, int32_t dst_x, int32_t dst_y);

ranal_context_t *ranal_context_create_offscreen(int32_t width, int32_t height);
void ranal_context_destroy(ranal_context_t *ctx);
int ranal_context_frame(ranal_context_t *ctx);
ranal_widget_t *ranal_context_root(ranal_context_t *ctx);
ranal_surface_t *ranal_context_surface(ranal_context_t *ctx);
void ranal_set_current(ranal_context_t *ctx);
ranal_context_t *ranal_get_current(void);
ranal_context_t *ranal_default_context(void);

ranal_widget_t *ranal_popup(int32_t x, int32_t y);
void ranal_popup_close(ranal_widget_t *popup);

ranal_result_t ranal_init_swm(const char *title, int32_t width, int32_t height);
ranal_result_t ranal_set_window_title(const char *title);
int ranal_is_swm_mode(void);
#endif
