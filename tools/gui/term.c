/*
 * term - GUI terminal emulator for OneTool.
 *
 * Spawns a real shell on a pseudo-terminal (posix_openpt + ptsname). All
 * shell output is parsed through a small ANSI/VT100 subset (CSI cursor +
 * SGR color + ED/EL erase) into a fixed cell grid that is redrawn each
 * frame using ranal's primitive draw calls. Keyboard input goes through
 * ranal's key hook and is translated into VT100 byte sequences that get
 * written back to the PTY master.
 *
 * Designed to run either standalone (ranal_init -> drm/fbdev) or as a
 * swm/sprot client (ranal_init_swm). All native shell features (command
 * history, line editing, tab-completion, job control) come from the
 * spawned shell itself, not from us.
 */

#define _GNU_SOURCE
#include <ranal/ranal.h>
#include <srapi/srapi.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <utmp.h>

/* Match ranal_font_glyph metrics. */
#define CELL_W       6
#define CELL_H       8
#define PAD_X        6
#define PAD_Y        6

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define MAX_COLS     256
#define MAX_ROWS     128
#define SCROLLBACK   2000

#define ATTR_BOLD    0x01
#define ATTR_REVERSE 0x02
#define ATTR_UNDER   0x04
#define ATTR_DIM     0x08

typedef struct {
    char    ch;
    uint8_t fg;       /* palette index 0..15 */
    uint8_t bg;       /* palette index 0..15 */
    uint8_t attrs;
} cell_t;

/* CSI parser state. */
typedef enum {
    P_NORMAL = 0,
    P_ESC,
    P_CSI,
    P_OSC,
} parser_state_t;

typedef struct {
    int cols, rows;
    cell_t  *grid;          /* visible: rows * cols */
    cell_t  *scroll;        /* scrollback: SCROLLBACK * cols */
    int      scroll_used;
    int      view_offset;   /* lines above bottom currently shown */
    int      cx, cy;        /* cursor in visible grid */
    int      saved_cx, saved_cy;
    int      scroll_top, scroll_bot;
    uint8_t  cur_fg, cur_bg, cur_attrs;
    int      cursor_visible;
    int      app_keypad;
    int      alt_screen;
    cell_t  *primary_grid;
    cell_t  *alt_grid;

    parser_state_t pstate;
    int      csi_params[16];
    int      csi_nparam;
    int      csi_priv;
    char     osc_buf[256];
    int      osc_len;
    int      osc_kind;

    /* Our own pixel buffer. ranal's draw commands are wiped by
       srapi_cmd_reset inside ranal_render, so we paint into this surface
       ourselves and ranal_blit_surface it onto the backbuffer after the
       render pass — same trick ranal_demo's offscreen surface uses. */
    ranal_surface_t *surface;
    int      surface_w;
    int      surface_h;

    int      pty_fd;
    pid_t    child_pid;

    /* pre-typed input buffer (writes coalesced once per frame). */
    char     in_buf[2048];
    int      in_len;

    /* blink animation */
    int      blink_phase;
    int      frames_since_blink;
} term_t;

static term_t g_t;

/* xterm-ish 16-color palette. */
static const uint32_t palette[16] = {
    RANAL_COLOR(20,  20,  24),   /* 0 black (bg) */
    RANAL_COLOR(205, 70,  70),   /* 1 red */
    RANAL_COLOR(115, 200, 110),  /* 2 green */
    RANAL_COLOR(220, 200, 90),   /* 3 yellow */
    RANAL_COLOR(90,  150, 240),  /* 4 blue */
    RANAL_COLOR(200, 110, 220),  /* 5 magenta */
    RANAL_COLOR(80,  200, 220),  /* 6 cyan */
    RANAL_COLOR(220, 222, 230),  /* 7 white (default fg) */
    RANAL_COLOR(90,  95,  105),  /* 8 bright black */
    RANAL_COLOR(255, 110, 110),  /* 9 bright red */
    RANAL_COLOR(150, 240, 140),  /* 10 bright green */
    RANAL_COLOR(255, 230, 130),  /* 11 bright yellow */
    RANAL_COLOR(140, 190, 255),  /* 12 bright blue */
    RANAL_COLOR(230, 150, 255),  /* 13 bright magenta */
    RANAL_COLOR(140, 240, 250),  /* 14 bright cyan */
    RANAL_COLOR(255, 255, 255),  /* 15 bright white */
};

#define DEFAULT_FG 7
#define DEFAULT_BG 0

static cell_t make_blank(const term_t *t) {
    cell_t c = { ' ', t->cur_fg, t->cur_bg, 0 };
    (void)t;
    c.fg = DEFAULT_FG;
    c.bg = DEFAULT_BG;
    c.attrs = 0;
    return c;
}

static void clear_region(term_t *t, cell_t *grid, int from, int to) {
    cell_t blank = make_blank(t);
    for (int i = from; i < to; i++) grid[i] = blank;
}

static int alloc_grids(term_t *t, int cols, int rows) {
    cell_t *g1 = calloc((size_t)cols * (size_t)rows, sizeof(cell_t));
    cell_t *g2 = calloc((size_t)cols * (size_t)rows, sizeof(cell_t));
    cell_t *sb = calloc((size_t)cols * (size_t)SCROLLBACK, sizeof(cell_t));
    if (g1 == NULL || g2 == NULL || sb == NULL) {
        free(g1); free(g2); free(sb);
        return -1;
    }
    t->primary_grid = g1;
    t->alt_grid = g2;
    t->scroll = sb;
    t->grid = g1;
    t->cols = cols;
    t->rows = rows;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    t->cur_fg = DEFAULT_FG;
    t->cur_bg = DEFAULT_BG;
    t->cur_attrs = 0;
    clear_region(t, g1, 0, cols * rows);
    clear_region(t, g2, 0, cols * rows);
    clear_region(t, sb, 0, cols * SCROLLBACK);
    return 0;
}

static void free_grids(term_t *t) {
    free(t->primary_grid);
    free(t->alt_grid);
    free(t->scroll);
    t->primary_grid = t->alt_grid = t->scroll = t->grid = NULL;
}

static cell_t *cell_at(term_t *t, int row, int col) {
    return &t->grid[row * t->cols + col];
}

static void push_scrollback_row(term_t *t, const cell_t *row_cells) {
    /* Don't capture into scrollback while on alt screen (vim, less, top). */
    if (t->alt_screen) return;
    if (t->scroll_used == SCROLLBACK) {
        memmove(t->scroll, t->scroll + t->cols,
                (size_t)(SCROLLBACK - 1) * t->cols * sizeof(cell_t));
        t->scroll_used = SCROLLBACK - 1;
    }
    memcpy(&t->scroll[t->scroll_used * t->cols], row_cells,
           (size_t)t->cols * sizeof(cell_t));
    t->scroll_used++;
}

static void scroll_up(term_t *t, int top, int bot, int n) {
    if (n <= 0 || top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    /* Top lines of the scroll region get captured into history when the
       whole screen scrolls. */
    if (top == 0 && bot == t->rows - 1) {
        for (int i = 0; i < n; i++) {
            push_scrollback_row(t, cell_at(t, i, 0));
        }
        t->view_offset = 0;
    }
    int lines_to_move = bot - top + 1 - n;
    if (lines_to_move > 0) {
        memmove(cell_at(t, top, 0), cell_at(t, top + n, 0),
                (size_t)lines_to_move * t->cols * sizeof(cell_t));
    }
    cell_t blank = make_blank(t);
    for (int r = bot - n + 1; r <= bot; r++) {
        for (int c = 0; c < t->cols; c++) cell_at(t, r, c)[0] = blank;
    }
}

static void scroll_down(term_t *t, int top, int bot, int n) {
    if (n <= 0 || top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    int lines_to_move = bot - top + 1 - n;
    if (lines_to_move > 0) {
        memmove(cell_at(t, top + n, 0), cell_at(t, top, 0),
                (size_t)lines_to_move * t->cols * sizeof(cell_t));
    }
    cell_t blank = make_blank(t);
    for (int r = top; r < top + n; r++) {
        for (int c = 0; c < t->cols; c++) cell_at(t, r, c)[0] = blank;
    }
}

static void clamp_cursor(term_t *t) {
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void put_glyph(term_t *t, char ch) {
    if (t->cx >= t->cols) {
        t->cx = 0;
        t->cy++;
        if (t->cy > t->scroll_bot) {
            scroll_up(t, t->scroll_top, t->scroll_bot, 1);
            t->cy = t->scroll_bot;
        }
    }
    cell_t *c = cell_at(t, t->cy, t->cx);
    c->ch = ch;
    c->fg = t->cur_fg;
    c->bg = t->cur_bg;
    c->attrs = t->cur_attrs;
    t->cx++;
}

static int param_or(term_t *t, int idx, int def) {
    if (idx < t->csi_nparam && t->csi_params[idx] > 0) return t->csi_params[idx];
    return def;
}

static void apply_sgr(term_t *t) {
    if (t->csi_nparam == 0) {
        t->cur_fg = DEFAULT_FG;
        t->cur_bg = DEFAULT_BG;
        t->cur_attrs = 0;
        return;
    }
    for (int i = 0; i < t->csi_nparam; i++) {
        int p = t->csi_params[i];
        if (p == 0) {
            t->cur_fg = DEFAULT_FG;
            t->cur_bg = DEFAULT_BG;
            t->cur_attrs = 0;
        } else if (p == 1) {
            t->cur_attrs |= ATTR_BOLD;
        } else if (p == 2) {
            t->cur_attrs |= ATTR_DIM;
        } else if (p == 4) {
            t->cur_attrs |= ATTR_UNDER;
        } else if (p == 7) {
            t->cur_attrs |= ATTR_REVERSE;
        } else if (p == 22) {
            t->cur_attrs &= ~(ATTR_BOLD | ATTR_DIM);
        } else if (p == 24) {
            t->cur_attrs &= ~ATTR_UNDER;
        } else if (p == 27) {
            t->cur_attrs &= ~ATTR_REVERSE;
        } else if (p >= 30 && p <= 37) {
            t->cur_fg = (uint8_t)(p - 30);
        } else if (p == 38 && i + 2 < t->csi_nparam && t->csi_params[i + 1] == 5) {
            int n = t->csi_params[i + 2];
            if (n < 16) t->cur_fg = (uint8_t)n;
            else if (n < 232) {
                /* 6x6x6 cube -> nearest base */
                int v = n - 16;
                int r = (v / 36) % 6;
                int g = (v / 6) % 6;
                int b = v % 6;
                int idx = 0;
                if (r > 2) idx |= 1;
                if (g > 2) idx |= 2;
                if (b > 2) idx |= 4;
                if (r + g + b > 9) idx += 8;
                t->cur_fg = (uint8_t)idx;
            } else {
                int gray = n - 232;
                t->cur_fg = (uint8_t)(gray < 12 ? 8 : 15);
            }
            i += 2;
        } else if (p == 39) {
            t->cur_fg = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            t->cur_bg = (uint8_t)(p - 40);
        } else if (p == 48 && i + 2 < t->csi_nparam && t->csi_params[i + 1] == 5) {
            int n = t->csi_params[i + 2];
            if (n < 16) t->cur_bg = (uint8_t)n;
            i += 2;
        } else if (p == 49) {
            t->cur_bg = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            t->cur_fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            t->cur_bg = (uint8_t)(p - 100 + 8);
        }
    }
}

static void switch_alt(term_t *t, int on) {
    if (on == t->alt_screen) return;
    cell_t *target = on ? t->alt_grid : t->primary_grid;
    t->grid = target;
    t->alt_screen = on;
    if (on) {
        clear_region(t, t->grid, 0, t->cols * t->rows);
        t->cx = 0; t->cy = 0;
    }
}

static void handle_csi(term_t *t, char final) {
    int n;
    cell_t blank;
    switch (final) {
    case 'A': /* CUU */
        t->cy -= param_or(t, 0, 1);
        if (t->cy < t->scroll_top) t->cy = t->scroll_top;
        break;
    case 'B': /* CUD */
        t->cy += param_or(t, 0, 1);
        if (t->cy > t->scroll_bot) t->cy = t->scroll_bot;
        break;
    case 'C': /* CUF */
        t->cx += param_or(t, 0, 1);
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        break;
    case 'D': /* CUB */
        t->cx -= param_or(t, 0, 1);
        if (t->cx < 0) t->cx = 0;
        break;
    case 'E': /* CNL */
        t->cy += param_or(t, 0, 1);
        t->cx = 0;
        clamp_cursor(t);
        break;
    case 'F': /* CPL */
        t->cy -= param_or(t, 0, 1);
        t->cx = 0;
        clamp_cursor(t);
        break;
    case 'G': /* CHA */
        t->cx = param_or(t, 0, 1) - 1;
        clamp_cursor(t);
        break;
    case 'H': case 'f': /* CUP / HVP */
        t->cy = param_or(t, 0, 1) - 1;
        t->cx = param_or(t, 1, 1) - 1;
        clamp_cursor(t);
        break;
    case 'd': /* VPA */
        t->cy = param_or(t, 0, 1) - 1;
        clamp_cursor(t);
        break;
    case 'J': /* ED */
        n = t->csi_nparam == 0 ? 0 : t->csi_params[0];
        blank = make_blank(t);
        if (n == 0) {
            /* cursor to end of screen */
            for (int c = t->cx; c < t->cols; c++) cell_at(t, t->cy, c)[0] = blank;
            for (int r = t->cy + 1; r < t->rows; r++)
                for (int c = 0; c < t->cols; c++) cell_at(t, r, c)[0] = blank;
        } else if (n == 1) {
            for (int r = 0; r < t->cy; r++)
                for (int c = 0; c < t->cols; c++) cell_at(t, r, c)[0] = blank;
            for (int c = 0; c <= t->cx && c < t->cols; c++) cell_at(t, t->cy, c)[0] = blank;
        } else {
            for (int r = 0; r < t->rows; r++)
                for (int c = 0; c < t->cols; c++) cell_at(t, r, c)[0] = blank;
            if (n == 3) {
                t->scroll_used = 0;
                t->view_offset = 0;
            }
        }
        break;
    case 'K': /* EL */
        n = t->csi_nparam == 0 ? 0 : t->csi_params[0];
        blank = make_blank(t);
        if (n == 0) {
            for (int c = t->cx; c < t->cols; c++) cell_at(t, t->cy, c)[0] = blank;
        } else if (n == 1) {
            for (int c = 0; c <= t->cx && c < t->cols; c++) cell_at(t, t->cy, c)[0] = blank;
        } else {
            for (int c = 0; c < t->cols; c++) cell_at(t, t->cy, c)[0] = blank;
        }
        break;
    case 'L': /* IL */
        if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot) {
            scroll_down(t, t->cy, t->scroll_bot, param_or(t, 0, 1));
        }
        break;
    case 'M': /* DL */
        if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot) {
            scroll_up(t, t->cy, t->scroll_bot, param_or(t, 0, 1));
        }
        break;
    case '@': /* ICH */
        n = param_or(t, 0, 1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(cell_at(t, t->cy, t->cx + n), cell_at(t, t->cy, t->cx),
                (size_t)(t->cols - t->cx - n) * sizeof(cell_t));
        blank = make_blank(t);
        for (int c = 0; c < n; c++) cell_at(t, t->cy, t->cx + c)[0] = blank;
        break;
    case 'P': /* DCH */
        n = param_or(t, 0, 1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(cell_at(t, t->cy, t->cx), cell_at(t, t->cy, t->cx + n),
                (size_t)(t->cols - t->cx - n) * sizeof(cell_t));
        blank = make_blank(t);
        for (int c = 0; c < n; c++) cell_at(t, t->cy, t->cols - 1 - c)[0] = blank;
        break;
    case 'S': /* SU */
        scroll_up(t, t->scroll_top, t->scroll_bot, param_or(t, 0, 1));
        break;
    case 'T': /* SD */
        scroll_down(t, t->scroll_top, t->scroll_bot, param_or(t, 0, 1));
        break;
    case 'r': /* DECSTBM */
        if (t->csi_nparam >= 2 && t->csi_params[0] > 0 && t->csi_params[1] > 0) {
            t->scroll_top = t->csi_params[0] - 1;
            t->scroll_bot = t->csi_params[1] - 1;
        } else {
            t->scroll_top = 0;
            t->scroll_bot = t->rows - 1;
        }
        if (t->scroll_top < 0) t->scroll_top = 0;
        if (t->scroll_bot >= t->rows) t->scroll_bot = t->rows - 1;
        t->cx = 0;
        t->cy = 0;
        break;
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; clamp_cursor(t); break;
    case 'm':
        apply_sgr(t);
        break;
    case 'h': case 'l': {
        int set = (final == 'h');
        if (t->csi_priv) {
            for (int i = 0; i < t->csi_nparam; i++) {
                int p = t->csi_params[i];
                if (p == 25) {
                    t->cursor_visible = set ? 1 : 0;
                } else if (p == 1049 || p == 47 || p == 1047) {
                    switch_alt(t, set);
                } else if (p == 1) {
                    /* app cursor keys, ignore for now */
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static void parser_reset_csi(term_t *t) {
    t->csi_nparam = 0;
    memset(t->csi_params, 0, sizeof(t->csi_params));
    t->csi_priv = 0;
}

static void feed_byte(term_t *t, unsigned char b) {
    switch (t->pstate) {
    case P_NORMAL:
        if (b == 0x1b) { t->pstate = P_ESC; return; }
        if (b == '\r') { t->cx = 0; return; }
        if (b == '\n' || b == 0x0b || b == 0x0c) {
            t->cy++;
            if (t->cy > t->scroll_bot) {
                scroll_up(t, t->scroll_top, t->scroll_bot, 1);
                t->cy = t->scroll_bot;
            }
            return;
        }
        if (b == '\b') { if (t->cx > 0) t->cx--; return; }
        if (b == '\t') {
            int target = (t->cx / 8 + 1) * 8;
            if (target >= t->cols) target = t->cols - 1;
            t->cx = target;
            return;
        }
        if (b == '\a') return;
        if (b == 0x0e || b == 0x0f) return;
        if (b < 0x20) return;
        if (b >= 0x80) b = '?';
        put_glyph(t, (char)b);
        return;
    case P_ESC:
        if (b == '[') {
            t->pstate = P_CSI;
            parser_reset_csi(t);
            return;
        }
        if (b == ']') {
            t->pstate = P_OSC;
            t->osc_len = 0;
            t->osc_kind = -1;
            return;
        }
        if (b == 'M') { /* RI */
            if (t->cy == t->scroll_top) {
                scroll_down(t, t->scroll_top, t->scroll_bot, 1);
            } else {
                t->cy--;
            }
        } else if (b == 'D') {
            if (t->cy == t->scroll_bot) scroll_up(t, t->scroll_top, t->scroll_bot, 1);
            else t->cy++;
        } else if (b == 'E') {
            t->cx = 0;
            t->cy++;
            if (t->cy > t->scroll_bot) {
                scroll_up(t, t->scroll_top, t->scroll_bot, 1);
                t->cy = t->scroll_bot;
            }
        } else if (b == '7') { t->saved_cx = t->cx; t->saved_cy = t->cy; }
          else if (b == '8') { t->cx = t->saved_cx; t->cy = t->saved_cy; clamp_cursor(t); }
          else if (b == 'c') {
            clear_region(t, t->grid, 0, t->cols * t->rows);
            t->cx = 0; t->cy = 0;
            t->cur_fg = DEFAULT_FG; t->cur_bg = DEFAULT_BG; t->cur_attrs = 0;
        }
        t->pstate = P_NORMAL;
        return;
    case P_CSI:
        if (b == '?') { t->csi_priv = 1; return; }
        if (b >= '0' && b <= '9') {
            if (t->csi_nparam == 0) t->csi_nparam = 1;
            int idx = t->csi_nparam - 1;
            if (idx < 16) {
                t->csi_params[idx] = t->csi_params[idx] * 10 + (b - '0');
            }
            return;
        }
        if (b == ';') {
            if (t->csi_nparam < 16) t->csi_nparam++;
            return;
        }
        if (b >= 0x40 && b <= 0x7e) {
            if (t->csi_nparam == 0 && (t->csi_params[0] != 0 || b != 'm')) {
                /* nothing — handle_csi consults nparam */
            }
            handle_csi(t, (char)b);
            t->pstate = P_NORMAL;
            return;
        }
        /* unknown intermediate – skip */
        return;
    case P_OSC:
        if (b == 0x07 || b == 0x9c) {
            t->osc_buf[t->osc_len < (int)sizeof(t->osc_buf) ? t->osc_len : (int)sizeof(t->osc_buf) - 1] = '\0';
            /* OSC 0;<title> or 2;<title> -> set window title */
            if (t->osc_len > 2 && (t->osc_buf[0] == '0' || t->osc_buf[0] == '2') &&
                t->osc_buf[1] == ';') {
                ranal_set_window_title(t->osc_buf + 2);
            }
            t->pstate = P_NORMAL;
            return;
        }
        if (b == 0x1b) {
            /* ESC \ ST sequence — wait for next byte */
            return;
        }
        if (t->osc_len + 1 < (int)sizeof(t->osc_buf)) {
            t->osc_buf[t->osc_len++] = (char)b;
        }
        return;
    }
}

static int pty_spawn(term_t *t, int cols, int rows) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    char *slave_name = ptsname(master);
    if (slave_name == NULL) { close(master); return -1; }

    pid_t pid = fork();
    if (pid < 0) { close(master); return -1; }
    if (pid == 0) {
        setsid();
        int sfd = open(slave_name, O_RDWR);
        if (sfd < 0) _exit(127);
        ioctl(sfd, TIOCSCTTY, 0);
        dup2(sfd, 0);
        dup2(sfd, 1);
        dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        close(master);

        struct winsize ws = {
            .ws_row = (unsigned short)rows,
            .ws_col = (unsigned short)cols,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        ioctl(0, TIOCSWINSZ, &ws);

        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        char colstr[16], rowstr[16];
        snprintf(colstr, sizeof(colstr), "%d", cols);
        snprintf(rowstr, sizeof(rowstr), "%d", rows);
        setenv("COLUMNS", colstr, 1);
        setenv("LINES", rowstr, 1);

        const char *shell = getenv("SHELL");
        if (shell == NULL || shell[0] == '\0') {
            struct passwd *pw = getpwuid(getuid());
            if (pw != NULL && pw->pw_shell[0] != '\0') shell = pw->pw_shell;
            else shell = "/bin/sh";
        }
        const char *argv0 = strrchr(shell, '/');
        argv0 = argv0 != NULL ? argv0 + 1 : shell;
        char dash_name[64];
        snprintf(dash_name, sizeof(dash_name), "-%s", argv0);
        execl(shell, dash_name, (char *)NULL);
        _exit(127);
    }

    int fl = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);

    t->pty_fd = master;
    t->child_pid = pid;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
    };
    ioctl(master, TIOCSWINSZ, &ws);
    return 0;
}

static void pty_drain(term_t *t) {
    if (t->pty_fd < 0) return;
    unsigned char buf[8192];
    for (;;) {
        ssize_t n = read(t->pty_fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) feed_byte(t, buf[i]);
            t->view_offset = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n == 0 || (n < 0 && errno == EIO)) {
            /* child closed */
            close(t->pty_fd);
            t->pty_fd = -1;
            return;
        }
        if (n < 0 && errno == EINTR) continue;
        return;
    }
}

static void push_input(term_t *t, const char *s, int len) {
    if (len <= 0) return;
    if (t->in_len + len > (int)sizeof(t->in_buf)) {
        len = (int)sizeof(t->in_buf) - t->in_len;
        if (len <= 0) return;
    }
    memcpy(t->in_buf + t->in_len, s, (size_t)len);
    t->in_len += len;
}

static void flush_input(term_t *t) {
    if (t->in_len == 0 || t->pty_fd < 0) return;
    int off = 0;
    while (off < t->in_len) {
        ssize_t w = write(t->pty_fd, t->in_buf + off, (size_t)(t->in_len - off));
        if (w > 0) { off += (int)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    t->in_len = 0;
}

/* ---------- key translation ---------- */

static int key_hook(uint32_t scancode, uint32_t mods, int pressed, void *user) {
    if (!pressed) return 0;
    term_t *t = (term_t *)user;
    int ctrl = (mods & SRAPI_KMOD_CTRL) != 0;
    int shift = (mods & SRAPI_KMOD_SHIFT) != 0;
    int alt = (mods & SRAPI_KMOD_ALT) != 0;

    /* Shift+PageUp/PageDown scrolls the view through scrollback. */
    if (shift && scancode == SRAPI_SCANCODE_PAGEUP) {
        t->view_offset += t->rows / 2;
        if (t->view_offset > t->scroll_used) t->view_offset = t->scroll_used;
        return 1;
    }
    if (shift && scancode == SRAPI_SCANCODE_PAGEDOWN) {
        t->view_offset -= t->rows / 2;
        if (t->view_offset < 0) t->view_offset = 0;
        return 1;
    }

    switch (scancode) {
    case SRAPI_SCANCODE_RETURN:
    case SRAPI_SCANCODE_KP_ENTER:
        push_input(t, "\r", 1);
        return 1;
    case SRAPI_SCANCODE_BACKSPACE:
        push_input(t, "\x7f", 1);
        return 1;
    case SRAPI_SCANCODE_TAB:
        if (shift) push_input(t, "\x1b[Z", 3);
        else push_input(t, "\t", 1);
        return 1;
    case SRAPI_SCANCODE_ESCAPE:
        push_input(t, "\x1b", 1);
        return 1;
    case SRAPI_SCANCODE_UP:     push_input(t, "\x1b[A", 3); return 1;
    case SRAPI_SCANCODE_DOWN:   push_input(t, "\x1b[B", 3); return 1;
    case SRAPI_SCANCODE_RIGHT:  push_input(t, "\x1b[C", 3); return 1;
    case SRAPI_SCANCODE_LEFT:   push_input(t, "\x1b[D", 3); return 1;
    case SRAPI_SCANCODE_HOME:   push_input(t, "\x1b[H", 3); return 1;
    case SRAPI_SCANCODE_END:    push_input(t, "\x1b[F", 3); return 1;
    case SRAPI_SCANCODE_PAGEUP:   push_input(t, "\x1b[5~", 4); return 1;
    case SRAPI_SCANCODE_PAGEDOWN: push_input(t, "\x1b[6~", 4); return 1;
    case SRAPI_SCANCODE_INSERT:   push_input(t, "\x1b[2~", 4); return 1;
    case SRAPI_SCANCODE_DELETE:   push_input(t, "\x1b[3~", 4); return 1;
    case SRAPI_SCANCODE_F1:  push_input(t, "\x1bOP", 3); return 1;
    case SRAPI_SCANCODE_F2:  push_input(t, "\x1bOQ", 3); return 1;
    case SRAPI_SCANCODE_F3:  push_input(t, "\x1bOR", 3); return 1;
    case SRAPI_SCANCODE_F4:  push_input(t, "\x1bOS", 3); return 1;
    case SRAPI_SCANCODE_F5:  push_input(t, "\x1b[15~", 5); return 1;
    case SRAPI_SCANCODE_F6:  push_input(t, "\x1b[17~", 5); return 1;
    case SRAPI_SCANCODE_F7:  push_input(t, "\x1b[18~", 5); return 1;
    case SRAPI_SCANCODE_F8:  push_input(t, "\x1b[19~", 5); return 1;
    case SRAPI_SCANCODE_F9:  push_input(t, "\x1b[20~", 5); return 1;
    case SRAPI_SCANCODE_F10: push_input(t, "\x1b[21~", 5); return 1;
    case SRAPI_SCANCODE_F11: push_input(t, "\x1b[23~", 5); return 1;
    case SRAPI_SCANCODE_F12: push_input(t, "\x1b[24~", 5); return 1;
    default: break;
    }

    /* Ctrl+letter → control code. */
    if (ctrl && scancode >= SRAPI_SCANCODE_A && scancode <= SRAPI_SCANCODE_Z) {
        char c = (char)(scancode - SRAPI_SCANCODE_A + 1);
        push_input(t, &c, 1);
        return 1;
    }
    if (ctrl && scancode == SRAPI_SCANCODE_LEFTBRACKET) {
        push_input(t, "\x1b", 1); return 1;
    }
    if (ctrl && scancode == SRAPI_SCANCODE_BACKSLASH) {
        push_input(t, "\x1c", 1); return 1;
    }
    if (ctrl && scancode == SRAPI_SCANCODE_RIGHTBRACKET) {
        push_input(t, "\x1d", 1); return 1;
    }
    if (ctrl && scancode == SRAPI_SCANCODE_SPACE) {
        push_input(t, "\x00", 1); return 1;
    }

    /* Printable char from scancode. */
    char ch = ranal_scancode_to_char(scancode, mods);
    if (ch == 0) return 0;
    if (alt) push_input(t, "\x1b", 1);
    push_input(t, &ch, 1);
    return 1;
}

/* ---------- rendering ---------- */

static uint32_t cell_fg(const cell_t *c) {
    uint8_t idx = c->fg;
    if (c->attrs & ATTR_BOLD) {
        if (idx < 8) idx = (uint8_t)(idx + 8);
    }
    if (c->attrs & ATTR_REVERSE) idx = c->bg;
    if (c->attrs & ATTR_DIM) {
        uint32_t p = palette[idx];
        uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
        r /= 2; g /= 2; b /= 2;
        return RANAL_COLOR(r, g, b);
    }
    return palette[idx];
}

static uint32_t cell_bg(const cell_t *c) {
    uint8_t idx = c->bg;
    if (c->attrs & ATTR_REVERSE) idx = c->fg;
    return palette[idx];
}

static const cell_t *row_for_screen_line(const term_t *t, int screen_line) {
    /* screen_line 0 = top visible row. Account for view_offset into history. */
    if (t->view_offset > 0) {
        int hist_rows = t->view_offset; /* lines pulled up from history */
        if (screen_line < hist_rows) {
            int src_idx = t->scroll_used - hist_rows + screen_line;
            if (src_idx >= 0 && src_idx < t->scroll_used) {
                return &t->scroll[src_idx * t->cols];
            }
            return NULL;
        }
        return &t->grid[(screen_line - hist_rows) * t->cols];
    }
    return &t->grid[screen_line * t->cols];
}

static void fill_rect_px(uint32_t *px, int pitch_px, int sw, int sh,
                         int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *r = px + (size_t)(y + row) * pitch_px + x;
        for (int col = 0; col < w; col++) r[col] = color;
    }
}

static void draw_glyph_px(uint32_t *px, int pitch_px, int sw, int sh,
                          int x, int y, char ch, uint32_t color) {
    const uint8_t *g = ranal_font_glyph(ch);
    if (g == NULL) return;
    for (int gy = 0; gy < RANAL_GLYPH_HEIGHT; gy++) {
        uint8_t bits = g[gy];
        int py = y + gy;
        if (py < 0 || py >= sh) continue;
        for (int gx = 0; gx < RANAL_GLYPH_WIDTH; gx++) {
            if ((bits & (1u << (RANAL_GLYPH_WIDTH - 1 - gx))) == 0) continue;
            int xx = x + gx;
            if (xx < 0 || xx >= sw) continue;
            px[(size_t)py * pitch_px + xx] = color;
        }
    }
}

static void render_term(term_t *t) {
    if (t->surface == NULL) return;
    uint32_t *px = ranal_surface_pixels(t->surface);
    if (px == NULL) return;
    int sw = ranal_surface_width(t->surface);
    int sh = ranal_surface_height(t->surface);
    int pitch_px = ranal_surface_pitch(t->surface) / 4;

    /* Whole pane background. */
    fill_rect_px(px, pitch_px, sw, sh, 0, 0, sw, sh, palette[DEFAULT_BG]);

    for (int row = 0; row < t->rows; row++) {
        const cell_t *line = row_for_screen_line(t, row);
        if (line == NULL) continue;
        int y = row * CELL_H;

        /* Backgrounds as horizontal runs to save fills. */
        int run_start = 0;
        uint8_t run_bg = line[0].bg;
        uint8_t run_attr = line[0].attrs & ATTR_REVERSE;
        for (int col = 1; col <= t->cols; col++) {
            int boundary = (col == t->cols) ||
                           (line[col].bg != run_bg) ||
                           ((line[col].attrs & ATTR_REVERSE) != run_attr);
            if (boundary) {
                cell_t prototype = { ' ', line[run_start].fg, run_bg, run_attr };
                uint32_t bg = cell_bg(&prototype);
                if (bg != palette[DEFAULT_BG]) {
                    fill_rect_px(px, pitch_px, sw, sh,
                                 run_start * CELL_W, y,
                                 (col - run_start) * CELL_W, CELL_H, bg);
                }
                if (col < t->cols) {
                    run_start = col;
                    run_bg = line[col].bg;
                    run_attr = line[col].attrs & ATTR_REVERSE;
                }
            }
        }

        /* Glyphs. */
        for (int col = 0; col < t->cols; col++) {
            const cell_t *c = &line[col];
            if (c->ch == ' ' || c->ch == 0) continue;
            draw_glyph_px(px, pitch_px, sw, sh,
                          col * CELL_W, y, c->ch, cell_fg(c));
        }
    }

    /* Cursor — only when viewing live content. */
    if (t->cursor_visible && t->view_offset == 0 && t->blink_phase < 30) {
        int cx = t->cx * CELL_W;
        int cy = t->cy * CELL_H;
        uint32_t cursor_col = RANAL_COLOR(220, 222, 230);
        fill_rect_px(px, pitch_px, sw, sh, cx, cy, CELL_W, CELL_H, cursor_col);
        const cell_t *c = cell_at(t, t->cy, t->cx);
        if (c->ch != ' ' && c->ch != 0) {
            draw_glyph_px(px, pitch_px, sw, sh, cx, cy, c->ch, palette[DEFAULT_BG]);
        }
    }

    if (t->view_offset > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "-- scroll: -%d --", t->view_offset);
        int w = (int)strlen(msg) * CELL_W;
        fill_rect_px(px, pitch_px, sw, sh,
                     sw - w - 4, 0, w + 4, CELL_H,
                     RANAL_COLOR(60, 60, 80));
        for (size_t i = 0; msg[i] != '\0'; i++) {
            draw_glyph_px(px, pitch_px, sw, sh,
                          sw - w - 2 + (int)i * CELL_W, 0,
                          msg[i], RANAL_COLOR(230, 230, 240));
        }
    }
}

/* ---------- main ---------- */

static void print_help(const char *prog) {
    printf("term - OneTool GUI terminal emulator\n");
    printf("usage: %s [--swm [WxH]] [--title TITLE] [--shell PATH]\n", prog);
    printf("\n");
    printf("  --swm [WxH]    run as a sprot/swm client (windowed)\n");
    printf("  --title TITLE  window title (default: term)\n");
    printf("  (no flag)      run standalone using ranal/srapi backend\n");
}

int main(int argc, char *argv[]) {
    int swm_mode = 0;
    int32_t swm_w = DEFAULT_COLS * CELL_W + 2 * PAD_X;
    int32_t swm_h = DEFAULT_ROWS * CELL_H + 2 * PAD_Y;
    const char *swm_title = "term";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]); return 0;
        } else if (strcmp(argv[i], "--swm") == 0) {
            swm_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int w = 0, h = 0;
                if (sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    swm_w = w; swm_h = h; i++;
                }
            }
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            swm_title = argv[++i];
        }
    }

    if (swm_mode) {
        if (ranal_init_swm(swm_title, swm_w, swm_h) != RANAL_OK) {
            fprintf(stderr, "term: %s\n", ranal_last_error());
            return 1;
        }
    } else {
        ranal_window_desc_t desc = { .width = 0, .height = 0, .title = "term" };
        if (ranal_init(&desc) != RANAL_OK) {
            fprintf(stderr, "term: %s\n", ranal_last_error());
            return 1;
        }
    }

    int win_w = ranal_window_width();
    int win_h = ranal_window_height();
    int cols = (win_w - 2 * PAD_X) / CELL_W;
    int rows = (win_h - 2 * PAD_Y) / CELL_H;
    if (cols < 20) cols = 20;
    if (rows < 5)  rows = 5;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;

    memset(&g_t, 0, sizeof(g_t));
    g_t.pty_fd = -1;
    g_t.cursor_visible = 1;
    if (alloc_grids(&g_t, cols, rows) != 0) {
        fprintf(stderr, "term: out of memory\n");
        ranal_shutdown();
        return 1;
    }
    if (pty_spawn(&g_t, cols, rows) != 0) {
        fprintf(stderr, "term: pty spawn failed: %s\n", strerror(errno));
        free_grids(&g_t);
        ranal_shutdown();
        return 1;
    }

    /* Suppress ESC=close in ranal so it can be sent to the shell. */
    ranal_set_key_hook(key_hook, &g_t);

    /* Make the root surface a uniform dark background so any unused
       padding pixels don't show whatever ranal painted previously. */
    ranal_widget_t *root = ranal_root();
    ranal_set_background(root, palette[DEFAULT_BG]);

    g_t.surface_w = cols * CELL_W;
    g_t.surface_h = rows * CELL_H;
    g_t.surface = ranal_surface_create(g_t.surface_w, g_t.surface_h);
    if (g_t.surface == NULL) {
        fprintf(stderr, "term: surface alloc failed: %s\n", ranal_last_error());
        if (g_t.child_pid > 0) kill(g_t.child_pid, SIGHUP);
        if (g_t.pty_fd >= 0) close(g_t.pty_fd);
        free_grids(&g_t);
        ranal_shutdown();
        return 1;
    }

    while (!ranal_should_close()) {
        pty_drain(&g_t);
        flush_input(&g_t);

        /* Reap the child non-blockingly to detect shell exit. */
        if (g_t.child_pid > 0) {
            int status = 0;
            pid_t r = waitpid(g_t.child_pid, &status, WNOHANG);
            if (r == g_t.child_pid) {
                g_t.child_pid = 0;
                ranal_request_close();
            }
        }
        if (g_t.pty_fd < 0 && g_t.child_pid == 0) {
            ranal_request_close();
        }

        g_t.frames_since_blink++;
        if (g_t.frames_since_blink > 30) {
            g_t.frames_since_blink = 0;
            g_t.blink_phase = (g_t.blink_phase < 30) ? 60 : 0;
        }
        if (g_t.blink_phase > 0) g_t.blink_phase--;

        /* Paint terminal into our own surface, then push frame through
           ranal so input is polled and the backbuffer is presented. We
           invalidate ranal so it actually submits the clear/widgets each
           frame (otherwise it short-circuits after two warm frames). */
        render_term(&g_t);
        ranal_invalidate();

        if (ranal_render() != 0) break;
        ranal_blit_surface(g_t.surface, PAD_X, PAD_Y);
        if (ranal_present() != 0) break;
    }

    if (g_t.surface != NULL) ranal_surface_destroy(g_t.surface);

    if (g_t.child_pid > 0) {
        kill(g_t.child_pid, SIGHUP);
        waitpid(g_t.child_pid, NULL, 0);
    }
    if (g_t.pty_fd >= 0) close(g_t.pty_fd);
    free_grids(&g_t);
    ranal_shutdown();
    return 0;
}
