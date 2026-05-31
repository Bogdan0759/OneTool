#ifndef ONETOOL_LIBS_GUI_SWM_SURFACE_H
#define ONETOOL_LIBS_GUI_SWM_SURFACE_H

#include <swm/swm.h>

int swm_surface_role_is_child(uint32_t role);
int swm_surface_role_is_window(uint32_t role);

swm_surface_t *swm_surface_alloc(swm_state_t *swm);
void swm_surface_free(swm_state_t *swm, swm_surface_t *s);
swm_surface_t *swm_surface_find(swm_state_t *swm, uint32_t id);

int swm_surface_collect_z_asc(swm_state_t *swm, swm_surface_t **out, int max);
void swm_surface_raise(swm_state_t *swm, swm_surface_t *s);
void swm_surface_raise_tree(swm_state_t *swm, swm_surface_t *s);

void swm_surface_effective_rect(swm_state_t *swm, const swm_surface_t *s,
                                int32_t *ex, int32_t *ey, int32_t *ew, int32_t *eh);
void swm_surface_outer_rect(swm_state_t *swm, const swm_surface_t *s,
                            int32_t *ox, int32_t *oy, int32_t *ow, int32_t *oh);
void swm_surface_titlebar_button_rects(swm_state_t *swm, const swm_surface_t *s,
                                       int32_t *min_x, int32_t *max_x,
                                       int32_t *close_x, int32_t *btn_y);
void swm_surface_maximize_target(swm_state_t *swm, int32_t *tw, int32_t *th);
void swm_surface_local_coords(swm_state_t *swm, swm_surface_t *s,
                              int32_t mx, int32_t my, int32_t *lx, int32_t *ly);

swm_surface_t *swm_surface_topmost_window(swm_state_t *swm);
swm_surface_t *swm_surface_topmost_popup(swm_state_t *swm);

#endif
