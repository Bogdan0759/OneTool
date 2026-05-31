#include "surface.h"

#include "../buffer/buffer.h"
#include "../de/de.h"

#include <stdlib.h>
#include <string.h>

int swm_surface_role_is_child(uint32_t role) {
    return role == SPROT_SURFACE_ROLE_POPUP || role == SPROT_SURFACE_ROLE_SUBSURFACE;
}

int swm_surface_role_is_window(uint32_t role) {
    return role == SPROT_SURFACE_ROLE_TOPLEVEL;
}

swm_surface_t *swm_surface_alloc(swm_state_t *swm) {
    if (swm == NULL) return NULL;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (!swm->surfaces[i].in_use) {
            memset(&swm->surfaces[i], 0, sizeof(swm->surfaces[i]));
            swm->surfaces[i].in_use = 1;
            return &swm->surfaces[i];
        }
    }
    return NULL;
}

void swm_surface_free(swm_state_t *swm, swm_surface_t *s) {
    if (swm == NULL || s == NULL || !s->in_use) return;

    uint32_t surface_id = s->id;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *child = &swm->surfaces[i];
        if (child->in_use && swm_surface_role_is_child(child->role) && child->parent_id == surface_id) {
            swm_surface_free(swm, child);
        }
    }

    if (swm->grab_surface == s) {
        swm->grab_surface = NULL;
        swm->interaction = SWM_INTERACT_NONE;
    }
    if (swm->hovered_surface == s) {
        swm->hovered_surface = NULL;
    }

    swm_buffer_destroy(s->buffer);
    if (s->owner != NULL) {
        for (int i = 0; i < s->owner->surface_count; i++) {
            if (s->owner->surfaces[i] == s) {
                s->owner->surfaces[i] = s->owner->surfaces[s->owner->surface_count - 1];
                s->owner->surface_count--;
                break;
            }
        }
    }
    memset(s, 0, sizeof(*s));
}

swm_surface_t *swm_surface_find(swm_state_t *swm, uint32_t id) {
    if (swm == NULL) return NULL;
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (swm->surfaces[i].in_use && swm->surfaces[i].id == id) {
            return &swm->surfaces[i];
        }
    }
    return NULL;
}

static int z_compare_asc(const void *a, const void *b) {
    const swm_surface_t * const *sa = a;
    const swm_surface_t * const *sb = b;
    if ((*sa)->z < (*sb)->z) return -1;
    if ((*sa)->z > (*sb)->z) return 1;
    return 0;
}

static int surface_tree_is_visible(swm_state_t *swm, swm_surface_t *s, int depth) {
    if (s == NULL || !s->in_use || !s->committed || s->minimized) return 0;
    if (!swm_surface_role_is_child(s->role)) return 1;
    if (depth >= SWM_MAX_SURFACES) return 0;
    return surface_tree_is_visible(swm, swm_surface_find(swm, s->parent_id), depth + 1);
}

int swm_surface_collect_z_asc(swm_state_t *swm, swm_surface_t **out, int max) {
    int n = 0;
    if (swm == NULL || out == NULL || max <= 0) return 0;
    for (int i = 0; i < SWM_MAX_SURFACES && n < max; i++) {
        swm_surface_t *s = &swm->surfaces[i];
        if (!surface_tree_is_visible(swm, s, 0)) continue;
        out[n++] = s;
    }
    qsort(out, (size_t)n, sizeof(*out), z_compare_asc);
    return n;
}

void swm_surface_raise(swm_state_t *swm, swm_surface_t *s) {
    if (swm == NULL || s == NULL) return;
    s->z = ++swm->next_z;
}

void swm_surface_raise_tree(swm_state_t *swm, swm_surface_t *s) {
    if (swm == NULL || s == NULL) return;
    swm_surface_raise(swm, s);
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        swm_surface_t *child = &swm->surfaces[i];
        if (child->in_use && swm_surface_role_is_child(child->role) && child->parent_id == s->id) {
            swm_surface_raise_tree(swm, child);
        }
    }
}

void swm_surface_effective_rect(swm_state_t *swm, const swm_surface_t *s,
                                int32_t *ex, int32_t *ey, int32_t *ew, int32_t *eh) {
    if (swm_surface_role_is_child(s->role)) {
        swm_surface_t *parent = swm_surface_find(swm, s->parent_id);
        if (parent != NULL) {
            int32_t pex, pey, pew, peh;
            swm_surface_effective_rect(swm, parent, &pex, &pey, &pew, &peh);
            *ex = pex + s->rel_x;
            *ey = pey + s->rel_y;
        } else {
            *ex = s->pos_x;
            *ey = s->pos_y;
        }
        *ew = (int32_t)s->width;
        *eh = (int32_t)s->height;
        return;
    }

    if (s->maximized) {
        *ex = SWM_BORDER;
        *ey = SWM_TITLEBAR_H + SWM_BORDER;
    } else {
        *ex = s->pos_x;
        *ey = s->pos_y;
    }
    *ew = (int32_t)s->width;
    *eh = (int32_t)s->height;
}

void swm_surface_maximize_target(swm_state_t *swm, int32_t *tw, int32_t *th) {
    int32_t workspace_h = (int32_t)swm->display_h;
    if (swm->de != NULL) {
        int32_t top = de_workspace_height(swm->de);
        if (top > 0 && top < workspace_h) workspace_h = top;
    }
    int32_t w = (int32_t)swm->display_w - 2 * SWM_BORDER;
    int32_t h = workspace_h - SWM_TITLEBAR_H - 2 * SWM_BORDER;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    *tw = w;
    *th = h;
}

void swm_surface_outer_rect(swm_state_t *swm, const swm_surface_t *s,
                            int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh) {
    int32_t ex, ey, ew, eh;
    swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
    if (swm_surface_role_is_child(s->role)) {
        *ox = ex;
        *oy = ey;
        *ow = ew;
        *oh = eh;
        return;
    }
    *ox = ex - SWM_BORDER;
    *oy = ey - SWM_TITLEBAR_H - SWM_BORDER;
    *ow = ew + 2 * SWM_BORDER;
    *oh = eh + SWM_TITLEBAR_H + 2 * SWM_BORDER;
}

void swm_surface_titlebar_button_rects(swm_state_t *swm, const swm_surface_t *s,
                                       int32_t *min_x, int32_t *max_x,
                                       int32_t *close_x, int32_t *btn_y) {
    int32_t outer_x, outer_y, outer_w, outer_h;
    swm_surface_outer_rect(swm, s, &outer_x, &outer_y, &outer_w, &outer_h);
    int32_t bar_right = outer_x + outer_w - SWM_BORDER;
    *close_x = bar_right - 4 - SWM_BTN_SIZE;
    *max_x   = *close_x - 4 - SWM_BTN_SIZE;
    *min_x   = *max_x   - 4 - SWM_BTN_SIZE;
    *btn_y = outer_y + SWM_BORDER + (SWM_TITLEBAR_H - SWM_BTN_SIZE) / 2;
}

void swm_surface_local_coords(swm_state_t *swm, swm_surface_t *s,
                              int32_t mx, int32_t my, int32_t *lx, int32_t *ly) {
    int32_t ex, ey, ew, eh;
    swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
    *lx = (int32_t)(((int64_t)(mx - ex) * (int64_t)s->width) / (ew > 0 ? ew : 1));
    *ly = (int32_t)(((int64_t)(my - ey) * (int64_t)s->height) / (eh > 0 ? eh : 1));
}

swm_surface_t *swm_surface_topmost_window(swm_state_t *swm) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_surface_collect_z_asc(swm, list, SWM_MAX_SURFACES);
    for (int i = n - 1; i >= 0; i--) {
        if (swm_surface_role_is_window(list[i]->role)) return list[i];
    }
    return NULL;
}

swm_surface_t *swm_surface_topmost_popup(swm_state_t *swm) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_surface_collect_z_asc(swm, list, SWM_MAX_SURFACES);
    for (int i = n - 1; i >= 0; i--) {
        if (list[i]->role == SPROT_SURFACE_ROLE_POPUP) return list[i];
    }
    return NULL;
}
