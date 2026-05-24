#define _GNU_SOURCE
#include "window.h"
#include "buffer/buffer.h"

#include "de/de.h"
#include <sprot/sprot.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void swm_logf(const char *fmt, ...);

swm_surface_t *swm_alloc_surface(swm_state_t *swm) {
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (!swm->surfaces[i].in_use) {
            memset(&swm->surfaces[i], 0, sizeof(swm->surfaces[i]));
            swm->surfaces[i].in_use = 1;
            return &swm->surfaces[i];
        }
    }
    return NULL;
}

void swm_free_surface(swm_state_t *swm, swm_surface_t *s) {
    if (s == NULL || !s->in_use) return;
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

swm_surface_t *swm_find_surface(const swm_state_t *swm, uint32_t id) {
    for (int i = 0; i < SWM_MAX_SURFACES; i++) {
        if (swm->surfaces[i].in_use && swm->surfaces[i].id == id) {
            return (swm_surface_t *)&swm->surfaces[i];
        }
    }
    return NULL;
}

swm_client_t *swm_alloc_client(swm_state_t *swm, int sock) {
    for (int i = 0; i < SWM_MAX_CLIENTS; i++) {
        if (!swm->clients[i].in_use) {
            memset(&swm->clients[i], 0, sizeof(swm->clients[i]));
            swm->clients[i].in_use = 1;
            swm->clients[i].sock = sock;
            return &swm->clients[i];
        }
    }
    return NULL;
}

void swm_drop_client(swm_state_t *swm, swm_client_t *c, const char *reason) {
    if (c == NULL || !c->in_use) return;
    swm_logf("client fd=%d dropped: %s", c->sock, reason);
    while (c->surface_count > 0) {
        swm_free_surface(swm, c->surfaces[0]);
    }
    if (c->sock >= 0) close(c->sock);
    memset(c, 0, sizeof(*c));
}

void swm_surface_effective_rect(const swm_state_t *swm, const swm_surface_t *s,
                                int32_t *ex, int32_t *ey, int32_t *ew, int32_t *eh) {
    if (s->role == SPROT_SURFACE_ROLE_POPUP) {
        swm_surface_t *parent = swm_find_surface(swm, s->parent_id);
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
    int32_t workspace_h = (int32_t)swm->display_h;
    if (swm->de != NULL) {
        int32_t top = de_workspace_height(swm->de);
        if (top > 0 && top < workspace_h) workspace_h = top;
    }
    if (s->maximized) {
        *ex = SWM_BORDER;
        *ey = SWM_TITLEBAR_H + SWM_BORDER;
        *ew = (int32_t)swm->display_w - 2 * SWM_BORDER;
        *eh = workspace_h - SWM_TITLEBAR_H - 2 * SWM_BORDER;
    } else {
        *ex = s->pos_x;
        *ey = s->pos_y;
        *ew = (int32_t)s->width;
        *eh = (int32_t)s->height;
    }
}

void swm_surface_outer_rect(const swm_state_t *swm, const swm_surface_t *s,
                            int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh) {
    int32_t ex, ey, ew, eh;
    swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
    if (s->role == SPROT_SURFACE_ROLE_POPUP) {
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

void swm_titlebar_button_rects(const swm_state_t *swm, const swm_surface_t *s,
                               int32_t *min_x, int32_t *max_x, int32_t *close_x,
                               int32_t *btn_y) {
    int32_t outer_x, outer_y, outer_w, outer_h;
    swm_surface_outer_rect(swm, s, &outer_x, &outer_y, &outer_w, &outer_h);
    int32_t bar_right = outer_x + outer_w - SWM_BORDER;
    *close_x = bar_right - 4 - SWM_BTN_SIZE;
    *max_x   = *close_x - 4 - SWM_BTN_SIZE;
    *min_x   = *max_x   - 4 - SWM_BTN_SIZE;
    *btn_y = outer_y + SWM_BORDER + (SWM_TITLEBAR_H - SWM_BTN_SIZE) / 2;
}

static int z_compare_asc(const void *a, const void *b) {
    const swm_surface_t * const *sa = a;
    const swm_surface_t * const *sb = b;
    if ((*sa)->z < (*sb)->z) return -1;
    if ((*sa)->z > (*sb)->z) return 1;
    return 0;
}

int swm_collect_surfaces_z_asc(const swm_state_t *swm, swm_surface_t **out, int max) {
    int n = 0;
    for (int i = 0; i < SWM_MAX_SURFACES && n < max; i++) {
        swm_surface_t *s = (swm_surface_t *)&swm->surfaces[i];
        if (!s->in_use || !s->committed || s->minimized) continue;
        out[n++] = s;
    }
    qsort(out, (size_t)n, sizeof(*out), z_compare_asc);
    return n;
}

void swm_raise_surface(swm_state_t *swm, swm_surface_t *s) {
    if (s == NULL) return;
    s->z = ++swm->next_z;
}

swm_surface_t *swm_topmost_surface(const swm_state_t *swm) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_collect_surfaces_z_asc(swm, list, SWM_MAX_SURFACES);
    return n > 0 ? list[n - 1] : NULL;
}

swm_hit_region_t swm_hit_test(const swm_state_t *swm, int32_t mx, int32_t my,
                              swm_surface_t **out_surface) {
    swm_surface_t *list[SWM_MAX_SURFACES];
    int n = swm_collect_surfaces_z_asc(swm, list, SWM_MAX_SURFACES);
    for (int i = n - 1; i >= 0; i--) {
        swm_surface_t *s = list[i];
        int32_t ox, oy, ow, oh;
        int32_t ex, ey, ew, eh;
        swm_surface_outer_rect(swm, s, &ox, &oy, &ow, &oh);
        swm_surface_effective_rect(swm, s, &ex, &ey, &ew, &eh);
        if (mx < ox || mx >= ox + ow || my < oy || my >= oy + oh) continue;

        if (my >= ey && my < ey + eh && mx >= ex && mx < ex + ew) {
            *out_surface = s;
            return SWM_HIT_CONTENT;
        }
        if (s->role == SPROT_SURFACE_ROLE_POPUP) {
            continue;
        }
        int32_t bmin, bmax, bclose, by;
        swm_titlebar_button_rects(swm, s, &bmin, &bmax, &bclose, &by);
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
