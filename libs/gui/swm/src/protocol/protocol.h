#ifndef ONETOOL_LIBS_GUI_SWM_PROTOCOL_H
#define ONETOOL_LIBS_GUI_SWM_PROTOCOL_H

#include <swm/swm.h>

int swm_protocol_send_event(int fd, uint16_t type, uint32_t object_id,
                            uint32_t serial, const void *body, size_t body_len);
int swm_protocol_send_event_nb(int fd, uint16_t type, uint32_t object_id,
                               uint32_t serial, const void *body, size_t body_len);

void swm_protocol_clear_client_cursor(swm_state_t *swm);
void swm_protocol_drop_client(swm_state_t *swm, swm_client_t *c, const char *reason);
swm_client_t *swm_protocol_alloc_client(swm_state_t *swm, int sock);
int swm_protocol_setup_socket(swm_state_t *swm, const char *path);
int swm_protocol_dispatch(swm_state_t *swm, swm_client_t *c);

#endif
