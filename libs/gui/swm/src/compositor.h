#ifndef ONETOOL_LIBS_GUI_SWM_COMPOSITOR_H
#define ONETOOL_LIBS_GUI_SWM_COMPOSITOR_H

#include <swm/swm.h>
#include <srapi/srapi.h>

#include <stdint.h>

void swm_composite_surfaces(swm_state_t *swm, srapi_framebuffer_t *fb, uint32_t bg_color);

void swm_mark_dirty_rect(swm_state_t *swm, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void swm_mark_dirty_surface_outer(swm_state_t *swm, const swm_surface_t *s);
void swm_reset_dirty(swm_state_t *swm);

#endif
