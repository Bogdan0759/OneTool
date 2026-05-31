#include "interaction.h"

#include "../de/de.h"
#include "../protocol/protocol.h"
#include "../surface/surface.h"

typedef enum {
    SWM_HIT_NONE = 0,
    SWM_HIT_CONTENT,
    SWM_HIT_TITLEBAR,
    SWM_HIT_BTN_MIN,
    SWM_HIT_BTN_MAX,
    SWM_HIT_BTN_CLOSE,
} swm_hit_region_t;

static swm_hit_region_t hit_test(swm_state_t *swm, int32_t mx, int32_t my,
                                 swm_surface_t **out_surface) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_surface_collect_z_asc(swm, list, SWM_MAX_SURFACES);
    for (int i = n - 1; i >= 0; i--) {
        swm_surface_t *s = list[i];
        if (s->role == SPROT_SURFACE_ROLE_SUBSURFACE) continue;
        int32_t ox, oy, ow, oh;
        int32_t ex, ey, ew, eh;
        swm_surface_outer_rect(swm, s, &ox, &oy, &ow, &oh);
        swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
        if (mx < ox || mx >= ox + ow || my < oy || my >= oy + oh) continue;

        if (my >= ey && my < ey + eh && mx >= ex && mx < ex + ew) {
            *out_surface = s;
            return SWM_HIT_CONTENT;
        }
        if (swm_surface_role_is_child(s->role)) continue;

        int32_t bmin, bmax, bclose, by;
        swm_surface_titlebar_button_rects(swm, s, &bmin, &bmax, &bclose, &by);
        if (my >= by && my < by + SWM_BTN_SIZE) {
            if (mx >= bclose && mx < bclose + SWM_BTN_SIZE) { *out_surface = s; return SWM_HIT_BTN_CLOSE; }
            if (mx >= bmax   && mx < bmax   + SWM_BTN_SIZE) { *out_surface = s; return SWM_HIT_BTN_MAX; }
            if (mx >= bmin   && mx < bmin   + SWM_BTN_SIZE) { *out_surface = s; return SWM_HIT_BTN_MIN; }
        }
        *out_surface = s;
        return SWM_HIT_TITLEBAR;
    }
    *out_surface = NULL;
    return SWM_HIT_NONE;
}

static void clear_client_cursor(swm_state_t *swm) {
    swm_protocol_clear_client_cursor(swm);
}

static void send_close_to(swm_surface_t *s) {
    if (s == NULL || s->owner == NULL) return;
    swm_protocol_send_event(s->owner->sock, SPROT_EVT_SURFACE_CLOSE, s->id, 0, NULL, 0);
}

static void send_configure_to(swm_state_t *swm, swm_surface_t *s, uint32_t state_flags) {
    if (s == NULL || s->owner == NULL) return;
    int32_t tw, th;
    if (s->maximized) {
        swm_surface_maximize_target(swm, &tw, &th);
    } else if (s->saved_width > 0 && s->saved_height > 0) {
        tw = (int32_t)s->saved_width;
        th = (int32_t)s->saved_height;
    } else {
        tw = (int32_t)s->width;
        th = (int32_t)s->height;
    }
    if (tw <= 0) tw = (int32_t)s->width;
    if (th <= 0) th = (int32_t)s->height;
    sprot_body_configure_t body = {
        .width = (uint32_t)tw,
        .height = (uint32_t)th,
        .state = state_flags,
        .serial = ++swm->next_z,
    };
    swm_protocol_send_event(s->owner->sock, SPROT_EVT_SURFACE_CONFIGURE, s->id,
                            body.serial, &body, sizeof(body));
}

static void toggle_maximize(swm_state_t *swm, swm_surface_t *s) {
    if (!s->maximized) {
        s->saved_pos_x = s->pos_x;
        s->saved_pos_y = s->pos_y;
        s->saved_width = s->width;
        s->saved_height = s->height;
        s->maximized = 1;
    } else {
        s->maximized = 0;
        s->pos_x = s->saved_pos_x;
        s->pos_y = s->saved_pos_y;
    }
    send_configure_to(swm, s, (s->maximized ? SPROT_SURFACE_STATE_MAXIMIZED : 0) |
                             SPROT_SURFACE_STATE_FOCUSED);
}

void swm_interaction_forward_input(swm_state_t *swm, const srapi_input_event_t *ev) {
    swm_surface_t *s = NULL;
    swm_hit_region_t region;

    switch (ev->type) {
        case SRAPI_INPUT_EVENT_MOUSE_MOTION: {
            swm->mouse_x = ev->mouse_motion.x;
            swm->mouse_y = ev->mouse_motion.y;
            if (swm->interaction == SWM_INTERACT_MOVE && swm->grab_surface != NULL) {
                swm->grab_surface->pos_x = swm->mouse_x - swm->grab_offset_x;
                swm->grab_surface->pos_y = swm->mouse_y - swm->grab_offset_y;
                return;
            }
            if (swm->de != NULL && swm_surface_topmost_popup(swm) == NULL) {
                de_on_mouse_motion(swm->de, swm->mouse_x, swm->mouse_y);
                if (de_point_in_panel(swm->de, swm->mouse_x, swm->mouse_y)) {
                    if (swm->hovered_surface != NULL && swm->hovered_surface->owner != NULL) {
                        swm_protocol_send_event_nb(swm->hovered_surface->owner->sock,
                                                   SPROT_EVT_POINTER_LEAVE,
                                                   swm->hovered_surface->id, 0, NULL, 0);
                    }
                    swm->hovered_surface = NULL;
                    clear_client_cursor(swm);
                    return;
                }
            }
            region = hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
            swm_surface_t *new_hover = (region == SWM_HIT_CONTENT) ? s : NULL;
            if (swm->hovered_surface != new_hover) {
                if (swm->hovered_surface != NULL && swm->hovered_surface->owner != NULL) {
                    swm_protocol_send_event_nb(swm->hovered_surface->owner->sock,
                                               SPROT_EVT_POINTER_LEAVE,
                                               swm->hovered_surface->id, 0, NULL, 0);
                }
                clear_client_cursor(swm);
                swm->hovered_surface = new_hover;
                if (new_hover != NULL && new_hover->owner != NULL) {
                    sprot_body_pointer_motion_t body;
                    swm_surface_local_coords(swm, new_hover, swm->mouse_x, swm->mouse_y,
                                             &body.x, &body.y);
                    swm_protocol_send_event_nb(new_hover->owner->sock, SPROT_EVT_POINTER_ENTER,
                                               new_hover->id, 0, &body, sizeof(body));
                }
            }

            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                int32_t lx, ly;
                swm_surface_local_coords(swm, s, swm->mouse_x, swm->mouse_y, &lx, &ly);
                sprot_body_pointer_motion_t body = { .x = lx, .y = ly };
                swm_protocol_send_event_nb(s->owner->sock, SPROT_EVT_POINTER_MOTION,
                                           s->id, 0, &body, sizeof(body));
            } else {
                clear_client_cursor(swm);
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN: {
            swm->mouse_left_down = (ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT);
            swm_surface_t *active_popup = swm_surface_topmost_popup(swm);
            if (active_popup != NULL) {
                region = hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
                if (s != active_popup) {
                    send_close_to(active_popup);
                    return;
                }
            } else if (swm->de != NULL &&
                de_on_mouse_button(swm->de, swm->mouse_x, swm->mouse_y,
                                   ev->mouse_button.button, 1)) {
                return;
            }
            if (active_popup == NULL) {
                region = hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
            }
            if (s != NULL) swm_surface_raise_tree(swm, s);
            int alt_held = (swm->modifiers & SRAPI_KMOD_ALT) != 0;
            if (region == SWM_HIT_BTN_CLOSE) {
                send_close_to(s);
                return;
            }
            if (region == SWM_HIT_BTN_MIN) {
                s->minimized = 1;
                return;
            }
            if (region == SWM_HIT_BTN_MAX) {
                toggle_maximize(swm, s);
                return;
            }
            if ((region == SWM_HIT_TITLEBAR ||
                 (region == SWM_HIT_CONTENT && alt_held)) && s != NULL &&
                !swm_surface_role_is_child(s->role) &&
                ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT && !s->maximized) {
                swm->interaction = SWM_INTERACT_MOVE;
                swm->grab_surface = s;
                swm->grab_offset_x = swm->mouse_x - s->pos_x;
                swm->grab_offset_y = swm->mouse_y - s->pos_y;
                return;
            }
            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                sprot_body_pointer_button_t body = {
                    .button = ev->mouse_button.button,
                    .state = SPROT_BUTTON_STATE_PRESSED,
                };
                swm_protocol_send_event(s->owner->sock, SPROT_EVT_POINTER_BUTTON,
                                        s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_UP: {
            if (ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT) {
                swm->mouse_left_down = 0;
                if (swm->interaction == SWM_INTERACT_MOVE) {
                    swm->interaction = SWM_INTERACT_NONE;
                    swm->grab_surface = NULL;
                    return;
                }
            }
            swm_surface_t *active_popup = swm_surface_topmost_popup(swm);
            if (active_popup != NULL) {
                region = hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
                if (s != active_popup) return;
            } else if (swm->de != NULL &&
                de_on_mouse_button(swm->de, swm->mouse_x, swm->mouse_y,
                                   ev->mouse_button.button, 0)) {
                return;
            }
            if (active_popup == NULL) {
                region = hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
            }
            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                sprot_body_pointer_button_t body = {
                    .button = ev->mouse_button.button,
                    .state = SPROT_BUTTON_STATE_RELEASED,
                };
                swm_protocol_send_event(s->owner->sock, SPROT_EVT_POINTER_BUTTON,
                                        s->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_WHEEL: {
            if (swm->hovered_surface != NULL && swm->hovered_surface->owner != NULL) {
                sprot_body_pointer_axis_t body = {
                    .dx = ev->mouse_wheel.dx,
                    .dy = ev->mouse_wheel.dy,
                };
                swm_protocol_send_event_nb(swm->hovered_surface->owner->sock,
                                           SPROT_EVT_POINTER_AXIS,
                                           swm->hovered_surface->id, 0, &body, sizeof(body));
            }
            break;
        }
        case SRAPI_INPUT_EVENT_KEY_DOWN:
        case SRAPI_INPUT_EVENT_KEY_UP: {
            swm->modifiers = ev->key.modifiers;
            if (ev->type == SRAPI_INPUT_EVENT_KEY_DOWN &&
                ev->key.scancode == SRAPI_SCANCODE_ESCAPE &&
                (ev->key.modifiers & (SRAPI_KMOD_CTRL | SRAPI_KMOD_ALT)) != 0) {
                swm->should_quit = 1;
                break;
            }
            if (ev->type == SRAPI_INPUT_EVENT_KEY_DOWN && ev->key.scancode == SRAPI_SCANCODE_ESCAPE) {
                swm_surface_t *popup = swm_surface_topmost_popup(swm);
                if (popup != NULL) {
                    send_close_to(popup);
                    break;
                }
            }
            if (ev->key.scancode == SRAPI_SCANCODE_LGUI || ev->key.scancode == SRAPI_SCANCODE_RGUI) {
                if (ev->type == SRAPI_INPUT_EVENT_KEY_DOWN && swm->de != NULL) {
                    de_toggle_menu(swm->de);
                }
                break;
            }
            swm_surface_t *focused = swm_surface_topmost_window(swm);
            if (focused != NULL && focused->owner != NULL) {
                sprot_body_key_t body = {
                    .scancode = ev->key.scancode,
                    .state = ev->type == SRAPI_INPUT_EVENT_KEY_DOWN
                             ? SPROT_KEY_STATE_PRESSED : SPROT_KEY_STATE_RELEASED,
                    .modifiers = ev->key.modifiers,
                };
                swm_protocol_send_event(focused->owner->sock, SPROT_EVT_KEY,
                                        focused->id, 0, &body, sizeof(body));
            }
            break;
        }
        default:
            break;
    }
}
