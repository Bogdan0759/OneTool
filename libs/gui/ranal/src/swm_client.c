#include "internal.h"

#include <sprot/client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void cleanup_partial(struct ranal_context *c) {
    if (c->target != NULL) {
        if (c->target->fb != NULL && c->target->owns_fb) {
            srapi_destroy_framebuffer(c->target->fb);
        }
        free(c->target);
        c->target = NULL;
    }
    if (c->cmd != NULL) {
        srapi_destroy_cmd_buffer(c->cmd);
        c->cmd = NULL;
    }
    if (c->ctx != NULL) {
        srapi_destroy_context(c->ctx);
        c->ctx = NULL;
    }
    if (c->sprot_surface != NULL) {
        sprot_destroy_surface((sprot_surface_t *)c->sprot_surface);
        c->sprot_surface = NULL;
    }
    if (c->sprot_conn != NULL) {
        sprot_disconnect((sprot_connection_t *)c->sprot_conn);
        c->sprot_conn = NULL;
    }
    if (c->root != NULL) {
        ranal_widget_free_recursive_(c->root);
        c->root = NULL;
    }
}

ranal_result_t ranal_init_swm(const char *title, int32_t width, int32_t height) {
    if (g_ranal->initialized) {
        ranal_set_error_("ranal: already initialized");
        return RANAL_ERROR_BAD_ARG;
    }
    if (width <= 0 || height <= 0) {
        ranal_set_error_("ranal_swm: bad size");
        return RANAL_ERROR_BAD_ARG;
    }
    memset(g_ranal, 0, sizeof(*g_ranal));

    sprot_connection_t *conn = sprot_connect(NULL);
    if (conn == NULL) {
        ranal_set_error_("ranal_swm: %s", sprot_last_error());
        return RANAL_ERROR_BACKEND;
    }
    g_ranal->sprot_conn = conn;

    sprot_surface_t *surf = sprot_create_surface(conn, (uint32_t)width, (uint32_t)height);
    if (surf == NULL) {
        ranal_set_error_("ranal_swm: %s", sprot_last_error());
        cleanup_partial(g_ranal);
        return RANAL_ERROR_BACKEND;
    }
    g_ranal->sprot_surface = surf;

    sprot_event_t sev;
    int got_id = 0;
    for (int tries = 0; tries < 80 && !got_id; tries++) {
        int r = sprot_poll_event(conn, &sev, 50);
        if (r < 0) break;
        if (r == 0) continue;
        if (sev.kind == SPROT_EVENT_SURFACE_CREATED) {
            got_id = 1;
        } else if (sev.kind == SPROT_EVENT_DISCONNECT) {
            break;
        }
    }
    if (!got_id) {
        ranal_set_error_("ranal_swm: server did not confirm surface");
        cleanup_partial(g_ranal);
        return RANAL_ERROR_BACKEND;
    }
    if (title != NULL && *title != '\0') {
        sprot_set_title(surf, title);
    }

    if (srapi_create_context(&(srapi_context_desc_t){
            .width = (uint32_t)width, .height = (uint32_t)height,
            .backend = SRAPI_BACKEND_CPU,
        }, &g_ranal->ctx) != SRAPI_OK) {
        ranal_set_error_("ranal_swm: %s", srapi_last_error());
        cleanup_partial(g_ranal);
        return RANAL_ERROR_BACKEND;
    }
    if (srapi_create_cmd_buffer(g_ranal->ctx, &g_ranal->cmd) != SRAPI_OK) {
        ranal_set_error_("ranal_swm: %s", srapi_last_error());
        cleanup_partial(g_ranal);
        return RANAL_ERROR_BACKEND;
    }

    g_ranal->target = calloc(1, sizeof(*g_ranal->target));
    if (g_ranal->target == NULL) {
        ranal_set_error_("ranal_swm: oom");
        cleanup_partial(g_ranal);
        return RANAL_ERROR_OOM;
    }
    srapi_framebuffer_t *fb = NULL;
    if (srapi_create_framebuffer(g_ranal->ctx, &(srapi_framebuffer_desc_t){
            .width = (uint32_t)width, .height = (uint32_t)height,
        }, &fb) != SRAPI_OK) {
        ranal_set_error_("ranal_swm: %s", srapi_last_error());
        cleanup_partial(g_ranal);
        return RANAL_ERROR_BACKEND;
    }
    g_ranal->target->fb = fb;
    g_ranal->target->owns_fb = 1;
    g_ranal->target->pixels = srapi_framebuffer_pixels(fb);
    g_ranal->target->width = width;
    g_ranal->target->height = height;
    g_ranal->target->pitch = (int32_t)srapi_framebuffer_pitch(fb);

    g_ranal->width = width;
    g_ranal->height = height;
    g_ranal->root = calloc(1, sizeof(*g_ranal->root));
    if (g_ranal->root == NULL) {
        ranal_set_error_("ranal_swm: oom on root");
        cleanup_partial(g_ranal);
        return RANAL_ERROR_OOM;
    }
    g_ranal->root->kind = RANAL_WIDGET_PANEL;
    g_ranal->root->visible = 1;
    g_ranal->root->enabled = 1;
    g_ranal->root->width = width;
    g_ranal->root->height = height;
    g_ranal->root->layout = RANAL_LAYOUT_ABSOLUTE;
    g_ranal->theme = &ranal_theme_dark;
    g_ranal->mouse_x = width / 2;
    g_ranal->mouse_y = height / 2;
    srapi_clock_init(&g_ranal->frame_clock);
    srapi_clock_init(&g_ranal->total_clock);
    g_ranal->dirty = 1;
    g_ranal->targets_drm = 0;
    g_ranal->initialized = 1;
    return RANAL_OK;
}

ranal_result_t ranal_set_window_title(const char *title) {
    if (!g_ranal->initialized || g_ranal->sprot_surface == NULL) {
        ranal_set_error_("ranal: not in swm mode");
        return RANAL_ERROR_BAD_ARG;
    }
    if (sprot_set_title((sprot_surface_t *)g_ranal->sprot_surface, title) != 0) {
        ranal_set_error_("ranal_swm: %s", sprot_last_error());
        return RANAL_ERROR;
    }
    return RANAL_OK;
}

int ranal_is_swm_mode(void) {
    return g_ranal->initialized && g_ranal->sprot_conn != NULL;
}

void ranal_swm_pump_events_(void) {
    if (g_ranal->sprot_conn == NULL) return;
    sprot_event_t ev;
    sprot_connection_t *conn = (sprot_connection_t *)g_ranal->sprot_conn;
    int timeout = g_ranal->sprot_pending_frame ? 50 : 0;
    g_ranal->prev_mouse_left = g_ranal->curr_mouse_left;

    for (;;) {
        int r = sprot_poll_event(conn, &ev, timeout);
        if (r <= 0) break;
        timeout = 0;
        switch (ev.kind) {
            case SPROT_EVENT_POINTER_MOTION:
                g_ranal->dirty = 1;
                g_ranal->mouse_x = ev.u.pointer_motion.x;
                g_ranal->mouse_y = ev.u.pointer_motion.y;
                if (g_ranal->mouse_hook != NULL) {
                    ranal_mouse_event_t me = {0};
                    me.kind = RANAL_MOUSE_MOTION;
                    me.x = ev.u.pointer_motion.x;
                    me.y = ev.u.pointer_motion.y;
                    g_ranal->mouse_hook(&me, g_ranal->mouse_hook_user);
                }
                break;
            case SPROT_EVENT_POINTER_BUTTON: {
                g_ranal->dirty = 1;
                int pressed = (ev.u.pointer_button.state == SPROT_BUTTON_STATE_PRESSED);
                if (ev.u.pointer_button.button == 1) {
                    g_ranal->curr_mouse_left = pressed;
                }
                if (g_ranal->mouse_hook != NULL) {
                    ranal_mouse_event_t me = {0};
                    me.kind = pressed ? RANAL_MOUSE_BUTTON_DOWN : RANAL_MOUSE_BUTTON_UP;
                    me.x = g_ranal->mouse_x;
                    me.y = g_ranal->mouse_y;
                    me.button = (int)ev.u.pointer_button.button;
                    g_ranal->mouse_hook(&me, g_ranal->mouse_hook_user);
                }
                break;
            }
            case SPROT_EVENT_POINTER_AXIS:
                g_ranal->dirty = 1;
                if (g_ranal->mouse_hook != NULL) {
                    ranal_mouse_event_t me = {0};
                    me.kind = RANAL_MOUSE_WHEEL;
                    me.x = g_ranal->mouse_x;
                    me.y = g_ranal->mouse_y;
                    /* sprot axis value: positive = scroll down/right.
                     * Our wheel_y convention matches srapi: positive = up. */
                    me.wheel_x = -ev.u.pointer_axis.dx / 10;
                    me.wheel_y = -ev.u.pointer_axis.dy / 10;
                    g_ranal->mouse_hook(&me, g_ranal->mouse_hook_user);
                }
                break;
            case SPROT_EVENT_KEY: {
                g_ranal->dirty = 1;
                int pressed = (ev.u.key.state == SPROT_KEY_STATE_PRESSED);
                if (g_ranal->key_hook != NULL &&
                    g_ranal->key_hook(ev.u.key.scancode,
                                      ev.u.key.modifiers,
                                      pressed,
                                      g_ranal->key_hook_user)) {
                    break;
                }
                if (pressed &&
                    g_ranal->focused != NULL &&
                    g_ranal->focused->kind == RANAL_WIDGET_TEXTBOX) {
                    if (ev.u.key.scancode == 42 /* BACKSPACE */) {
                        if (g_ranal->focused->data.textbox.length > 0) {
                            g_ranal->focused->data.textbox.length--;
                            g_ranal->focused->data.textbox.cursor = g_ranal->focused->data.textbox.length;
                            g_ranal->focused->data.textbox.buffer[g_ranal->focused->data.textbox.length] = '\0';
                        }
                    } else {
                        char ch = ranal_scancode_to_char_((srapi_scancode_t)ev.u.key.scancode, ev.u.key.modifiers);
                        if (ch != 0 && g_ranal->focused->data.textbox.length + 1 < g_ranal->focused->data.textbox.capacity) {
                            size_t idx = g_ranal->focused->data.textbox.length;
                            g_ranal->focused->data.textbox.buffer[idx] = ch;
                            g_ranal->focused->data.textbox.length++;
                            g_ranal->focused->data.textbox.cursor = g_ranal->focused->data.textbox.length;
                            g_ranal->focused->data.textbox.buffer[g_ranal->focused->data.textbox.length] = '\0';
                        }
                    }
                }
                break;
            }
            case SPROT_EVENT_SURFACE_CONFIGURE: {
                int32_t new_w = (int32_t)ev.u.configure.width;
                int32_t new_h = (int32_t)ev.u.configure.height;
                if (new_w <= 0 || new_h <= 0) break;
                if (new_w == g_ranal->width && new_h == g_ranal->height) break;
                if (sprot_resize_surface((sprot_surface_t *)g_ranal->sprot_surface,
                                         (uint32_t)new_w, (uint32_t)new_h) != 0) {
                    break;
                }
                if (g_ranal->target != NULL) {
                    if (g_ranal->target->fb != NULL && g_ranal->target->owns_fb) {
                        srapi_destroy_framebuffer(g_ranal->target->fb);
                        g_ranal->target->fb = NULL;
                    }
                    srapi_framebuffer_t *fb = NULL;
                    if (srapi_create_framebuffer(g_ranal->ctx, &(srapi_framebuffer_desc_t){
                            .width = (uint32_t)new_w, .height = (uint32_t)new_h,
                        }, &fb) == SRAPI_OK) {
                        g_ranal->target->fb = fb;
                        g_ranal->target->owns_fb = 1;
                        g_ranal->target->pixels = srapi_framebuffer_pixels(fb);
                        g_ranal->target->width = new_w;
                        g_ranal->target->height = new_h;
                        g_ranal->target->pitch = (int32_t)srapi_framebuffer_pitch(fb);
                    }
                }
                g_ranal->width = new_w;
                g_ranal->height = new_h;
                if (g_ranal->root != NULL) {
                    g_ranal->root->width = new_w;
                    g_ranal->root->height = new_h;
                }
                g_ranal->dirty = 1;
                g_ranal->presented = 0;
                break;
            }
            case SPROT_EVENT_SURFACE_CLOSE:
                g_ranal->should_close = 1;
                break;
            case SPROT_EVENT_SURFACE_FRAME:
                g_ranal->sprot_pending_frame = 0;
                break;
            case SPROT_EVENT_DISCONNECT:
                g_ranal->should_close = 1;
                break;
            default:
                break;
        }
    }
}

void ranal_swm_present_(void) {
    if (g_ranal->sprot_surface == NULL || g_ranal->target == NULL) return;
    sprot_surface_t *surf = (sprot_surface_t *)g_ranal->sprot_surface;
    uint32_t *src = g_ranal->target->pixels;
    uint32_t *dst = sprot_surface_pixels(surf);
    if (src != NULL && dst != NULL) {
        int32_t src_pitch_px = g_ranal->target->pitch / 4;
        int32_t dst_pitch_px = (int32_t)(sprot_surface_stride(surf) / 4u);
        for (int32_t row = 0; row < g_ranal->height; row++) {
            memcpy(dst + row * dst_pitch_px,
                   src + row * src_pitch_px,
                   (size_t)g_ranal->width * 4u);
        }
        sprot_commit(surf);
        sprot_request_frame(surf);
    }
    
    for (ranal_widget_t *p = g_ranal->popups; p != NULL; p = p->next_sibling) {
        if (p->sprot_popup_surface == NULL) {
            sprot_surface_t *psurf = sprot_create_surface((sprot_connection_t *)g_ranal->sprot_conn, p->computed_w, p->computed_h);
            if (psurf != NULL) {
                sprot_set_role(psurf, SPROT_SURFACE_ROLE_POPUP, sprot_surface_id(surf), p->x, p->y);
                p->sprot_popup_surface = psurf;
            }
        }
        if (p->sprot_popup_surface != NULL) {
            if (p->popup_target == NULL || p->popup_target->width != p->computed_w || p->popup_target->height != p->computed_h) {
                if (p->popup_target != NULL) ranal_surface_destroy(p->popup_target);
                p->popup_target = ranal_surface_create(p->computed_w, p->computed_h);
            }
            if (p->popup_target != NULL) {
                srapi_cmd_reset(g_ranal->cmd);
                srapi_cmd_emit(g_ranal->cmd, &(srapi_command_t){
                    .kind = SRAPI_COMMAND_CLEAR,
                    .color = p->has_bg ? p->bg_color : ranal_get_theme()->bg,
                });
                int32_t old_x = p->computed_x, old_y = p->computed_y;
                p->computed_x = 0; p->computed_y = 0;
                ranal_render_pass_(p);
                p->computed_x = old_x; p->computed_y = old_y;
                srapi_submit(g_ranal->ctx, p->popup_target->fb, g_ranal->cmd);
                
                sprot_surface_t *psurf = (sprot_surface_t *)p->sprot_popup_surface;
                uint32_t *pdst = sprot_surface_pixels(psurf);
                uint32_t *psrc = p->popup_target->pixels;
                if (pdst != NULL && psrc != NULL) {
                    int32_t ppitch_src = p->popup_target->pitch / 4;
                    int32_t ppitch_dst = (int32_t)(sprot_surface_stride(psurf) / 4u);
                    for (int32_t row = 0; row < p->computed_h; row++) {
                        memcpy(pdst + row * ppitch_dst, psrc + row * ppitch_src, (size_t)p->computed_w * 4u);
                    }
                    sprot_commit(psurf);
                }
            }
        }
    }
    
    g_ranal->sprot_pending_frame = 1;
}
