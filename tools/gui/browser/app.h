#ifndef ONETOOL_TOOLS_GUI_BROWSER_APP_H
#define ONETOOL_TOOLS_GUI_BROWSER_APP_H

#include "browser.h"

/* Initialize the app struct (allocate doc, surfaces will be created on
 * first frame after we know the window size). */
int  br_app_init(browser_app_t *app, const char *initial_url);
void br_app_shutdown(browser_app_t *app);

/* Drive one frame: handle pending navigation, lay out if dirty, draw
 * page + chrome into our two surfaces. Returns 0 on success. */
int  br_app_frame(browser_app_t *app);

/* Trigger a navigation to app->url. Returns 0 on success, non-zero on
 * fetch/parse failure (status string set accordingly). */
int  br_app_navigate(browser_app_t *app, const char *url);

/* Resolve the link with the given index (against the current base URL)
 * and schedule navigation. No-op on out-of-range index or empty href. */
void br_app_follow_link(browser_app_t *app, int link_index);

#endif
