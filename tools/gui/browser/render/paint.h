#ifndef ONETOOL_TOOLS_GUI_BROWSER_RENDER_PAINT_H
#define ONETOOL_TOOLS_GUI_BROWSER_RENDER_PAINT_H

#include "../browser.h"
#include "layout.h"

/* Paint the laid-out boxes into a freshly-allocated ranal surface
 * (caller passes a ranal_surface_t *). scroll_y subtracts that many pixels
 * from each box y; offset_y shifts everything down (used to leave room
 * for the chrome bar above the page). The link_hits structure is reset
 * and refilled with screen-space rectangles for visible link clusters. */
void br_paint_page(void *surface,
                   const br_layout_t *layout,
                   int scroll_y,
                   int viewport_h,
                   int focused_link,
                   br_link_hits_t *hits);

#endif
