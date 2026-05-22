#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void destroy_resources(void) {
    if (g_ranal.root != NULL) {
        ranal_widget_free_recursive_(g_ranal.root);
        g_ranal.root = NULL;
    }
    if (g_ranal.input != NULL) {
        srapi_input_destroy(g_ranal.input);
        g_ranal.input = NULL;
    }
    if (g_ranal.cmd != NULL) {
        srapi_destroy_cmd_buffer(g_ranal.cmd);
        g_ranal.cmd = NULL;
    }
    if (g_ranal.ctx != NULL) {
        srapi_destroy_context(g_ranal.ctx);
        g_ranal.ctx = NULL;
    }
    if (g_ranal.drm != NULL) {
        srapi_drm_close(g_ranal.drm);
        g_ranal.drm = NULL;
    }
}

ranal_result_t ranal_init(const ranal_window_desc_t *desc) {
    srapi_drm_recommendation_t rec;
    srapi_result_t r;

    if (g_ranal.initialized) {
        ranal_set_error_("ranal: already initialized");
        return RANAL_ERROR_BAD_ARG;
    }

    memset(&g_ranal, 0, sizeof(g_ranal));

    if (srapi_drm_recommend(&rec) != SRAPI_OK) {
        ranal_set_error_("ranal: %s", srapi_last_error());
        return RANAL_ERROR_BACKEND;
    }

    r = srapi_drm_open_display(&(srapi_drm_display_desc_t){
        .device_path = rec.path,
        .width = desc != NULL && desc->width > 0 ? (uint32_t)desc->width : 0,
        .height = desc != NULL && desc->height > 0 ? (uint32_t)desc->height : 0,
    }, &g_ranal.drm);
    if (r != SRAPI_OK) {
        ranal_set_error_("ranal: drm open: %s", srapi_last_error());
        return RANAL_ERROR_BACKEND;
    }

    g_ranal.width = (int32_t)srapi_drm_width(g_ranal.drm);
    g_ranal.height = (int32_t)srapi_drm_height(g_ranal.drm);

    r = srapi_create_context(&(srapi_context_desc_t){
        .width = (uint32_t)g_ranal.width,
        .height = (uint32_t)g_ranal.height,
        .backend = SRAPI_BACKEND_GPU,
    }, &g_ranal.ctx);
    if (r != SRAPI_OK) {
        ranal_set_error_("ranal: context: %s", srapi_last_error());
        destroy_resources();
        return RANAL_ERROR_BACKEND;
    }

    r = srapi_create_cmd_buffer(g_ranal.ctx, &g_ranal.cmd);
    if (r != SRAPI_OK) {
        ranal_set_error_("ranal: cmd buffer: %s", srapi_last_error());
        destroy_resources();
        return RANAL_ERROR_BACKEND;
    }

    r = srapi_input_create(&(srapi_input_desc_t){
        .auto_discover = 1,
        .initial_mouse_x = g_ranal.width / 2,
        .initial_mouse_y = g_ranal.height / 2,
    }, &g_ranal.input);
    if (r != SRAPI_OK) {
        ranal_set_error_("ranal: input: %s", srapi_last_error());
        destroy_resources();
        return RANAL_ERROR_BACKEND;
    }
    srapi_input_set_bounds(g_ranal.input, g_ranal.width, g_ranal.height);

    g_ranal.root = calloc(1, sizeof(*g_ranal.root));
    if (g_ranal.root == NULL) {
        ranal_set_error_("ranal: oom on root");
        destroy_resources();
        return RANAL_ERROR_OOM;
    }
    g_ranal.root->kind = RANAL_WIDGET_PANEL;
    g_ranal.root->visible = 1;
    g_ranal.root->enabled = 1;
    g_ranal.root->width = g_ranal.width;
    g_ranal.root->height = g_ranal.height;
    g_ranal.root->layout = RANAL_LAYOUT_ABSOLUTE;
    g_ranal.theme = &ranal_theme_dark;
    g_ranal.mouse_x = g_ranal.width / 2;
    g_ranal.mouse_y = g_ranal.height / 2;

    srapi_clock_init(&g_ranal.frame_clock);
    srapi_clock_init(&g_ranal.total_clock);

    g_ranal.dirty = 1;
    g_ranal.presented = 0;
    g_ranal.initialized = 1;
    (void)desc;
    return RANAL_OK;
}

void ranal_shutdown(void) {
    if (!g_ranal.initialized) return;
    destroy_resources();
    memset(&g_ranal, 0, sizeof(g_ranal));
}

int ranal_should_close(void) {
    return g_ranal.should_close;
}

void ranal_request_close(void) {
    g_ranal.should_close = 1;
}

int32_t ranal_window_width(void) { return g_ranal.width; }
int32_t ranal_window_height(void) { return g_ranal.height; }
float   ranal_frame_time(void) { return g_ranal.dt; }
double  ranal_time_elapsed(void) { return (double)srapi_clock_elapsed(&g_ranal.total_clock); }
ranal_result_t ranal_record_start(const char *path, uint32_t fps) {
    if (!g_ranal.initialized || g_ranal.drm == NULL) {
        ranal_set_error_("ranal: not initialized");
        return RANAL_ERROR;
    }
    if (fps == 0) {
        fps = 30;
    }
    if (srapi_drm_record_start(g_ranal.drm, path, fps * 1000) != SRAPI_OK) {
        ranal_set_error_("ranal: %s", srapi_last_error());
        return RANAL_ERROR;
    }
    return RANAL_OK;
}
void ranal_record_stop(void) {
    if (g_ranal.initialized && g_ranal.drm != NULL) {
        srapi_drm_record_stop(g_ranal.drm);
    }
}
static void size_pass(ranal_widget_t *w) {
    if (w == NULL) return;
    for (ranal_widget_t *c = w->first_child; c != NULL; c = c->next_sibling) {
        size_pass(c);
    }

    int32_t cw = w->width;
    int32_t ch = w->height;

    if (w->kind == RANAL_WIDGET_PANEL && (w->auto_w || w->auto_h)) {
        int32_t inner_w = 0;
        int32_t inner_h = 0;
        int n_visible = 0;
        for (ranal_widget_t *c = w->first_child; c != NULL; c = c->next_sibling) {
            if (!c->visible) continue;
            n_visible++;
            int32_t ccw = c->computed_w;
            int32_t cch = c->computed_h;
            if (w->layout == RANAL_LAYOUT_VERT) {
                if (ccw > inner_w) inner_w = ccw;
                inner_h += cch;
            } else if (w->layout == RANAL_LAYOUT_HORIZ) {
                inner_w += ccw;
                if (cch > inner_h) inner_h = cch;
            } else { 
                int32_t right = c->x + ccw;
                int32_t bottom = c->y + cch;
                if (right > inner_w) inner_w = right;
                if (bottom > inner_h) inner_h = bottom;
            }
        }
        if (n_visible > 1 && (w->layout == RANAL_LAYOUT_VERT || w->layout == RANAL_LAYOUT_HORIZ)) {
            int32_t gap = w->spacing * (n_visible - 1);
            if (w->layout == RANAL_LAYOUT_VERT) inner_h += gap;
            else inner_w += gap;
        }
        if (w->auto_w) cw = inner_w + 2 * w->padding;
        if (w->auto_h) ch = inner_h + 2 * w->padding;
    }

    w->computed_w = cw;
    w->computed_h = ch;
}

static void position_pass(ranal_widget_t *w, int32_t origin_x, int32_t origin_y);

static void position_panel_children(ranal_widget_t *panel) {
    if (panel->layout == RANAL_LAYOUT_ABSOLUTE) {
        for (ranal_widget_t *c = panel->first_child; c != NULL; c = c->next_sibling) {
            if (c->fill_x_in_parent) {
                c->computed_w = panel->computed_w - 2 * panel->padding;
            }
            if (c->fill_y_in_parent) {
                c->computed_h = panel->computed_h - 2 * panel->padding;
            }
            int32_t ox = panel->computed_x + c->x;
            int32_t oy = panel->computed_y + c->y;
            if (c->center_x_in_parent) {
                ox = panel->computed_x + (panel->computed_w - c->computed_w) / 2;
            }
            if (c->center_y_in_parent) {
                oy = panel->computed_y + (panel->computed_h - c->computed_h) / 2;
            }
            position_pass(c, ox, oy);
        }
        return;
    }

    int32_t cursor_x = panel->computed_x + panel->padding;
    int32_t cursor_y = panel->computed_y + panel->padding;

    for (ranal_widget_t *c = panel->first_child; c != NULL; c = c->next_sibling) {
        if (!c->visible) continue;
        c->computed_x = cursor_x;
        c->computed_y = cursor_y;
        position_panel_children(c);
        if (panel->layout == RANAL_LAYOUT_VERT) {
            cursor_y += c->computed_h + panel->spacing;
        } else {
            cursor_x += c->computed_w + panel->spacing;
        }
    }
}

static void position_pass(ranal_widget_t *w, int32_t origin_x, int32_t origin_y) {
    if (!w->visible) return;
    w->computed_x = origin_x;
    w->computed_y = origin_y;
    if (w->kind == RANAL_WIDGET_PANEL) {
        position_panel_children(w);
    }
}

void ranal_layout_pass_(ranal_widget_t *root) {
    if (root == NULL) return;
    root->computed_w = g_ranal.width;
    root->computed_h = g_ranal.height;
    size_pass(root);
    root->computed_w = g_ranal.width;
    root->computed_h = g_ranal.height;
    position_pass(root, 0, 0);
}
static int rect_contains(const ranal_widget_t *w, int32_t mx, int32_t my) {
    return mx >= w->computed_x && mx < w->computed_x + w->computed_w &&
           my >= w->computed_y && my < w->computed_y + w->computed_h;
}

ranal_widget_t *ranal_hit_test_(ranal_widget_t *root, int32_t mx, int32_t my) {
    if (root == NULL || !root->visible) return NULL;
    ranal_widget_t *hit = NULL;
    for (ranal_widget_t *c = root->first_child; c != NULL; c = c->next_sibling) {
        ranal_widget_t *child_hit = ranal_hit_test_(c, mx, my);
        if (child_hit != NULL) hit = child_hit;
    }
    if (hit != NULL) return hit;
    if (root != g_ranal.root && rect_contains(root, mx, my)) {
        return root;
    }
    return NULL;
}

static void clear_hover(ranal_widget_t *w) {
    if (w == NULL) return;
    switch (w->kind) {
        case RANAL_WIDGET_BUTTON:   w->data.button.hover = 0; w->data.button.pressed = 0; break;
        case RANAL_WIDGET_CHECKBOX: w->data.checkbox.hover = 0; break;
        case RANAL_WIDGET_SLIDER:   w->data.slider.hover = 0; break;
        case RANAL_WIDGET_TEXTBOX:  w->data.textbox.hover = 0; break;
        default: break;
    }
    for (ranal_widget_t *c = w->first_child; c != NULL; c = c->next_sibling) {
        clear_hover(c);
    }
}

static void apply_slider_drag(ranal_widget_t *s, int32_t mx) {
    if (s->computed_w <= 0) return;
    float t = (float)(mx - s->computed_x) / (float)s->computed_w;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float old = s->data.slider.value;
    float v = s->data.slider.min + t * (s->data.slider.max - s->data.slider.min);
    s->data.slider.value = v;
    if (v != old && s->data.slider.on_change != NULL) {
        s->data.slider.on_change(s, v, s->data.slider.user);
    }
}

static void textbox_insert_char(ranal_widget_t *t, char ch) {
    if (t->data.textbox.length + 1 >= t->data.textbox.capacity) return;
    memmove(t->data.textbox.buffer + t->data.textbox.cursor + 1,
            t->data.textbox.buffer + t->data.textbox.cursor,
            t->data.textbox.length - t->data.textbox.cursor);
    t->data.textbox.buffer[t->data.textbox.cursor] = ch;
    t->data.textbox.cursor++;
    t->data.textbox.length++;
    t->data.textbox.buffer[t->data.textbox.length] = '\0';
    if (t->data.textbox.on_change != NULL) {
        t->data.textbox.on_change(t, t->data.textbox.buffer, t->data.textbox.user);
    }
}

static void textbox_backspace(ranal_widget_t *t) {
    if (t->data.textbox.cursor == 0) return;
    memmove(t->data.textbox.buffer + t->data.textbox.cursor - 1,
            t->data.textbox.buffer + t->data.textbox.cursor,
            t->data.textbox.length - t->data.textbox.cursor);
    t->data.textbox.cursor--;
    t->data.textbox.length--;
    t->data.textbox.buffer[t->data.textbox.length] = '\0';
    if (t->data.textbox.on_change != NULL) {
        t->data.textbox.on_change(t, t->data.textbox.buffer, t->data.textbox.user);
    }
}

char ranal_scancode_to_char_(srapi_scancode_t sc, uint32_t mods) {
    int shift = (mods & SRAPI_KMOD_SHIFT) != 0;
    if (sc >= SRAPI_SCANCODE_A && sc <= SRAPI_SCANCODE_Z) {
        char base = 'a' + (sc - SRAPI_SCANCODE_A);
        return shift ? (char)(base - 32) : base;
    }
    if (sc >= SRAPI_SCANCODE_1 && sc <= SRAPI_SCANCODE_9) {
        static const char unshift[] = "123456789";
        static const char shifted[] = "!@#$%^&*(";
        int idx = sc - SRAPI_SCANCODE_1;
        return shift ? shifted[idx] : unshift[idx];
    }
    switch (sc) {
        case SRAPI_SCANCODE_0:            return shift ? ')' : '0';
        case SRAPI_SCANCODE_SPACE:        return ' ';
        case SRAPI_SCANCODE_MINUS:        return shift ? '_' : '-';
        case SRAPI_SCANCODE_EQUALS:       return shift ? '+' : '=';
        case SRAPI_SCANCODE_LEFTBRACKET:  return shift ? '{' : '[';
        case SRAPI_SCANCODE_RIGHTBRACKET: return shift ? '}' : ']';
        case SRAPI_SCANCODE_BACKSLASH:    return shift ? '|' : '\\';
        case SRAPI_SCANCODE_SEMICOLON:    return shift ? ':' : ';';
        case SRAPI_SCANCODE_APOSTROPHE:   return shift ? '"' : '\'';
        case SRAPI_SCANCODE_GRAVE:        return shift ? '~' : '`';
        case SRAPI_SCANCODE_COMMA:        return shift ? '<' : ',';
        case SRAPI_SCANCODE_PERIOD:       return shift ? '>' : '.';
        case SRAPI_SCANCODE_SLASH:        return shift ? '?' : '/';
        default: return 0;
    }
}

static void update_slider_drags(ranal_widget_t *w, int32_t mx, int mouse_held) {
    if (w == NULL) return;
    if (w->kind == RANAL_WIDGET_SLIDER) {
        if (mouse_held && w->data.slider.dragging) {
            apply_slider_drag(w, mx);
        } else if (!mouse_held) {
            w->data.slider.dragging = 0;
        }
    }
    for (ranal_widget_t *c = w->first_child; c != NULL; c = c->next_sibling) {
        update_slider_drags(c, mx, mouse_held);
    }
}

void ranal_event_pass_(void) {
    srapi_input_event_t ev;
    g_ranal.prev_mouse_left = g_ranal.curr_mouse_left;

    while (srapi_input_poll(g_ranal.input, &ev) == 1) {
        g_ranal.dirty = 1;
        switch (ev.type) {
            case SRAPI_INPUT_EVENT_KEY_DOWN: {
                if (ev.key.scancode == SRAPI_SCANCODE_ESCAPE) {
                    g_ranal.should_close = 1;
                    break;
                }
                if (g_ranal.focused != NULL &&
                    g_ranal.focused->kind == RANAL_WIDGET_TEXTBOX) {
                    if (ev.key.scancode == SRAPI_SCANCODE_BACKSPACE) {
                        textbox_backspace(g_ranal.focused);
                    } else if (ev.key.scancode == SRAPI_SCANCODE_RETURN) {
                        g_ranal.focused->data.textbox.focused = 0;
                        g_ranal.focused = NULL;
                    } else {
                        char ch = ranal_scancode_to_char_(ev.key.scancode, ev.key.modifiers);
                        if (ch != 0) {
                            textbox_insert_char(g_ranal.focused, ch);
                        }
                    }
                }
                break;
            }
            case SRAPI_INPUT_EVENT_MOUSE_MOTION:
                g_ranal.mouse_x = ev.mouse_motion.x;
                g_ranal.mouse_y = ev.mouse_motion.y;
                break;
            case SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT) {
                    g_ranal.curr_mouse_left = 1;
                    g_ranal.mouse_x = ev.mouse_button.x;
                    g_ranal.mouse_y = ev.mouse_button.y;
                }
                break;
            case SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP:
                if (ev.mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT) {
                    g_ranal.curr_mouse_left = 0;
                }
                break;
            default:
                break;
        }
    }

    clear_hover(g_ranal.root);
    ranal_widget_t *hot = ranal_hit_test_(g_ranal.root, g_ranal.mouse_x, g_ranal.mouse_y);
    if (hot != NULL && hot->enabled) {
        switch (hot->kind) {
            case RANAL_WIDGET_BUTTON:
                hot->data.button.hover = 1;
                if (g_ranal.curr_mouse_left) hot->data.button.pressed = 1;
                break;
            case RANAL_WIDGET_CHECKBOX: hot->data.checkbox.hover = 1; break;
            case RANAL_WIDGET_SLIDER:   hot->data.slider.hover = 1; break;
            case RANAL_WIDGET_TEXTBOX:  hot->data.textbox.hover = 1; break;
            default: break;
        }
    }

    int just_pressed = (g_ranal.curr_mouse_left && !g_ranal.prev_mouse_left);
    int just_released = (!g_ranal.curr_mouse_left && g_ranal.prev_mouse_left);

    if (just_pressed && hot != NULL && hot->enabled) {
        if (hot->kind == RANAL_WIDGET_SLIDER) {
            hot->data.slider.dragging = 1;
            apply_slider_drag(hot, g_ranal.mouse_x);
        } else if (hot->kind == RANAL_WIDGET_TEXTBOX) {
            if (g_ranal.focused != NULL && g_ranal.focused->kind == RANAL_WIDGET_TEXTBOX) {
                g_ranal.focused->data.textbox.focused = 0;
            }
            g_ranal.focused = hot;
            hot->data.textbox.focused = 1;
        }
    } else if (just_pressed && hot == NULL) {
        if (g_ranal.focused != NULL && g_ranal.focused->kind == RANAL_WIDGET_TEXTBOX) {
            g_ranal.focused->data.textbox.focused = 0;
            g_ranal.focused = NULL;
        }
    }

    if (just_released) {
        if (hot != NULL && hot->enabled) {
            if (hot->kind == RANAL_WIDGET_BUTTON && hot->data.button.on_click != NULL) {
                hot->data.button.on_click(hot, hot->data.button.user);
            } else if (hot->kind == RANAL_WIDGET_CHECKBOX) {
                hot->data.checkbox.value = !hot->data.checkbox.value;
                if (hot->data.checkbox.on_change != NULL) {
                    hot->data.checkbox.on_change(hot, hot->data.checkbox.value, hot->data.checkbox.user);
                }
            }
        }
        update_slider_drags(g_ranal.root, g_ranal.mouse_x, 0);
    }

    if (g_ranal.curr_mouse_left) {
        update_slider_drags(g_ranal.root, g_ranal.mouse_x, 1);
    }
}


static ranal_color_t role_color(ranal_role_t role, const ranal_theme_t *t) {
    switch (role) {
        case RANAL_ROLE_ACCENT: return t->accent;
        case RANAL_ROLE_DIM:    return t->text_dim;
        case RANAL_ROLE_DANGER: return t->danger;
        default:                return t->text;
    }
}

static ranal_color_t resolve_fg(const ranal_widget_t *w, const ranal_theme_t *t) {
    if (w->has_fg) return w->fg_color;
    return role_color(w->role, t);
}

static void render_panel(ranal_widget_t *w) {
    const ranal_theme_t *t = ranal_get_theme();
    ranal_color_t bg = w->has_bg ? w->bg_color
                     : (w->parent == NULL ? t->bg : t->panel_bg);
    srapi_cmd_fill_rect(g_ranal.cmd,
        w->computed_x, w->computed_y,
        (uint32_t)w->computed_w, (uint32_t)w->computed_h, bg);
}

static void render_label(ranal_widget_t *w) {
    const ranal_theme_t *t = ranal_get_theme();
    ranal_text_render_(w->computed_x, w->computed_y, w->data.label.text, resolve_fg(w, t));
}

static void render_button(ranal_widget_t *w) {
    const ranal_theme_t *t = ranal_get_theme();
    ranal_color_t bg;
    if (!w->enabled)                  bg = t->button_bg_disabled;
    else if (w->data.button.pressed)  bg = t->button_bg_pressed;
    else if (w->data.button.hover)    bg = t->button_bg_hover;
    else                              bg = t->button_bg;

    srapi_cmd_fill_rect(g_ranal.cmd,
        w->computed_x, w->computed_y,
        (uint32_t)w->computed_w, (uint32_t)w->computed_h, bg);
    ranal_draw_rect_lines(w->computed_x, w->computed_y, w->computed_w, w->computed_h, 1, t->border);

    if (w->data.button.text != NULL) {
        int32_t tw = ranal_text_width(w->data.button.text);
        int32_t tx = w->computed_x + (w->computed_w - tw) / 2;
        int32_t ty = w->computed_y + (w->computed_h - RANAL_GLYPH_H) / 2;
        ranal_text_render_(tx, ty, w->data.button.text, resolve_fg(w, t));
    }
}

static void render_checkbox(ranal_widget_t *w) {
    const ranal_theme_t *t = ranal_get_theme();
    int32_t box = 14;
    int32_t bx = w->computed_x;
    int32_t by = w->computed_y + (w->computed_h - box) / 2;
    ranal_color_t border = w->data.checkbox.hover ? t->accent_hot : t->border;
    srapi_cmd_fill_rect(g_ranal.cmd, bx, by, (uint32_t)box, (uint32_t)box, t->textbox_bg);
    ranal_draw_rect_lines(bx, by, box, box, 1, border);
    if (w->data.checkbox.value) {
        srapi_cmd_fill_rect(g_ranal.cmd, bx + 3, by + 3,
                            (uint32_t)(box - 6), (uint32_t)(box - 6), t->accent);
    }
    if (w->data.checkbox.text != NULL) {
        ranal_text_render_(bx + box + 6, by + (box - RANAL_GLYPH_H) / 2,
                           w->data.checkbox.text, resolve_fg(w, t));
    }
}

static void render_slider(ranal_widget_t *w) {
    const ranal_theme_t *t = ranal_get_theme();
    int32_t track_h = 4;
    int32_t track_y = w->computed_y + (w->computed_h - track_h) / 2;
    srapi_cmd_fill_rect(g_ranal.cmd,
        w->computed_x, track_y,
        (uint32_t)w->computed_w, (uint32_t)track_h, t->border);

    float range = w->data.slider.max - w->data.slider.min;
    float r = range > 0.0f ? (w->data.slider.value - w->data.slider.min) / range : 0.0f;
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    int32_t thumb_w = 12;
    int32_t thumb_h = w->computed_h - 2;
    int32_t thumb_x = w->computed_x + (int32_t)(r * (float)(w->computed_w - thumb_w));
    int32_t thumb_y = w->computed_y + 1;
    ranal_color_t color = w->data.slider.dragging ? t->accent_hot
                       : w->data.slider.hover     ? t->accent
                                                  : t->text_dim;
    srapi_cmd_fill_rect(g_ranal.cmd, thumb_x, thumb_y,
                        (uint32_t)thumb_w, (uint32_t)thumb_h, color);
}

static void render_textbox(ranal_widget_t *w) {
    const ranal_theme_t *t = ranal_get_theme();
    ranal_color_t border = w->data.textbox.focused ? t->accent
                        : w->data.textbox.hover    ? t->accent_hot
                                                   : t->border;
    srapi_cmd_fill_rect(g_ranal.cmd,
        w->computed_x, w->computed_y,
        (uint32_t)w->computed_w, (uint32_t)w->computed_h, t->textbox_bg);
    ranal_draw_rect_lines(w->computed_x, w->computed_y, w->computed_w, w->computed_h, 1, border);
    if (w->data.textbox.buffer != NULL) {
        ranal_text_render_(w->computed_x + 4,
                           w->computed_y + (w->computed_h - RANAL_GLYPH_H) / 2,
                           w->data.textbox.buffer, resolve_fg(w, t));
    }
    if (w->data.textbox.focused) {
        int32_t cur_x = w->computed_x + 4 + (int32_t)w->data.textbox.cursor * RANAL_FONT_W;
        srapi_cmd_fill_rect(g_ranal.cmd,
            cur_x, w->computed_y + 3,
            1, (uint32_t)(w->computed_h - 6),
            t->accent);
    }
}

void ranal_render_pass_(ranal_widget_t *root) {
    if (root == NULL || !root->visible) return;
    switch (root->kind) {
        case RANAL_WIDGET_PANEL:    render_panel(root); break;
        case RANAL_WIDGET_LABEL:    render_label(root); return;
        case RANAL_WIDGET_BUTTON:   render_button(root); return;
        case RANAL_WIDGET_CHECKBOX: render_checkbox(root); return;
        case RANAL_WIDGET_SLIDER:   render_slider(root); return;
        case RANAL_WIDGET_TEXTBOX:  render_textbox(root); return;
    }
    for (ranal_widget_t *c = root->first_child; c != NULL; c = c->next_sibling) {
        ranal_render_pass_(c);
    }
}

static void draw_cursor(int32_t x, int32_t y) {
    srapi_cmd_fill_triangle(g_ranal.cmd,
        x,     y,
        x,     y + 14,
        x + 10, y + 10,
        RANAL_COLOR(0, 0, 0));
    srapi_cmd_fill_triangle(g_ranal.cmd,
        x + 1, y + 2,
        x + 1, y + 12,
        x + 8, y + 9,
        RANAL_COLOR(255, 255, 255));
    srapi_cmd_fill_triangle(g_ranal.cmd,
        x + 5, y + 10,
        x + 11, y + 16,
        x + 7, y + 16,
        RANAL_COLOR(0, 0, 0));
    srapi_cmd_fill_triangle(g_ranal.cmd,
        x + 6, y + 11,
        x + 10, y + 15,
        x + 7, y + 15,
        RANAL_COLOR(255, 255, 255));
}


int ranal_frame(void) {
    if (!g_ranal.initialized) return 1;
    g_ranal.dt = srapi_clock_tick(&g_ranal.frame_clock);
    ranal_event_pass_();
    int frames_needed = g_ranal.presented >= 2 ? 0 : 2 - g_ranal.presented;
    if (!g_ranal.dirty && frames_needed == 0) {
        struct timespec ts = { 0, 8 * 1000 * 1000 };  
        nanosleep(&ts, NULL);
        return g_ranal.should_close;
    }
    ranal_layout_pass_(g_ranal.root);
    srapi_cmd_reset(g_ranal.cmd);
    srapi_cmd_emit(g_ranal.cmd, &(srapi_command_t){
        .kind = SRAPI_COMMAND_CLEAR,
        .color = g_ranal.root->has_bg ? g_ranal.root->bg_color : ranal_get_theme()->bg,
    });
    ranal_render_pass_(g_ranal.root);
    draw_cursor(g_ranal.mouse_x, g_ranal.mouse_y);

    srapi_framebuffer_t *fb = srapi_drm_backbuffer(g_ranal.drm);
    srapi_result_t r = srapi_submit(g_ranal.ctx, fb, g_ranal.cmd);
    if (r != SRAPI_OK) {
        ranal_set_error_("ranal: submit: %s", srapi_last_error());
        return 1;
    }

    r = srapi_drm_present(g_ranal.drm);
    if (r != SRAPI_OK) {
        ranal_set_error_("ranal: present: %s", srapi_last_error());
        return 1;
    }

    g_ranal.dirty = 0;
    if (g_ranal.presented < 2) g_ranal.presented++;
    return g_ranal.should_close;
}

void ranal_run(void) {
    while (!ranal_frame()) {
    }
}
