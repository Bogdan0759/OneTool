#include "tui.h"

#include <curses.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

enum tui_pair_id {
    TUI_PAIR_NORMAL = 1,
    TUI_PAIR_PANEL = 2,
    TUI_PAIR_ACCENT = 3,
    TUI_PAIR_MUTED = 4,
    TUI_PAIR_SUCCESS = 5,
    TUI_PAIR_ERROR = 6,
    TUI_PAIR_SELECTION = 7,
};

static int tui_ready = 0;
static tui_palette_t tui_palette = {
    TUI_COLOR_BLACK,
    TUI_COLOR_BLUE,
    TUI_COLOR_CYAN,
    TUI_COLOR_WHITE,
    TUI_COLOR_CYAN,
    TUI_COLOR_GREEN,
    TUI_COLOR_RED,
};

static int style_to_pair(int style) {
    switch (style) {
        case TUI_STYLE_PANEL:
            return TUI_PAIR_PANEL;
        case TUI_STYLE_ACCENT:
            return TUI_PAIR_ACCENT;
        case TUI_STYLE_MUTED:
            return TUI_PAIR_MUTED;
        case TUI_STYLE_SUCCESS:
            return TUI_PAIR_SUCCESS;
        case TUI_STYLE_ERROR:
            return TUI_PAIR_ERROR;
        case TUI_STYLE_SELECTION:
            return TUI_PAIR_SELECTION;
        case TUI_STYLE_NORMAL:
        default:
            return TUI_PAIR_NORMAL;
    }
}

static void apply_palette(void) {
    int selection_fg = tui_palette.background;

    if (selection_fg == TUI_COLOR_DEFAULT) {
        selection_fg = TUI_COLOR_BLACK;
    }

    init_pair(TUI_PAIR_NORMAL, tui_palette.text, tui_palette.background);
    init_pair(TUI_PAIR_PANEL, tui_palette.text, tui_palette.panel);
    init_pair(TUI_PAIR_ACCENT, tui_palette.accent, tui_palette.background);
    init_pair(TUI_PAIR_MUTED, tui_palette.muted, tui_palette.background);
    init_pair(TUI_PAIR_SUCCESS, tui_palette.success, tui_palette.background);
    init_pair(TUI_PAIR_ERROR, tui_palette.error, tui_palette.background);
    init_pair(TUI_PAIR_SELECTION, selection_fg, tui_palette.accent);
}

static attr_t style_attr(int style) {
    attr_t attr = COLOR_PAIR(style_to_pair(style));

    if (style == TUI_STYLE_ACCENT || style == TUI_STYLE_SELECTION) {
        attr |= A_BOLD;
    }
    return attr;
}

static int translate_key(int raw_key) {
    switch (raw_key) {
        case KEY_UP:
            return TUI_KEY_UP;
        case KEY_DOWN:
            return TUI_KEY_DOWN;
        case KEY_LEFT:
            return TUI_KEY_LEFT;
        case KEY_RIGHT:
            return TUI_KEY_RIGHT;
        case KEY_HOME:
            return TUI_KEY_HOME;
        case KEY_END:
            return TUI_KEY_END;
        case KEY_PPAGE:
            return TUI_KEY_PAGE_UP;
        case KEY_NPAGE:
            return TUI_KEY_PAGE_DOWN;
        case KEY_DC:
            return TUI_KEY_DELETE;
        case KEY_BACKSPACE:
        case 127:
        case 8:
            return TUI_KEY_BACKSPACE;
        case '\n':
        case '\r':
        case KEY_ENTER:
            return TUI_KEY_ENTER;
        case '\t':
            return TUI_KEY_TAB;
        case 27:
            return TUI_KEY_ESCAPE;
        case KEY_RESIZE:
            return TUI_KEY_RESIZE;
        default:
            return raw_key;
    }
}

static void clip_rect_to_screen(int *x, int *y, int *width, int *height) {
    int max_y;
    int max_x;

    if (!tui_ready) {
        return;
    }

    getmaxyx(stdscr, max_y, max_x);

    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*x >= max_x || *y >= max_y) {
        *width = 0;
        *height = 0;
        return;
    }
    if (*x + *width > max_x) {
        *width = max_x - *x;
    }
    if (*y + *height > max_y) {
        *height = max_y - *y;
    }
}

int tui_init(void) {
    if (tui_ready) {
        return 0;
    }

    if (newterm(NULL, stdout, stdin) == NULL) {
        return 1;
    }

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
#ifdef NCURSES_VERSION
    set_escdelay(25);
#endif

    if (has_colors()) {
        start_color();
#ifdef NCURSES_VERSION
        use_default_colors();
#endif
        apply_palette();
    }

    tui_ready = 1;
    return 0;
}

void tui_shutdown(void) {
    if (!tui_ready) {
        return;
    }   

    endwin();
    tui_ready = 0;
}

void tui_begin_frame(void) {
    if (!tui_ready) {
        return;
    }

    erase();
}

void tui_end_frame(void) {
    if (!tui_ready) {
        return;
    }

    refresh();
}

void tui_clear(int style) {
    if (!tui_ready) {
        return;
    }

    bkgdset(' ' | style_attr(style));
    erase();
}

void tui_get_size(int *width, int *height) {
    int max_y = 0;
    int max_x = 0;

    if (!tui_ready) {
        if (width != NULL) {
            *width = 0;
        }
        if (height != NULL) {
            *height = 0;
        }
        return;
    }

    getmaxyx(stdscr, max_y, max_x);
    if (width != NULL) {
        *width = max_x;
    }
    if (height != NULL) {
        *height = max_y;
    }
}

int tui_poll_event(tui_event_t *event, int timeout_ms) {
    int raw_key;

    if (event == NULL || !tui_ready) {
        return 0;
    }

    event->kind = TUI_EVENT_NONE;
    event->key = TUI_KEY_NONE;

    timeout(timeout_ms);
    raw_key = getch();
    if (raw_key == ERR) {
        return 0;
    }

    event->key = translate_key(raw_key);
    if (event->key == TUI_KEY_RESIZE) {
        event->kind = TUI_EVENT_RESIZE;
    } else {
        event->kind = TUI_EVENT_KEY;
    }
    return 1;
}

void tui_set_palette(const tui_palette_t *palette) {
    if (palette == NULL) {
        return;
    }

    tui_palette = *palette;
    if (tui_ready && has_colors()) {
        apply_palette();
    }
}

void tui_draw_text(int x, int y, int style, const char *text) {
    int max_y;
    int max_x;
    int width;
    int text_len;

    if (!tui_ready || text == NULL) {
        return;
    }

    getmaxyx(stdscr, max_y, max_x);
    if (y < 0 || y >= max_y || x >= max_x) {
        return;
    }

    if (x < 0) {
        text_len = (int)strlen(text);
        if (-x >= text_len) {
            return;
        }
        text += -x;
        x = 0;
    }

    width = max_x - x;
    attrset(style_attr(style));
    mvaddnstr(y, x, text, width);
}

void tui_draw_textf(int x, int y, int style, const char *fmt, ...) {
    va_list ap;
    char buf[1024];

    if (fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_draw_text(x, y, style, buf);
}

void tui_fill_rect(int x, int y, int width, int height, int style, char fill) {
    int row;
    int col;

    if (!tui_ready) {
        return;
    }

    clip_rect_to_screen(&x, &y, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    attrset(style_attr(style));
    for (row = 0; row < height; row++) {
        for (col = 0; col < width; col++) {
            mvaddch(y + row, x + col, fill);
        }
    }
}

void tui_draw_hline(int x, int y, int width, int style, char ch) {
    int i;

    if (!tui_ready || width <= 0) {
        return;
    }

    attrset(style_attr(style));
    for (i = 0; i < width; i++) {
        mvaddch(y, x + i, ch);
    }
}

void tui_draw_vline(int x, int y, int height, int style, char ch) {
    int i;

    if (!tui_ready || height <= 0) {
        return;
    }

    attrset(style_attr(style));
    for (i = 0; i < height; i++) {
        mvaddch(y + i, x, ch);
    }
}

void tui_draw_box(int x, int y, int width, int height, int style, const char *title) {
    int title_width;
    int max_title_width;

    if (!tui_ready || width < 2 || height < 2) {
        return;
    }

    attrset(style_attr(style));
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + width - 1, ACS_URCORNER);
    mvaddch(y + height - 1, x, ACS_LLCORNER);
    mvaddch(y + height - 1, x + width - 1, ACS_LRCORNER);
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvvline(y + 1, x, ACS_VLINE, height - 2);
    mvvline(y + 1, x + width - 1, ACS_VLINE, height - 2);

    if (title != NULL && title[0] != '\0' && width > 4) {
        max_title_width = width - 4;
        title_width = (int)strlen(title);
        if (title_width > max_title_width) {
            title_width = max_title_width;
        }
        mvaddch(y, x + 2, ACS_RTEE);
        mvaddnstr(y, x + 3, title, title_width);
        mvaddch(y, x + 3 + title_width, ACS_LTEE);
    }
}

void tui_draw_status_line(int style, const char *text) {
    int width = 0;
    int height = 0;
    char clipped[1024];
    const char *rendered = text;

    if (!tui_ready) {
        return;
    }

    tui_get_size(&width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }

    tui_fill_rect(0, height - 1, width, 1, style, ' ');
    if (text != NULL) {
        rendered = tui_clip_text(text, clipped, sizeof(clipped), width - 1);
        tui_draw_text(0, height - 1, style, rendered);
    }
}

int tui_key_is_printable(int key) {
    return key >= 32 && key <= 126;
}

const char *tui_clip_text(const char *src, char *dst, int dst_size, int width) {
    int src_len;

    if (src == NULL) {
        return "";
    }
    if (dst == NULL || dst_size <= 0) {
        return src;
    }
    if (width < 0) {
        width = 0;
    }

    src_len = (int)strlen(src);
    if (src_len <= width && src_len < dst_size) {
        memcpy(dst, src, (size_t)src_len + 1);
        return dst;
    }

    if (width <= 0) {
        dst[0] = '\0';
        return dst;
    }
    if (width == 1) {
        dst[0] = '.';
        dst[1] = '\0';
        return dst;
    }
    if (width == 2) {
        dst[0] = '.';
        dst[1] = '.';
        dst[2] = '\0';
        return dst;
    }

    if (width >= dst_size) {
        width = dst_size - 1;
    }
    if (width < 3) {
        width = 3;
    }

    memcpy(dst, src, (size_t)(width - 3));
    dst[width - 3] = '.';
    dst[width - 2] = '.';
    dst[width - 1] = '.';
    dst[width] = '\0';
    return dst;
}
