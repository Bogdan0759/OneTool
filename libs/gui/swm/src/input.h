#ifndef ONETOOL_LIBS_GUI_SWM_INPUT_H
#define ONETOOL_LIBS_GUI_SWM_INPUT_H

#include <swm/swm.h>
#include <srapi/srapi.h>

void swm_forward_input(swm_state_t *swm, const srapi_input_event_t *ev);

#endif
