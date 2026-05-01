#include "taskmng.h"

#include <stdio.h>
#include <string.h>

static void format_uptime(double seconds, char *dst, size_t dst_size) {
    int days = 0;
    int hours = 0;
    int minutes = 0;
    long total = (long)seconds;

    days = (int)(total / 86400);
    total %= 86400;
    hours = (int)(total / 3600);
    total %= 3600;
    minutes = (int)(total / 60);
    snprintf(dst, dst_size, "%dd %02dh %02dm", days, hours, minutes);
}

static void format_kb(unsigned long kb, char *dst, size_t dst_size) {
    double value = (double)kb;
    const char *unit = "K";

    if (value >= 1024.0) {
        value /= 1024.0;
        unit = "M";
    }
    if (value >= 1024.0) {
        value /= 1024.0;
        unit = "G";
    }
    snprintf(dst, dst_size, "%.1f%s", value, unit);
}

static void build_bar(double percent, int width, char *dst, size_t dst_size) {
    int filled = 0;

    if (width < 1) {
        snprintf(dst, dst_size, "");
        return;
    }
    if (percent < 0.0) {
        percent = 0.0;
    }
    if (percent > 100.0) {
        percent = 100.0;
    }

    filled = (int)((percent / 100.0) * width + 0.5);
    if (filled < 0) {
        filled = 0;
    }
    if (filled > width) {
        filled = width;
    }

    if (dst_size < (size_t)(width + 3)) {
        dst[0] = '\0';
        return;
    }

    dst[0] = '[';
    for (int i = 0; i < width; i++) {
        dst[i + 1] = i < filled ? '|' : '.';
    }
    dst[width + 1] = ']';
    dst[width + 2] = '\0';
}

static void clip_and_draw(int x, int y, int style, int width, const char *text) {
    char clipped[256];

    if (width <= 0 || text == NULL) {
        return;
    }
    tui_draw_text(x, y, style, tui_clip_text(text, clipped, sizeof(clipped), width));
}

static void draw_process_table(const taskmng_snapshot_t *snapshot, const taskmng_view_t *view, int x, int y, int width, int height) {
    int rows = height - 1;
    int command_x = x + 41;
    char line[256];

    if (rows <= 1) {
        return;
    }

    snprintf(line, sizeof(line), " PID   USER      S  CPU%% MEM%%   RSS    COMMAND");
    clip_and_draw(x, y, TUI_STYLE_ACCENT, width, line);

    for (int row = 0; row < rows - 1; row++) {
        const taskmng_process_t *process = taskmng_visible_process_at(view, snapshot, view->scroll + row);
        char rss_text[24];
        char command[192];
        int style = TUI_STYLE_NORMAL;

        if (process == NULL) {
            break;
        }
        format_kb(process->rss_kb, rss_text, sizeof(rss_text));
        snprintf(
            line,
            sizeof(line),
            "%5d %-8.8s %c %5.1f %5.1f %6s",
            process->pid,
            process->user,
            process->state,
            process->cpu_percent,
            process->mem_percent,
            rss_text
        );
        style = view->scroll + row == view->selected ? TUI_STYLE_SELECTION : TUI_STYLE_NORMAL;
        clip_and_draw(x, y + 1 + row, style, width, line);

        snprintf(command, sizeof(command), "%s", process->command);
        clip_and_draw(command_x, y + 1 + row, style, width - (command_x - x), command);
    }
}

static void ensure_visible(taskmng_view_t *view, int visible_rows, int total_rows) {
    if (view->selected < view->scroll) {
        view->scroll = view->selected;
    }
    if (view->selected >= view->scroll + visible_rows) {
        view->scroll = view->selected - visible_rows + 1;
    }
    if (view->scroll < 0) {
        view->scroll = 0;
    }
    if (view->scroll > total_rows - visible_rows) {
        view->scroll = total_rows - visible_rows;
    }
    if (view->scroll < 0) {
        view->scroll = 0;
    }
}

void taskmng_draw_panel(const taskmng_snapshot_t *snapshot, const taskmng_view_t *view_in, int x, int y, int width, int height, int compact_mode) {
    taskmng_view_t local_view;
    char top_line[256];
    char filter_line[256];
    char uptime_text[48];
    char mem_used[32];
    char mem_total[32];
    char mem_bar[32];
    char cpu_bar[32];
    char swap_used[32];
    char swap_total[32];
    char swap_bar[32];
    char selected_summary[160];
    int visible_total = 0;
    int table_y = y + 2;
    int footer_y = y + height - 4;
    int table_height = height - 7;
    int visible_rows = table_height - 1;

    if (snapshot == NULL || view_in == NULL || width < 24 || height < 10) {
        return;
    }

    local_view = *view_in;
    visible_total = taskmng_visible_total(&local_view, snapshot);
    if (visible_total <= 0) {
        local_view.selected = 0;
        local_view.scroll = 0;
    } else if (local_view.selected >= visible_total) {
        local_view.selected = visible_total - 1;
    }
    ensure_visible(&local_view, visible_rows > 1 ? visible_rows - 1 : 1, visible_total);

    format_uptime(snapshot->uptime_seconds, uptime_text, sizeof(uptime_text));
    if (local_view.filter_editing) {
        snprintf(top_line, sizeof(top_line), "Filter: %s", local_view.filter_input);
        clip_and_draw(x, y, TUI_STYLE_SELECTION, width, top_line);
        clip_and_draw(x, y + 1, TUI_STYLE_MUTED, width, "Enter apply | empty Enter clears | Esc cancel");
    } else {
        snprintf(
            top_line,
            sizeof(top_line),
            "Kernel: %s | Host: %s | Uptime: %s | Load: %.2f %.2f %.2f",
            snapshot->kernel,
            snapshot->hostname,
            uptime_text,
            snapshot->loadavg[0],
            snapshot->loadavg[1],
            snapshot->loadavg[2]
        );
        clip_and_draw(x, y, TUI_STYLE_NORMAL, width, top_line);
        if (local_view.filter_text[0] != '\0') {
            snprintf(filter_line, sizeof(filter_line), "Filter: %s | Showing %d of %d", local_view.filter_text, visible_total, snapshot->process_total);
            clip_and_draw(x, y + 1, TUI_STYLE_MUTED, width, filter_line);
        } else {
            snprintf(filter_line, sizeof(filter_line), "Showing %d of %d | G rescans list | H filters", visible_total, snapshot->process_total);
            clip_and_draw(x, y + 1, TUI_STYLE_MUTED, width, filter_line);
        }
    }

    draw_process_table(snapshot, &local_view, x, table_y, width, table_height);
    if (visible_total <= 0) {
        clip_and_draw(x, table_y + 2, TUI_STYLE_MUTED, width, local_view.filter_text[0] != '\0' ? "No processes match the current filter." : "No processes available.");
    }

    taskmng_format_selected_summary(&local_view, snapshot, selected_summary, sizeof(selected_summary));
    clip_and_draw(x, y + height - 5, TUI_STYLE_MUTED, width, selected_summary);

    format_kb(snapshot->mem_total_kb - snapshot->mem_available_kb, mem_used, sizeof(mem_used));
    format_kb(snapshot->mem_total_kb, mem_total, sizeof(mem_total));
    format_kb(snapshot->swap_total_kb - snapshot->swap_free_kb, swap_used, sizeof(swap_used));
    format_kb(snapshot->swap_total_kb, swap_total, sizeof(swap_total));
    build_bar(snapshot->total_cpu_percent, compact_mode ? 10 : 14, cpu_bar, sizeof(cpu_bar));
    build_bar(
        snapshot->mem_total_kb > 0
            ? ((double)(snapshot->mem_total_kb - snapshot->mem_available_kb) * 100.0 / (double)snapshot->mem_total_kb)
            : 0.0,
        compact_mode ? 10 : 14,
        mem_bar,
        sizeof(mem_bar)
    );
    build_bar(
        snapshot->swap_total_kb > 0
            ? ((double)(snapshot->swap_total_kb - snapshot->swap_free_kb) * 100.0 / (double)snapshot->swap_total_kb)
            : 0.0,
        compact_mode ? 10 : 14,
        swap_bar,
        sizeof(swap_bar)
    );

    snprintf(top_line, sizeof(top_line), "CPU  %s %5.1f%%   MEM  %s %s / %s", cpu_bar, snapshot->total_cpu_percent, mem_bar, mem_used, mem_total);
    clip_and_draw(x, footer_y, TUI_STYLE_NORMAL, width, top_line);
    snprintf(top_line, sizeof(top_line), "SWAP %s %s / %s   GPU %s   VRAM %s", swap_bar, swap_used, swap_total, snapshot->gpu_usage_text, snapshot->gpu_memory_text);
    clip_and_draw(x, footer_y + 1, TUI_STYLE_NORMAL, width, top_line);

    if (snapshot->cpu_core_count > 0) {
        int cpu_limit = snapshot->cpu_core_count < 4 ? snapshot->cpu_core_count : 4;
        char core_line[256];
        size_t used = 0;

        core_line[0] = '\0';
        for (int i = 0; i < cpu_limit; i++) {
            int written = snprintf(
                core_line + used,
                sizeof(core_line) - used,
                "%sCPU%d %4.1f%%",
                i == 0 ? "" : " | ",
                i,
                snapshot->cpu_core_percent[i]
            );
            if (written < 0 || (size_t)written >= sizeof(core_line) - used) {
                break;
            }
            used += (size_t)written;
        }
        clip_and_draw(x, footer_y + 2, TUI_STYLE_MUTED, width, core_line);
    }
}

static int taskmng_loop(const char *status_prefix) {
    taskmng_runtime_t runtime;
    taskmng_snapshot_t snapshot;
    taskmng_view_t view;
    tui_event_t event;
    char status[160];
    int width = 0;
    int height = 0;

    taskmng_runtime_init(&runtime);
    taskmng_view_init(&view);
    snprintf(status, sizeof(status), "%s", status_prefix);

    if (tui_init() != 0) {
        fprintf(stderr, "taskmng: failed to initialize TUI\n");
        return 1;
    }

    if (!taskmng_collect(&runtime, &snapshot)) {
        snprintf(status, sizeof(status), "taskmng: failed to read /proc data.");
    }

    for (;;) {
        if (!taskmng_refresh(&runtime, &snapshot)) {
            snprintf(status, sizeof(status), "taskmng: failed to read /proc data.");
        }

        tui_get_size(&width, &height);
        tui_begin_frame();
        tui_clear(TUI_STYLE_NORMAL);
        tui_draw_box(0, 0, width, height - 1, TUI_STYLE_PANEL, "Task Manager");
        taskmng_draw_panel(&snapshot, &view, 2, 1, width - 4, height - 3, 0);
        tui_draw_status_line(TUI_STYLE_PANEL, status);
        tui_end_frame();

        if (!tui_poll_event(&event, 350)) {
            continue;
        }
        if (event.kind != TUI_EVENT_KEY) {
            continue;
        }
        if (taskmng_handle_key(&runtime, &view, &snapshot, event.key, status, sizeof(status))) {
            continue;
        }
        if (event.key == 'q' || event.key == 'Q' || event.key == TUI_KEY_ESCAPE) {
            break;
        }
    }

    tui_shutdown();
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return taskmng_loop("Task manager: W/S move, K kill, t term, F freeze, G rescan, H filter, Q quit.");
}
