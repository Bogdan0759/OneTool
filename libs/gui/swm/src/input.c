#define _GNU_SOURCE
#include "input.h"
#include "window.h"
#include "protocol.h"
#include "de/de.h"
#include "compositor.h"

#include <sprot/sprot.h>

#include <stdint.h>

void swm_forward_input(swm_state_t *swm, const srapi_input_event_t *ev) {
    swm_surface_t *s = NULL;
    swm_hit_region_t region;

    switch (ev->type) {
        case SRAPI_INPUT_EVENT_MOUSE_MOTION: {
            swm->mouse_x = ev->mouse_motion.x;
            swm->mouse_y = ev->mouse_motion.y;
            if (swm->interaction == SWM_INTERACT_MOVE && swm->grab_surface != NULL) {
                int32_t old_x = swm->grab_surface->pos_x;
                int32_t old_y = swm->grab_surface->pos_y;
                swm->grab_surface->pos_x = swm->mouse_x - swm->grab_offset_x;
                swm->grab_surface->pos_y = swm->mouse_y - swm->grab_offset_y;
                if (old_x != swm->grab_surface->pos_x || old_y != swm->grab_surface->pos_y) {
                    swm_mark_dirty_surface_outer(swm, swm->grab_surface);
                }
                return;
            }
            if (swm->de != NULL) {
                de_on_mouse_motion(swm->de, swm->mouse_x, swm->mouse_y);
                if (de_point_in_panel(swm->de, swm->mouse_x, swm->mouse_y)) {
                    return;
                }
            }
            region = swm_hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
            swm_surface_t *new_hover = (region == SWM_HIT_CONTENT) ? s : NULL;
            if (swm->hovered_surface != new_hover) {
                if (swm->hovered_surface != NULL && swm->hovered_surface->owner != NULL) {
                    sprot_header_t hdr = { .type = SPROT_EVT_POINTER_LEAVE, .object_id = swm->hovered_surface->id };
                    sprot_send_message(swm->hovered_surface->owner->sock, &hdr, NULL, 0, -1);
                }
                swm->hovered_surface = new_hover;
                if (new_hover != NULL && new_hover->owner != NULL) {
                    sprot_header_t hdr = { .type = SPROT_EVT_POINTER_ENTER, .object_id = new_hover->id };
                    sprot_send_message(new_hover->owner->sock, &hdr, NULL, 0, -1);
                }
            }

            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                int32_t ex, ey, ew, eh;
                swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
                int32_t lx = (int32_t)(((int64_t)(swm->mouse_x - ex) * (int64_t)s->width) / (ew > 0 ? ew : 1));
                int32_t ly = (int32_t)(((int64_t)(swm->mouse_y - ey) * (int64_t)s->height) / (eh > 0 ? eh : 1));
                sprot_body_pointer_motion_t body = { .x = lx, .y = ly };
                sprot_header_t hdr = { .type = SPROT_EVT_POINTER_MOTION, .object_id = s->id };
                sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_BUTTON_DOWN: {
            swm->mouse_left_down = (ev->mouse_button.button == SRAPI_MOUSE_BUTTON_LEFT);
            if (swm->de != NULL &&
                de_on_mouse_button(swm->de, swm->mouse_x, swm->mouse_y,
                                   ev->mouse_button.button, 1)) {
                return;
            }
            region = swm_hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
            if (s != NULL) swm_raise_surface(swm, s);
            int alt_held = (swm->modifiers & SRAPI_KMOD_ALT) != 0;
            if (region == SWM_HIT_BTN_CLOSE) {
                swm_send_close_to(swm, s);
                return;
            }
            if (region == SWM_HIT_BTN_MIN) {
                swm_mark_dirty_surface_outer(swm, s);
                s->minimized = 1;
                return;
            }
            if (region == SWM_HIT_BTN_MAX) {
                swm_toggle_maximize(swm, s);
                return;
            }
            if ((region == SWM_HIT_TITLEBAR ||
                 (region == SWM_HIT_CONTENT && alt_held)) && s != NULL &&
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
                sprot_header_t hdr = { .type = SPROT_EVT_POINTER_BUTTON, .object_id = s->id };
                sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
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
            if (swm->de != NULL &&
                de_on_mouse_button(swm->de, swm->mouse_x, swm->mouse_y,
                                   ev->mouse_button.button, 0)) {
                return;
            }
            region = swm_hit_test(swm, swm->mouse_x, swm->mouse_y, &s);
            if (region == SWM_HIT_CONTENT && s != NULL && s->owner != NULL) {
                sprot_body_pointer_button_t body = {
                    .button = ev->mouse_button.button,
                    .state = SPROT_BUTTON_STATE_RELEASED,
                };
                sprot_header_t hdr = { .type = SPROT_EVT_POINTER_BUTTON, .object_id = s->id };
                sprot_send_message(s->owner->sock, &hdr, &body, sizeof(body), -1);
            }
            break;
        }
        case SRAPI_INPUT_EVENT_MOUSE_WHEEL: {
            if (swm->hovered_surface != NULL && swm->hovered_surface->owner != NULL) {
                sprot_body_pointer_axis_t body = {
                    .dx = ev->mouse_wheel.dx,
                    .dy = ev->mouse_wheel.dy,
                };
                sprot_header_t hdr = { .type = SPROT_EVT_POINTER_AXIS, .object_id = swm->hovered_surface->id };
                sprot_send_message(swm->hovered_surface->owner->sock, &hdr, &body, sizeof(body), -1);
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
            if (ev->key.scancode == SRAPI_SCANCODE_LGUI || ev->key.scancode == SRAPI_SCANCODE_RGUI) {
                if (ev->type == SRAPI_INPUT_EVENT_KEY_DOWN) {
                    if (swm->de != NULL) {
                        de_toggle_menu(swm->de);
                    }
                }
                break;
            }
            swm_surface_t *focused = swm_topmost_surface(swm);
            if (focused != NULL && focused->owner != NULL) {
                sprot_body_key_t body = {
                    .scancode = ev->key.scancode,
                    .state = ev->type == SRAPI_INPUT_EVENT_KEY_DOWN
                             ? SPROT_KEY_STATE_PRESSED : SPROT_KEY_STATE_RELEASED,
                    .modifiers = ev->key.modifiers,
                };
                sprot_header_t hdr = { .type = SPROT_EVT_KEY, .object_id = focused->id };
                sprot_send_message(focused->owner->sock, &hdr, &body, sizeof(body), -1);
            }
            break;
        }
        default:
            break;
    }
}
