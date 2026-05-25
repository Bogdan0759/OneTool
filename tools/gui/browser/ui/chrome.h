#ifndef ONETOOL_TOOLS_GUI_BROWSER_UI_CHROME_H
#define ONETOOL_TOOLS_GUI_BROWSER_UI_CHROME_H

#include "../browser.h"

/* Draw the URL bar + status line into the given ranal surface.
 *  surface_v - ranal_surface_t *
 *  app       - app state (URL/status/edit buffer)
 *  width     - viewport width in px
 *  loading   - 1 while a fetch is in-flight (changes spinner glyph)
 */
void br_chrome_paint(void *surface_v, const browser_app_t *app,
                     int width, int loading);

#endif
