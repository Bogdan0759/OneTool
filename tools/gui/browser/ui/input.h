#ifndef ONETOOL_TOOLS_GUI_BROWSER_UI_INPUT_H
#define ONETOOL_TOOLS_GUI_BROWSER_UI_INPUT_H

#include "../browser.h"
#include <stdint.h>

/* Keyboard / scrollwheel handler results — used by main loop to decide
 * what to do after a key event. */
typedef enum {
    BR_ACTION_NONE = 0,
    BR_ACTION_QUIT,
    BR_ACTION_NAVIGATE,        /* user submitted the URL bar */
    BR_ACTION_FOLLOW_LINK,     /* user pressed Enter on a focused link */
    BR_ACTION_BACK,            /* alt+left */
    BR_ACTION_RELOAD,          /* F5 / ctrl+R */
} br_input_action_t;

void br_input_install(browser_app_t *app);

#endif
