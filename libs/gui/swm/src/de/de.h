#ifndef ONETOOL_LIBS_GUI_SWM_DE_H
#define ONETOOL_LIBS_GUI_SWM_DE_H

#include <swm/swm.h>

#include <stdint.h>

#define DE_TASKBAR_H        32
#define DE_LAUNCHER_W       72
#define DE_CLOCK_W          54
#define DE_ITEM_W_MAX       148
#define DE_ITEM_W_MIN       64
#define DE_ITEM_GAP         4
#define DE_TASK_PADDING     4
#define DE_MAX_ITEMS        12
#define DE_MENU_W           180
#define DE_MENU_ITEM_H      22
#define DE_MENU_ENTRIES     4
#define DE_TOOLTIP_FADE_SEC 0.18f
#define DE_TOOLTIP_GAP      8
#define DE_TOOLTIP_PAD_X    8
#define DE_TOOLTIP_PAD_Y    5

typedef struct de de_t;

typedef struct {
    uint32_t surface_id;
    int32_t  x, y, w, h;
    int      focused;
    int      minimized;
    int      hover;
} de_item_t;

de_t *de_create(swm_state_t *swm);
void  de_destroy(de_t *de);

int32_t de_taskbar_top(const de_t *de);
int32_t de_workspace_height(const de_t *de);

int de_point_in_panel(const de_t *de, int32_t mx, int32_t my);

void de_on_mouse_motion(de_t *de, int32_t mx, int32_t my);
int  de_on_mouse_button(de_t *de, int32_t mx, int32_t my, uint32_t button, uint32_t state);

void de_tick(de_t *de, uint64_t frame_count);

void de_render(de_t *de, struct srapi_framebuffer *fb);

#endif
