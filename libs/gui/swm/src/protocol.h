#ifndef ONETOOL_LIBS_GUI_SWM_PROTOCOL_H
#define ONETOOL_LIBS_GUI_SWM_PROTOCOL_H

#include <swm/swm.h>

int swm_setup_socket(swm_state_t *swm, const char *path);
int swm_dispatch_message(swm_state_t *swm, swm_client_t *c);
void swm_send_close_to(swm_state_t *swm, swm_surface_t *s);
void swm_send_configure_to(swm_state_t *swm, swm_surface_t *s, uint32_t state_flags);
void swm_toggle_maximize(swm_state_t *swm, swm_surface_t *s);
void swm_deliver_frame_callbacks(swm_state_t *swm);

#endif
