#include "taskmng.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

void taskmng_view_init(taskmng_view_t *view) {
    if (view == NULL) {
        return;
    }
    memset(view, 0, sizeof(*view));
    view->frozen_pid = -1;
}

static int clamp_selected_index(int selected, int count) {
    if (count <= 0) {
        return 0;
    }
    if (selected < 0) {
        return 0;
    }
    if (selected >= count) {
        return count - 1;
    }
    return selected;
}

static void append_char(char *text, size_t text_size, int key) {
    size_t len = strlen(text);

    if (len + 1 < text_size && tui_key_is_printable(key)) {
        text[len] = (char)key;
        text[len + 1] = '\0';
    }
}

static void backspace_char(char *text) {
    size_t len = strlen(text);

    if (len > 0) {
        text[len - 1] = '\0';
    }
}

static int text_contains_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = 0;

    if (needle == NULL || needle[0] == '\0') {
        return 1;
    }
    if (haystack == NULL) {
        return 0;
    }

    needle_len = strlen(needle);
    for (size_t start = 0; haystack[start] != '\0'; start++) {
        size_t i = 0;

        while (i < needle_len && haystack[start + i] != '\0' &&
               tolower((unsigned char)haystack[start + i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

static int process_matches_filter(const taskmng_view_t *view, const taskmng_process_t *process) {
    char pid_text[32];

    if (view == NULL || process == NULL || view->filter_text[0] == '\0') {
        return 1;
    }

    snprintf(pid_text, sizeof(pid_text), "%d", process->pid);
    return text_contains_case_insensitive(pid_text, view->filter_text) ||
           text_contains_case_insensitive(process->command, view->filter_text) ||
           text_contains_case_insensitive(process->user, view->filter_text);
}

int taskmng_visible_total(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot) {
    int total = 0;

    if (view == NULL || snapshot == NULL) {
        return 0;
    }

    for (int i = 0; i < snapshot->process_total; i++) {
        if (process_matches_filter(view, &snapshot->processes[i])) {
            total++;
        }
    }
    return total;
}

const taskmng_process_t *taskmng_visible_process_at(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int visible_index) {
    int current = 0;

    if (view == NULL || snapshot == NULL || visible_index < 0) {
        return NULL;
    }

    for (int i = 0; i < snapshot->process_total; i++) {
        if (!process_matches_filter(view, &snapshot->processes[i])) {
            continue;
        }
        if (current == visible_index) {
            return &snapshot->processes[i];
        }
        current++;
    }
    return NULL;
}

static void clamp_view(taskmng_view_t *view, const taskmng_snapshot_t *snapshot) {
    int visible_total = taskmng_visible_total(view, snapshot);

    if (view == NULL || snapshot == NULL) {
        return;
    }

    view->selected = clamp_selected_index(view->selected, visible_total);
    if (visible_total <= 0) {
        view->selected = 0;
        view->scroll = 0;
    }
}

static void select_pid_if_present(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int pid) {
    int current = 0;

    if (view == NULL || snapshot == NULL || pid < 0) {
        clamp_view(view, snapshot);
        return;
    }

    for (int i = 0; i < snapshot->process_total; i++) {
        if (!process_matches_filter(view, &snapshot->processes[i])) {
            continue;
        }
        if (snapshot->processes[i].pid == pid) {
            view->selected = current;
            clamp_view(view, snapshot);
            return;
        }
        current++;
    }

    clamp_view(view, snapshot);
}

void taskmng_view_move(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int delta) {
    if (view == NULL || snapshot == NULL) {
        return;
    }
    view->selected = clamp_selected_index(view->selected + delta, taskmng_visible_total(view, snapshot));
}

void taskmng_view_page(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int delta) {
    taskmng_view_move(view, snapshot, delta * 12);
}

void taskmng_view_home(taskmng_view_t *view) {
    if (view != NULL) {
        view->selected = 0;
    }
}

void taskmng_view_end(taskmng_view_t *view, const taskmng_snapshot_t *snapshot) {
    int visible_total = 0;

    if (view == NULL || snapshot == NULL) {
        return;
    }
    visible_total = taskmng_visible_total(view, snapshot);
    view->selected = visible_total > 0 ? visible_total - 1 : 0;
}

int taskmng_selected_pid(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot) {
    const taskmng_process_t *process = taskmng_visible_process_at(view, snapshot, view != NULL ? view->selected : 0);

    if (process == NULL) {
        return -1;
    }
    return process->pid;
}

int taskmng_selected_process(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot, taskmng_process_t *out) {
    const taskmng_process_t *process = NULL;

    if (view == NULL || snapshot == NULL || out == NULL) {
        return 0;
    }
    process = taskmng_visible_process_at(view, snapshot, view->selected);
    if (process == NULL) {
        return 0;
    }
    *out = *process;
    return 1;
}

void taskmng_format_selected_summary(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot, char *dst, size_t dst_size) {
    taskmng_process_t process;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (!taskmng_selected_process(view, snapshot, &process)) {
        if (view != NULL && view->filter_text[0] != '\0') {
            snprintf(dst, dst_size, "No process matches filter \"%s\".", view->filter_text);
        } else {
            snprintf(dst, dst_size, "No process selected.");
        }
        return;
    }

    snprintf(
        dst,
        dst_size,
        "PID %d | user %s | state %c | cpu %.1f%% | mem %.1f%% | rss %luK",
        process.pid,
        process.user,
        process.state,
        process.cpu_percent,
        process.mem_percent,
        process.rss_kb
    );
}

int taskmng_send_signal_to_selected(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int sig, char *status, size_t status_size) {
    taskmng_process_t process;

    if (!taskmng_selected_process(view, snapshot, &process)) {
        snprintf(status, status_size, "No process selected.");
        return 0;
    }

    if (kill(process.pid, sig) != 0) {
        snprintf(status, status_size, "Signal %d failed for PID %d.", sig, process.pid);
        return 0;
    }

    if (sig == SIGKILL) {
        snprintf(status, status_size, "Killed PID %d (%s).", process.pid, process.command);
    } else if (sig == SIGTERM) {
        snprintf(status, status_size, "Sent TERM to PID %d (%s).", process.pid, process.command);
    } else if (sig == SIGSTOP) {
        snprintf(status, status_size, "Froze PID %d (%s).", process.pid, process.command);
    } else if (sig == SIGCONT) {
        snprintf(status, status_size, "Continued PID %d (%s).", process.pid, process.command);
    } else {
        snprintf(status, status_size, "Sent signal %d to PID %d.", sig, process.pid);
    }
    return 1;
}

int taskmng_toggle_freeze_selected(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, char *status, size_t status_size) {
    int pid = taskmng_selected_pid(view, snapshot);

    if (pid < 0) {
        snprintf(status, status_size, "No process selected.");
        return 0;
    }

    if (view->frozen_pid == pid) {
        if (kill(pid, SIGCONT) != 0) {
            snprintf(status, status_size, "Failed to continue PID %d.", pid);
            return 0;
        }
        view->frozen_pid = -1;
        snprintf(status, status_size, "Continued PID %d.", pid);
        return 1;
    }

    if (view->frozen_pid > 0) {
        kill(view->frozen_pid, SIGCONT);
    }
    if (kill(pid, SIGSTOP) != 0) {
        snprintf(status, status_size, "Failed to freeze PID %d.", pid);
        return 0;
    }
    view->frozen_pid = pid;
    snprintf(status, status_size, "Froze PID %d.", pid);
    return 1;
}

int taskmng_handle_key(taskmng_runtime_t *runtime, taskmng_view_t *view, taskmng_snapshot_t *snapshot, int key, char *status, size_t status_size) {
    if (view == NULL || snapshot == NULL) {
        return 0;
    }

    if (view->filter_editing) {
        if (key == TUI_KEY_ESCAPE) {
            view->filter_editing = 0;
            view->filter_input[0] = '\0';
            snprintf(status, status_size, "Filter input cancelled.");
            return 1;
        }
        if (key == TUI_KEY_BACKSPACE) {
            backspace_char(view->filter_input);
            return 1;
        }
        if (key == TUI_KEY_ENTER) {
            snprintf(view->filter_text, sizeof(view->filter_text), "%s", view->filter_input);
            view->filter_input[0] = '\0';
            view->filter_editing = 0;
            view->selected = 0;
            view->scroll = 0;
            clamp_view(view, snapshot);
            if (view->filter_text[0] == '\0') {
                snprintf(status, status_size, "Filter cleared.");
            } else {
                snprintf(status, status_size, "Filter applied: %s", view->filter_text);
            }
            return 1;
        }
        if (tui_key_is_printable(key)) {
            append_char(view->filter_input, sizeof(view->filter_input), key);
            return 1;
        }
        return 1;
    }

    switch (key) {
        case 'w':
        case 'W':
        case TUI_KEY_UP:
            taskmng_view_move(view, snapshot, -1);
            return 1;
        case 's':
        case 'S':
        case TUI_KEY_DOWN:
            taskmng_view_move(view, snapshot, 1);
            return 1;
        case TUI_KEY_PAGE_UP:
            taskmng_view_page(view, snapshot, -1);
            return 1;
        case TUI_KEY_PAGE_DOWN:
            taskmng_view_page(view, snapshot, 1);
            return 1;
        case TUI_KEY_HOME:
            taskmng_view_home(view);
            return 1;
        case TUI_KEY_END:
            taskmng_view_end(view, snapshot);
            return 1;
        case 'k':
        case 'K':
            taskmng_send_signal_to_selected(view, snapshot, SIGKILL, status, status_size);
            return 1;
        case 't':
            taskmng_send_signal_to_selected(view, snapshot, SIGTERM, status, status_size);
            return 1;
        case 'f':
        case 'F':
            taskmng_toggle_freeze_selected(view, snapshot, status, status_size);
            return 1;
        case 'g':
        case 'G': {
            int selected_pid = taskmng_selected_pid(view, snapshot);

            if (runtime == NULL || !taskmng_collect(runtime, snapshot)) {
                snprintf(status, status_size, "Full process rescan failed.");
                return 1;
            }

            select_pid_if_present(view, snapshot, selected_pid);
            snprintf(status, status_size, "Process list rescanned.");
            return 1;
        }
        case 'h':
        case 'H':
            view->filter_editing = 1;
            view->filter_input[0] = '\0';
            snprintf(status, status_size, "Type PID/name/path and press Enter. Empty input clears filter.");
            return 1;
        case TUI_KEY_ESCAPE:
            return 0;
        default:
            return 0;
    }
}
