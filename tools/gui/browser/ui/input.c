/*
 * Input plumbing. ranal exposes a single key-hook; we route all keys
 * through it. URL-bar focus toggles whether keys edit the URL or scroll
 * the page / move link focus.
 */
#include "input.h"
#include "../app.h"

#include <ranal/ranal.h>
#include <srapi/srapi.h>
#include <string.h>
#include <stdlib.h>

/* App pointer for the hook. The hook signature is fixed so we stash a
 * file-local pointer rather than using `void *user`. (ranal does pass
 * user data through, but we keep it simple.) */
static browser_app_t *g_app = NULL;

static int utf8_emit(char *buf, size_t buf_size, size_t *pos, uint32_t cp) {
    if (cp < 0x80) {
        if (*pos + 1 >= buf_size) return -1;
        buf[(*pos)++] = (char)cp;
        return 0;
    }
    if (cp < 0x800) {
        if (*pos + 2 >= buf_size) return -1;
        buf[(*pos)++] = (char)(0xC0 | (cp >> 6));
        buf[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        return 0;
    }
    if (cp < 0x10000) {
        if (*pos + 3 >= buf_size) return -1;
        buf[(*pos)++] = (char)(0xE0 | (cp >> 12));
        buf[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[(*pos)++] = (char)(0x80 | (cp & 0x3F));
        return 0;
    }
    return -1;
}

static void url_insert_char(browser_app_t *app, char c) {
    if (app->url_edit_len + 1 >= sizeof(app->url_edit)) return;
    memmove(app->url_edit + app->url_cursor + 1,
            app->url_edit + app->url_cursor,
            app->url_edit_len - app->url_cursor + 1);
    app->url_edit[app->url_cursor] = c;
    app->url_edit_len++;
    app->url_cursor++;
}

static void url_backspace(browser_app_t *app) {
    if (app->url_cursor == 0) return;
    /* Step back one UTF-8 codepoint. */
    size_t step = 1;
    while (step <= app->url_cursor &&
           (((unsigned char)app->url_edit[app->url_cursor - step] & 0xC0) == 0x80)) {
        step++;
    }
    if (step > app->url_cursor) step = app->url_cursor;
    memmove(app->url_edit + app->url_cursor - step,
            app->url_edit + app->url_cursor,
            app->url_edit_len - app->url_cursor + 1);
    app->url_edit_len -= step;
    app->url_cursor -= step;
}

static void url_clear(browser_app_t *app) {
    app->url_edit[0] = '\0';
    app->url_edit_len = 0;
    app->url_cursor = 0;
}

static void focus_url_bar(browser_app_t *app) {
    strncpy(app->url_edit, app->url, sizeof(app->url_edit) - 1);
    app->url_edit[sizeof(app->url_edit) - 1] = '\0';
    app->url_edit_len = strlen(app->url_edit);
    app->url_cursor = app->url_edit_len;
    app->url_focused = 1;
    app->chrome_dirty = 1;
}

static int link_count(const browser_app_t *app) {
    if (app->doc == NULL) return 0;
    return (int)app->doc->link_count;
}

static int key_hook(uint32_t scancode, uint32_t modifiers,
                    int pressed, void *user) {
    (void)user;
    browser_app_t *app = g_app;
    if (app == NULL) return 0;
    if (!pressed) return 0;
    int ctrl = (modifiers & SRAPI_KMOD_CTRL) != 0;
    int alt  = (modifiers & SRAPI_KMOD_ALT) != 0;

    /* Universal shortcuts. */
    if (ctrl && scancode == SRAPI_SCANCODE_L) {
        focus_url_bar(app);
        return 1;
    }
    if (ctrl && scancode == SRAPI_SCANCODE_Q) {
        ranal_request_close();
        return 1;
    }
    if (alt && scancode == SRAPI_SCANCODE_LEFT) {
        app->pending_navigate = 0;
        if (app->history_count > 0) {
            free(app->history[app->history_count - 1]);  /* current */
            app->history_count--;
            if (app->history_count > 0) {
                strncpy(app->url, app->history[app->history_count - 1],
                        sizeof(app->url) - 1);
                app->url[sizeof(app->url) - 1] = '\0';
                strncpy(app->url_edit, app->url, sizeof(app->url_edit) - 1);
                app->url_edit[sizeof(app->url_edit) - 1] = '\0';
                app->url_edit_len = strlen(app->url_edit);
                app->url_cursor = app->url_edit_len;
                /* Pop so reload below sees the right URL and will push it
                   again as the new "current" entry. */
                free(app->history[app->history_count - 1]);
                app->history_count--;
                app->pending_navigate = 1;
            }
        }
        app->chrome_dirty = 1;
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_F5 ||
        (ctrl && scancode == SRAPI_SCANCODE_R)) {
        app->pending_navigate = 1;
        return 1;
    }

    if (app->url_focused) {
        if (scancode == SRAPI_SCANCODE_ESCAPE) {
            app->url_focused = 0;
            app->chrome_dirty = 1;
            return 1;
        }
        if (scancode == SRAPI_SCANCODE_RETURN ||
            scancode == SRAPI_SCANCODE_KP_ENTER) {
            app->url_focused = 0;
            strncpy(app->url, app->url_edit, sizeof(app->url) - 1);
            app->url[sizeof(app->url) - 1] = '\0';
            app->pending_navigate = 1;
            app->chrome_dirty = 1;
            return 1;
        }
        if (scancode == SRAPI_SCANCODE_BACKSPACE) {
            url_backspace(app);
            app->chrome_dirty = 1;
            return 1;
        }
        if (scancode == SRAPI_SCANCODE_LEFT) {
            if (app->url_cursor > 0) {
                size_t s = 1;
                while (s <= app->url_cursor &&
                       (((unsigned char)app->url_edit[app->url_cursor - s] & 0xC0) == 0x80))
                    s++;
                if (s > app->url_cursor) s = app->url_cursor;
                app->url_cursor -= s;
            }
            app->chrome_dirty = 1;
            return 1;
        }
        if (scancode == SRAPI_SCANCODE_RIGHT) {
            if (app->url_cursor < app->url_edit_len) {
                unsigned char c = (unsigned char)app->url_edit[app->url_cursor];
                int n = 1;
                if ((c & 0xE0) == 0xC0) n = 2;
                else if ((c & 0xF0) == 0xE0) n = 3;
                else if ((c & 0xF8) == 0xF0) n = 4;
                if (app->url_cursor + (size_t)n > app->url_edit_len)
                    n = (int)(app->url_edit_len - app->url_cursor);
                app->url_cursor += (size_t)n;
            }
            app->chrome_dirty = 1;
            return 1;
        }
        if (scancode == SRAPI_SCANCODE_HOME) { app->url_cursor = 0; app->chrome_dirty = 1; return 1; }
        if (scancode == SRAPI_SCANCODE_END)  { app->url_cursor = app->url_edit_len; app->chrome_dirty = 1; return 1; }
        if (ctrl && scancode == SRAPI_SCANCODE_U) { url_clear(app); app->chrome_dirty = 1; return 1; }

        char ch = ranal_scancode_to_char(scancode, modifiers);
        if (ch != 0 && (unsigned char)ch >= 0x20) {
            char tmp[8];
            size_t pos = 0;
            if (utf8_emit(tmp, sizeof(tmp), &pos, (uint32_t)(unsigned char)ch) == 0) {
                for (size_t i = 0; i < pos; i++) url_insert_char(app, tmp[i]);
                app->chrome_dirty = 1;
            }
            return 1;
        }
        return 1;
    }

    /* Page-mode keys. */
    if (scancode == SRAPI_SCANCODE_ESCAPE) {
        ranal_request_close();
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_DOWN || scancode == SRAPI_SCANCODE_J) {
        app->scroll_y += 40;
        app->page_dirty = 1;
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_UP || scancode == SRAPI_SCANCODE_K) {
        app->scroll_y -= 40;
        if (app->scroll_y < 0) app->scroll_y = 0;
        app->page_dirty = 1;
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_PAGEDOWN || scancode == SRAPI_SCANCODE_SPACE) {
        app->scroll_y += app->page_h - 40;
        app->page_dirty = 1;
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_PAGEUP) {
        app->scroll_y -= app->page_h - 40;
        if (app->scroll_y < 0) app->scroll_y = 0;
        app->page_dirty = 1;
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_HOME) { app->scroll_y = 0; app->page_dirty = 1; return 1; }
    if (scancode == SRAPI_SCANCODE_END)  {
        app->scroll_y = app->content_h;
        if (app->scroll_y < 0) app->scroll_y = 0;
        app->page_dirty = 1;
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_TAB) {
        int n = link_count(app);
        if (n > 0) {
            if (app->focused_link < 0) app->focused_link = 0;
            else app->focused_link = (app->focused_link + 1) % n;
            app->page_dirty = 1;
            app->chrome_dirty = 1;
        }
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_RETURN ||
        scancode == SRAPI_SCANCODE_KP_ENTER) {
        int n = link_count(app);
        if (app->focused_link >= 0 && app->focused_link < n) {
            br_app_follow_link(app, app->focused_link);
        }
        return 1;
    }
    if (scancode == SRAPI_SCANCODE_SLASH) {
        focus_url_bar(app);
        return 1;
    }
    return 0;
}

/* ----- mouse ----- */

static int hit_url_bar(const browser_app_t *app, int x, int y) {
    int bar_x = 12;
    int bar_y = 14;
    int bar_h = BROWSER_CHROME_HEIGHT - 24;
    int bar_w = app->win_w - 24;
    return (y >= bar_y && y < bar_y + bar_h &&
            x >= bar_x && x < bar_x + bar_w);
}

static int mouse_hook(const ranal_mouse_event_t *ev, void *user) {
    (void)user;
    browser_app_t *app = g_app;
    if (app == NULL) return 0;

    /* Track cursor + hovered link for status-bar feedback and hover paint. */
    if (ev->kind == RANAL_MOUSE_MOTION || ev->kind == RANAL_MOUSE_BUTTON_DOWN ||
        ev->kind == RANAL_MOUSE_BUTTON_UP) {
        app->mouse_x = ev->x;
        app->mouse_y = ev->y;
        int prev_hover = app->hover_link;
        app->hover_link = -1;
        if (ev->y >= BROWSER_CHROME_HEIGHT &&
            ev->y < BROWSER_CHROME_HEIGHT + app->page_h) {
            int page_x = ev->x;
            int page_y = ev->y - BROWSER_CHROME_HEIGHT;
            for (size_t i = 0; i < app->hits.count; i++) {
                const br_link_rect_t *r = &app->hits.rects[i];
                if (page_x >= r->x && page_x < r->x + r->w &&
                    page_y >= r->y && page_y < r->y + r->h) {
                    app->hover_link = r->link_index;
                    break;
                }
            }
        }
        if (prev_hover != app->hover_link) {
            app->page_dirty = 1;
            app->chrome_dirty = 1;   /* status bar shows hovered link's href */
        }
    }

    if (ev->kind == RANAL_MOUSE_WHEEL) {
        /* Vertical wheel scrolls the page (y > 0 = scroll up). */
        int dy = ev->wheel_y;
        if (dy != 0) {
            app->scroll_y -= dy * 40;
            if (app->scroll_y < 0) app->scroll_y = 0;
            app->page_dirty = 1;
            return 1;
        }
        return 0;
    }

    if (ev->kind != RANAL_MOUSE_BUTTON_DOWN || ev->button != 1) return 0;

    int x = ev->x;
    int y = ev->y;

    /* Click on the URL bar focuses it. */
    if (y < BROWSER_CHROME_HEIGHT) {
        if (hit_url_bar(app, x, y)) {
            focus_url_bar(app);
        } else {
            /* clicking elsewhere on the chrome strip just unfocuses */
            if (app->url_focused) {
                app->url_focused = 0;
                app->chrome_dirty = 1;
            }
        }
        return 1;
    }

    /* Click on the status bar — ignore. */
    if (y >= BROWSER_CHROME_HEIGHT + app->page_h) return 1;

    /* Click on the page: unfocus URL bar, then try to follow a link. */
    if (app->url_focused) {
        app->url_focused = 0;
        app->chrome_dirty = 1;
    }

    int page_x = x;                                 /* page surface is at x=0 */
    int page_y = y - BROWSER_CHROME_HEIGHT;
    for (size_t i = 0; i < app->hits.count; i++) {
        const br_link_rect_t *r = &app->hits.rects[i];
        if (page_x >= r->x && page_x < r->x + r->w &&
            page_y >= r->y && page_y < r->y + r->h) {
            br_app_follow_link(app, r->link_index);
            return 1;
        }
    }
    return 1;
}

void br_input_install(browser_app_t *app) {
    g_app = app;
    ranal_set_key_hook(key_hook, app);
    ranal_set_mouse_hook(mouse_hook, app);
}
