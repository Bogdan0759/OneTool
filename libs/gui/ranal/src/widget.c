#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const ranal_theme_t ranal_theme_dark = {
    .bg                 = RANAL_COLOR(22, 24, 30),
    .panel_bg           = RANAL_COLOR(34, 38, 48),
    .border             = RANAL_COLOR(80, 90, 110),
    .text               = RANAL_COLOR(220, 222, 230),
    .text_dim           = RANAL_COLOR(150, 155, 170),
    .accent             = RANAL_COLOR(90, 170, 240),
    .accent_hot         = RANAL_COLOR(120, 200, 255),
    .danger             = RANAL_COLOR(240, 90, 90),
    .button_bg          = RANAL_COLOR(50, 56, 70),
    .button_bg_hover    = RANAL_COLOR(70, 80, 100),
    .button_bg_pressed  = RANAL_COLOR(90, 170, 240),
    .button_bg_disabled = RANAL_COLOR(40, 42, 50),
    .textbox_bg         = RANAL_COLOR(18, 20, 26),
};

const ranal_theme_t ranal_theme_light = {
    .bg                 = RANAL_COLOR(235, 238, 245),
    .panel_bg           = RANAL_COLOR(250, 251, 254),
    .border             = RANAL_COLOR(180, 188, 200),
    .text               = RANAL_COLOR(30, 36, 50),
    .text_dim           = RANAL_COLOR(110, 118, 130),
    .accent             = RANAL_COLOR(40, 120, 220),
    .accent_hot         = RANAL_COLOR(80, 150, 240),
    .danger             = RANAL_COLOR(210, 70, 70),
    .button_bg          = RANAL_COLOR(220, 226, 235),
    .button_bg_hover    = RANAL_COLOR(200, 210, 225),
    .button_bg_pressed  = RANAL_COLOR(40, 120, 220),
    .button_bg_disabled = RANAL_COLOR(225, 228, 235),
    .textbox_bg         = RANAL_COLOR(255, 255, 255),
};

void ranal_set_theme(const ranal_theme_t *theme) {
    if (theme == NULL) return;
    g_ranal->theme = theme;
    g_ranal->dirty = 1;
}

const ranal_theme_t *ranal_get_theme(void) {
    return g_ranal->theme != NULL ? g_ranal->theme : &ranal_theme_dark;
}

void ranal_set_role(ranal_widget_t *w, ranal_role_t role) {
    if (w == NULL) return;
    w->role = role;
    g_ranal->dirty = 1;
}

ranal_color_t ranal_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

const char *ranal_last_error(void) {
    return g_ranal->error_buffer[0] != '\0' ? g_ranal->error_buffer : "ok";
}

void ranal_set_error_(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_ranal->error_buffer, sizeof(g_ranal->error_buffer), fmt, args);
    va_end(args);
}

ranal_widget_t *ranal_widget_alloc_(ranal_widget_kind_t kind, ranal_widget_t *parent) {
    ranal_widget_t *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        ranal_set_error_("ranal: out of memory");
        return NULL;
    }
    w->kind = kind;
    w->visible = 1;
    w->enabled = 1;
    w->align = RANAL_ALIGN_START;
    w->layout = RANAL_LAYOUT_ABSOLUTE;
    w->padding = 8;
    w->spacing = 6;
    if (parent != NULL) {
        ranal_widget_attach_(parent, w);
    }
    return w;
}

void ranal_widget_attach_(ranal_widget_t *parent, ranal_widget_t *child) {
    if (parent == NULL || child == NULL) {
        return;
    }
    child->parent = parent;
    if (parent->first_child == NULL) {
        parent->first_child = child;
        return;
    }
    ranal_widget_t *tail = parent->first_child;
    while (tail->next_sibling != NULL) {
        tail = tail->next_sibling;
    }
    tail->next_sibling = child;
}

static void free_widget_data(ranal_widget_t *w) {
    switch (w->kind) {
        case RANAL_WIDGET_LABEL:    free(w->data.label.text); break;
        case RANAL_WIDGET_BUTTON:   free(w->data.button.text); break;
        case RANAL_WIDGET_CHECKBOX: free(w->data.checkbox.text); break;
        case RANAL_WIDGET_TEXTBOX:
            break;
        default: break;
    }
}

void ranal_widget_free_recursive_(ranal_widget_t *widget) {
    if (widget == NULL) {
        return;
    }
    ranal_widget_t *child = widget->first_child;
    while (child != NULL) {
        ranal_widget_t *next = child->next_sibling;
        ranal_widget_free_recursive_(child);
        child = next;
    }
    free_widget_data(widget);
    free(widget);
}

static char *dup_text(const char *src) {
    if (src == NULL) {
        return NULL;
    }
    size_t len = strlen(src);
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, src, len + 1);
    return out;
}

ranal_widget_t *ranal_root(void) {
    return g_ranal->root;
}

ranal_widget_t *ranal_panel(ranal_widget_t *parent) {
    ranal_widget_t *w;
    if (parent == NULL) parent = g_ranal->root;
    w = ranal_widget_alloc_(RANAL_WIDGET_PANEL, parent);
    if (w == NULL) return NULL;
    return w;
}

ranal_widget_t *ranal_label(ranal_widget_t *parent, const char *text) {
    ranal_widget_t *w;
    if (parent == NULL) parent = g_ranal->root;
    w = ranal_widget_alloc_(RANAL_WIDGET_LABEL, parent);
    if (w == NULL) return NULL;
    w->data.label.text = dup_text(text != NULL ? text : "");
    w->height = RANAL_FONT_H;
    w->width = ranal_text_width(text);
    return w;
}

ranal_widget_t *ranal_button(ranal_widget_t *parent, const char *text) {
    ranal_widget_t *w;
    if (parent == NULL) parent = g_ranal->root;
    w = ranal_widget_alloc_(RANAL_WIDGET_BUTTON, parent);
    if (w == NULL) return NULL;
    w->data.button.text = dup_text(text != NULL ? text : "");
    w->width = ranal_text_width(text) + 20;
    w->height = RANAL_FONT_H + 12;
    return w;
}

ranal_widget_t *ranal_checkbox(ranal_widget_t *parent, const char *text, int initial) {
    ranal_widget_t *w;
    if (parent == NULL) parent = g_ranal->root;
    w = ranal_widget_alloc_(RANAL_WIDGET_CHECKBOX, parent);
    if (w == NULL) return NULL;
    w->data.checkbox.text = dup_text(text != NULL ? text : "");
    w->data.checkbox.value = initial ? 1 : 0;
    w->height = 18;
    w->width = 18 + 6 + ranal_text_width(text);
    return w;
}

ranal_widget_t *ranal_slider(ranal_widget_t *parent, float min, float max, float initial) {
    ranal_widget_t *w;
    if (parent == NULL) parent = g_ranal->root;
    w = ranal_widget_alloc_(RANAL_WIDGET_SLIDER, parent);
    if (w == NULL) return NULL;
    w->data.slider.min = min;
    w->data.slider.max = max;
    if (initial < min) initial = min;
    if (initial > max) initial = max;
    w->data.slider.value = initial;
    w->width = 160;
    w->height = 18;
    return w;
}

ranal_widget_t *ranal_textbox(ranal_widget_t *parent, char *buffer, size_t capacity) {
    ranal_widget_t *w;
    if (buffer == NULL || capacity < 2) {
        ranal_set_error_("ranal: textbox needs non-NULL buffer >= 2 bytes");
        return NULL;
    }
    if (parent == NULL) parent = g_ranal->root;
    w = ranal_widget_alloc_(RANAL_WIDGET_TEXTBOX, parent);
    if (w == NULL) return NULL;
    w->data.textbox.buffer = buffer;
    w->data.textbox.capacity = capacity;
    w->data.textbox.length = strnlen(buffer, capacity - 1);
    w->data.textbox.cursor = w->data.textbox.length;
    buffer[w->data.textbox.length] = '\0';
    w->width = 200;
    w->height = RANAL_FONT_H + 10;
    return w;
}

void ranal_destroy_widget(ranal_widget_t *widget) {
    if (widget == NULL) {
        return;
    }
    if (widget->parent != NULL) {
        ranal_widget_t *prev = NULL;
        ranal_widget_t *cur = widget->parent->first_child;
        while (cur != NULL && cur != widget) {
            prev = cur;
            cur = cur->next_sibling;
        }
        if (cur == widget) {
            if (prev == NULL) {
                widget->parent->first_child = widget->next_sibling;
            } else {
                prev->next_sibling = widget->next_sibling;
            }
        }
    }
    if (g_ranal->focused == widget) {
        g_ranal->focused = NULL;
    }
    ranal_widget_free_recursive_(widget);
}

void ranal_invalidate(void) {
    g_ranal->dirty = 1;
}

void ranal_set_pos(ranal_widget_t *w, int32_t x, int32_t y) {
    if (w == NULL) return;
    w->x = x;
    w->y = y;
    g_ranal->dirty = 1;
}

void ranal_set_size(ranal_widget_t *w, int32_t width, int32_t height) {
    if (w == NULL) return;
    w->width = width;
    w->height = height;
    g_ranal->dirty = 1;
}

void ranal_set_auto_size(ranal_widget_t *w, int auto_w, int auto_h) {
    if (w == NULL) return;
    w->auto_w = auto_w ? 1 : 0;
    w->auto_h = auto_h ? 1 : 0;
    g_ranal->dirty = 1;
}

void ranal_set_center_in_parent(ranal_widget_t *w, int center_x, int center_y) {
    if (w == NULL) return;
    w->center_x_in_parent = center_x ? 1 : 0;
    w->center_y_in_parent = center_y ? 1 : 0;
    g_ranal->dirty = 1;
}

void ranal_set_fill_parent(ranal_widget_t *w, int fill_x, int fill_y) {
    if (w == NULL) return;
    w->fill_x_in_parent = fill_x ? 1 : 0;
    w->fill_y_in_parent = fill_y ? 1 : 0;
    g_ranal->dirty = 1;
}

void ranal_set_visible(ranal_widget_t *w, int visible) {
    if (w == NULL) return;
    int next = visible ? 1 : 0;
    if (w->visible == next) return;
    w->visible = next;
    g_ranal->dirty = 1;
}

void ranal_set_enabled(ranal_widget_t *w, int enabled) {
    if (w == NULL) return;
    w->enabled = enabled ? 1 : 0;
    g_ranal->dirty = 1;
}

void ranal_set_text(ranal_widget_t *w, const char *text) {
    if (w == NULL) return;
    char **slot = NULL;
    switch (w->kind) {
        case RANAL_WIDGET_LABEL:    slot = &w->data.label.text; break;
        case RANAL_WIDGET_BUTTON:   slot = &w->data.button.text; break;
        case RANAL_WIDGET_CHECKBOX: slot = &w->data.checkbox.text; break;
        default: return;
    }
    const char *new_text = text != NULL ? text : "";
    if (*slot != NULL && strcmp(*slot, new_text) == 0) {
        return;  
    }
    free(*slot);
    *slot = dup_text(new_text);
    g_ranal->dirty = 1;
}

const char *ranal_get_text(const ranal_widget_t *w) {
    if (w == NULL) return "";
    switch (w->kind) {
        case RANAL_WIDGET_LABEL:    return w->data.label.text ? w->data.label.text : "";
        case RANAL_WIDGET_BUTTON:   return w->data.button.text ? w->data.button.text : "";
        case RANAL_WIDGET_CHECKBOX: return w->data.checkbox.text ? w->data.checkbox.text : "";
        case RANAL_WIDGET_TEXTBOX:  return w->data.textbox.buffer ? w->data.textbox.buffer : "";
        default: return "";
    }
}

void ranal_set_layout(ranal_widget_t *w, ranal_layout_kind_t kind) {
    if (w == NULL) return;
    w->layout = kind;
    g_ranal->dirty = 1;
}

void ranal_set_padding(ranal_widget_t *w, int32_t padding) {
    if (w == NULL) return;
    w->padding = padding;
    g_ranal->dirty = 1;
}

void ranal_set_spacing(ranal_widget_t *w, int32_t spacing) {
    if (w == NULL) return;
    w->spacing = spacing;
    g_ranal->dirty = 1;
}

void ranal_set_align(ranal_widget_t *w, ranal_align_t align) {
    if (w == NULL) return;
    w->align = align;
    g_ranal->dirty = 1;
}

void ranal_set_background(ranal_widget_t *w, ranal_color_t color) {
    if (w == NULL) return;
    if (w->has_bg && w->bg_color == color) return;
    w->bg_color = color;
    w->has_bg = 1;
    if (w->kind == RANAL_WIDGET_PANEL) {
        w->data.panel.bg = color;
    }
    g_ranal->dirty = 1;
}

void ranal_set_foreground(ranal_widget_t *w, ranal_color_t color) {
    if (w == NULL) return;
    if (w->has_fg && w->fg_color == color) return;
    w->fg_color = color;
    w->has_fg = 1;
    if (w->kind == RANAL_WIDGET_LABEL) {
        w->data.label.color = color;
    }
    g_ranal->dirty = 1;
}

int ranal_checkbox_value(const ranal_widget_t *w) {
    if (w == NULL || w->kind != RANAL_WIDGET_CHECKBOX) return 0;
    return w->data.checkbox.value;
}

void ranal_checkbox_set_value(ranal_widget_t *w, int value) {
    if (w == NULL || w->kind != RANAL_WIDGET_CHECKBOX) return;
    int v = value ? 1 : 0;
    if (w->data.checkbox.value == v) return;
    w->data.checkbox.value = v;
    g_ranal->dirty = 1;
}

float ranal_slider_value(const ranal_widget_t *w) {
    if (w == NULL || w->kind != RANAL_WIDGET_SLIDER) return 0.0f;
    return w->data.slider.value;
}

void ranal_slider_set_value(ranal_widget_t *w, float value) {
    if (w == NULL || w->kind != RANAL_WIDGET_SLIDER) return;
    if (value < w->data.slider.min) value = w->data.slider.min;
    if (value > w->data.slider.max) value = w->data.slider.max;
    if (w->data.slider.value == value) return;
    w->data.slider.value = value;
    g_ranal->dirty = 1;
}

void ranal_on_click(ranal_widget_t *w, ranal_on_click_fn fn, void *user) {
    if (w == NULL || w->kind != RANAL_WIDGET_BUTTON) return;
    w->data.button.on_click = fn;
    w->data.button.user = user;
}

void ranal_on_toggle(ranal_widget_t *w, ranal_on_toggle_fn fn, void *user) {
    if (w == NULL || w->kind != RANAL_WIDGET_CHECKBOX) return;
    w->data.checkbox.on_change = fn;
    w->data.checkbox.user = user;
}

void ranal_on_slide(ranal_widget_t *w, ranal_on_slide_fn fn, void *user) {
    if (w == NULL || w->kind != RANAL_WIDGET_SLIDER) return;
    w->data.slider.on_change = fn;
    w->data.slider.user = user;
}

void ranal_on_text(ranal_widget_t *w, ranal_on_text_fn fn, void *user) {
    if (w == NULL || w->kind != RANAL_WIDGET_TEXTBOX) return;
    w->data.textbox.on_change = fn;
    w->data.textbox.user = user;
}
