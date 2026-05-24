#ifndef ONETOOL_LIBS_GUI_SWM_WINDOW_H
#define ONETOOL_LIBS_GUI_SWM_WINDOW_H

#include <swm/swm.h>

#include <stdint.h>

swm_surface_t *swm_alloc_surface(swm_state_t *swm);
void swm_free_surface(swm_state_t *swm, swm_surface_t *s);
swm_surface_t *swm_find_surface(const swm_state_t *swm, uint32_t id);

swm_client_t *swm_alloc_client(swm_state_t *swm, int sock);
void swm_drop_client(swm_state_t *swm, swm_client_t *c, const char *reason);

void swm_surface_effective_rect(const swm_state_t *swm, const swm_surface_t *s,
                                int32_t *ex, int32_t *ey, int32_t *ew, int32_t *eh);
void swm_surface_outer_rect(const swm_state_t *swm, const swm_surface_t *s,
                            int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh);
void swm_titlebar_button_rects(const swm_state_t *swm, const swm_surface_t *s,
                               int32_t *min_x, int32_t *max_x, int32_t *close_x, int32_t *btn_y);

int swm_collect_surfaces_z_asc(const swm_state_t *swm, swm_surface_t **out, int max);
void swm_raise_surface(swm_state_t *swm, swm_surface_t *s);
swm_surface_t *swm_topmost_surface(const swm_state_t *swm);

typedef enum {
    SWM_HIT_NONE = 0,
    SWM_HIT_CONTENT,
    SWM_HIT_TITLEBAR,
    SWM_HIT_BTN_MIN,
    SWM_HIT_BTN_MAX,
    SWM_HIT_BTN_CLOSE,
} swm_hit_region_t;

swm_hit_region_t swm_hit_test(const swm_state_t *swm, int32_t mx, int32_t my,
                              swm_surface_t **out_surface);

#endif
