#ifndef ONETOOL_LIBS_TUI_H
#define ONETOOL_LIBS_TUI_H

enum tui_color {
    TUI_COLOR_DEFAULT = -1,
    TUI_COLOR_BLACK = 0,
    TUI_COLOR_RED = 1,
    TUI_COLOR_GREEN = 2,
    TUI_COLOR_YELLOW = 3,
    TUI_COLOR_BLUE = 4,
    TUI_COLOR_MAGENTA = 5,
    TUI_COLOR_CYAN = 6,
    TUI_COLOR_WHITE = 7,
};

enum tui_style {
    TUI_STYLE_NORMAL = 0,
    TUI_STYLE_PANEL = 1,
    TUI_STYLE_ACCENT = 2,
    TUI_STYLE_MUTED = 3,
    TUI_STYLE_SUCCESS = 4,
    TUI_STYLE_ERROR = 5,
    TUI_STYLE_SELECTION = 6,
};

enum tui_event_kind {
    TUI_EVENT_NONE = 0,
    TUI_EVENT_KEY = 1,
    TUI_EVENT_RESIZE = 2,
};

enum tui_key {
    TUI_KEY_NONE = 0,
    TUI_KEY_UP = 256,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_ENTER,
    TUI_KEY_ESCAPE,
    TUI_KEY_TAB,
    TUI_KEY_BACKSPACE,
    TUI_KEY_DELETE,
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PAGE_UP,
    TUI_KEY_PAGE_DOWN,
    TUI_KEY_RESIZE,
};

typedef struct {
    int background;
    int panel;
    int accent;
    int text;
    int muted;
    int success;
    int error;
} tui_palette_t;

typedef struct {
    int kind;
    int key;
} tui_event_t;

int tui_init(void);
void tui_shutdown(void);
void tui_begin_frame(void);
void tui_end_frame(void);
void tui_clear(int style);
void tui_get_size(int *width, int *height);
int tui_poll_event(tui_event_t *event, int timeout_ms);
void tui_set_palette(const tui_palette_t *palette);
void tui_draw_text(int x, int y, int style, const char *text);
void tui_draw_textf(int x, int y, int style, const char *fmt, ...);
void tui_fill_rect(int x, int y, int width, int height, int style, char fill);
void tui_draw_box(int x, int y, int width, int height, int style, const char *title);
void tui_draw_hline(int x, int y, int width, int style, char ch);
void tui_draw_vline(int x, int y, int height, int style, char ch);
void tui_draw_status_line(int style, const char *text);
int tui_key_is_printable(int key);
const char *tui_clip_text(const char *src, char *dst, int dst_size, int width);

#endif
