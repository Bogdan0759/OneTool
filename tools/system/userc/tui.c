#include "userc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void clip_and_draw(int x, int y, int style, int width, const char *text) {
    char clipped[256];
    if (width <= 0 || text == NULL) return;
    tui_draw_text(x, y, style, tui_clip_text(text, clipped, sizeof(clipped), width));
}

void userc_draw_panel(const userc_list_t *list, userc_view_t *view, int x, int y, int width, int height) {
    int rows = height - 4;
    if (rows <= 0) return;

    if (view->selected < view->scroll) {
        view->scroll = view->selected;
    }
    if (view->selected >= view->scroll + rows) {
        view->scroll = view->selected - rows + 1;
    }
    if (view->scroll < 0) view->scroll = 0;
    if (view->scroll > list->count - rows) view->scroll = list->count - rows;
    if (view->scroll < 0) view->scroll = 0;

    if (view->mode == 0) { // List mode
        char header[256];
        snprintf(header, sizeof(header), " %-16s %5s %5s %-20s %-16s", "USER", "UID", "GID", "HOME", "SHELL");
        clip_and_draw(x, y, TUI_STYLE_ACCENT, width, header);

        for (int i = 0; i < rows; i++) {
            int idx = view->scroll + i;
            if (idx >= list->count) break;
            const userc_user_t *u = &list->users[idx];
            char line[256];
            snprintf(line, sizeof(line), " %-16s %5d %5d %-20s %-16s", u->name, u->uid, u->gid, u->home, u->shell);
            int style = (idx == view->selected) ? TUI_STYLE_SELECTION : TUI_STYLE_NORMAL;
            clip_and_draw(x, y + 1 + i, style, width, line);
        }
        
        tui_draw_text(x, y + height - 2, TUI_STYLE_MUTED, "A: Add | D: Delete | E: Edit | Q: Quit | W/S: Navigate list");
    } else if (view->mode == 1 || view->mode == 2) { // Add/Edit mode
        tui_draw_text(x, y, TUI_STYLE_ACCENT, view->mode == 1 ? "ADD NEW USER" : "EDIT USER");
        
        const char *labels[] = {"Username:", "UID:", "GID:", "Home:", "Shell:"};
        char *values[] = {(char*)view->name_buf, (char*)view->uid_buf, (char*)view->gid_buf, (char*)view->home_buf, (char*)view->shell_buf};
        
        for (int i = 0; i < 5; i++) {
            int style = (i == view->field_idx) ? TUI_STYLE_SELECTION : TUI_STYLE_NORMAL;
            tui_draw_text(x, y + 2 + i*2, TUI_STYLE_NORMAL, labels[i]);
            tui_draw_text(x + 12, y + 2 + i*2, style, values[i][0] ? values[i] : "(empty)");
        }
        
        tui_draw_text(x, y + 13, TUI_STYLE_MUTED, "Enter: Next/Submit | Esc: Cancel | Backspace: Delete char");
    } else if (view->mode == 3) { // Delete confirmation
        tui_draw_text(x, y, TUI_STYLE_ERROR, "DELETE USER?");
        tui_draw_textf(x, y + 2, TUI_STYLE_NORMAL, "Are you sure you want to delete user '%s'?", list->users[view->selected].name);
        tui_draw_text(x, y + 4, TUI_STYLE_SELECTION, " [ YES ] ");
        tui_draw_text(x + 10, y + 4, TUI_STYLE_NORMAL, " [ NO ] ");
        tui_draw_text(x, y + 6, TUI_STYLE_MUTED, "Enter: Confirm | Esc/N: Cancel");
    }
}

int userc_handle_key(userc_list_t *list, userc_view_t *view, int key) {
    if (view->mode == 0) {
        if (key == TUI_KEY_UP || key == 'w' || key == 'W') {
            if (view->selected > 0) {
                view->selected--;
                return 1;
            }
            return 0;
        }
        if (key == TUI_KEY_DOWN || key == 's' || key == 'S') {
            if (view->selected < list->count - 1) {
                view->selected++;
                return 1;
            }
            return 0;
        }
        if (key == 'a' || key == 'A') {
            view->mode = 1;
            view->field_idx = 0;
            memset(view->name_buf, 0, sizeof(view->name_buf));
            memset(view->uid_buf, 0, sizeof(view->uid_buf));
            memset(view->gid_buf, 0, sizeof(view->gid_buf));
            memset(view->home_buf, 0, sizeof(view->home_buf));
            memset(view->shell_buf, 0, sizeof(view->shell_buf));
            return 1;
        }
        if (key == 'd' || key == 'D') {
            if (list->count > 0) view->mode = 3;
            return 1;
        }
        if (key == 'e' || key == 'E') {
            if (list->count > 0) {
                view->mode = 2;
                view->field_idx = 0;
                userc_user_t *u = &list->users[view->selected];
                strncpy(view->name_buf, u->name, 63);
                snprintf(view->uid_buf, 15, "%d", u->uid);
                snprintf(view->gid_buf, 15, "%d", u->gid);
                strncpy(view->home_buf, u->home, 127);
                strncpy(view->shell_buf, u->shell, 63);
            }
            return 1;
        }
    } else if (view->mode == 1 || view->mode == 2) {
        if (key == TUI_KEY_ESCAPE) {
            view->mode = 0;
            return 1;
        }
        if (key == TUI_KEY_ENTER) {
            if (view->field_idx < 4) {
                view->field_idx++;
            } else {
                if (view->mode == 1) {
                    if (userc_add_user(view->name_buf, view->uid_buf, view->gid_buf, view->home_buf, view->shell_buf) == 0) {
                        snprintf(view->status, sizeof(view->status), "User added successfully.");
                    } else {
                        snprintf(view->status, sizeof(view->status), "Failed to add user.");
                    }
                } else {
                    snprintf(view->status, sizeof(view->status), "User edit submitted (mock).");
                }
                userc_load_users(list);
                view->mode = 0;
            }
            return 1;
        }
        if (key == TUI_KEY_BACKSPACE || key == 127) {
            char *buf = NULL;
            if (view->field_idx == 0) buf = view->name_buf;
            else if (view->field_idx == 1) buf = view->uid_buf;
            else if (view->field_idx == 2) buf = view->gid_buf;
            else if (view->field_idx == 3) buf = view->home_buf;
            else if (view->field_idx == 4) buf = view->shell_buf;
            
            if (buf) {
                int len = strlen(buf);
                if (len > 0) buf[len-1] = '\0';
            }
            return 1;
        }
        if (key >= 32 && key <= 126) {
            char *buf = NULL;
            int max = 0;
            if (view->field_idx == 0) { buf = view->name_buf; max = 63; }
            else if (view->field_idx == 1) { buf = view->uid_buf; max = 15; }
            else if (view->field_idx == 2) { buf = view->gid_buf; max = 15; }
            else if (view->field_idx == 3) { buf = view->home_buf; max = 127; }
            else if (view->field_idx == 4) { buf = view->shell_buf; max = 63; }
            
            if (buf) {
                int len = strlen(buf);
                if (len < max) {
                    buf[len] = (char)key;
                    buf[len+1] = '\0';
                }
            }
            return 1;
        }
    } else if (view->mode == 3) {
        if (key == TUI_KEY_ENTER || key == 'y' || key == 'Y') {
            if (userc_del_user(list->users[view->selected].name) == 0) {
                snprintf(view->status, sizeof(view->status), "User deleted.");
            } else {
                snprintf(view->status, sizeof(view->status), "Failed to delete user.");
            }
            userc_load_users(list);
            view->mode = 0;
            if (view->selected >= list->count && list->count > 0) view->selected = list->count - 1;
            return 1;
        }
        if (key == TUI_KEY_ESCAPE || key == 'n' || key == 'N') {
            view->mode = 0;
            return 1;
        }
    }
    return 0;
}

int uc(int argc, char *argv[]) {
    userc_list_t list;
    userc_view_t view;
    tui_event_t event;
    int width, height;

    memset(&view, 0, sizeof(view));
    userc_load_users(&list);
    snprintf(view.status, sizeof(view.status), "User Control: A add, D delete, E edit, Q quit.");

    if (tui_init() != 0) return 1;

    for (;;) {
        tui_get_size(&width, &height);
        tui_begin_frame();
        tui_clear(TUI_STYLE_NORMAL);
        tui_draw_box(0, 0, width, height - 1, TUI_STYLE_PANEL, "User Control");
        userc_draw_panel(&list, &view, 2, 1, width - 4, height - 3);
        tui_draw_status_line(TUI_STYLE_PANEL, view.status);
        tui_end_frame();

        if (!tui_poll_event(&event, 350)) continue;
        if (event.kind != TUI_EVENT_KEY) continue;
        
        if (userc_handle_key(&list, &view, event.key)) continue;
        
        if (event.key == 'q' || event.key == 'Q' || (view.mode == 0 && event.key == TUI_KEY_ESCAPE)) {
            break;
        }
    }

    tui_shutdown();
    return 0;
}
