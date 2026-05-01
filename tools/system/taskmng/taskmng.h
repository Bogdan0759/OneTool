#ifndef ONETOOL_TOOLS_SYSTEM_TASKMNG_TASKMNG_H
#define ONETOOL_TOOLS_SYSTEM_TASKMNG_TASKMNG_H

#include "../../../libs/TUI/tui.h"

#include <stddef.h>

#define TASKMNG_MAX_PROCESSES 512
#define TASKMNG_MAX_CPUS 16

typedef struct {
    unsigned long long total;
    unsigned long long idle;
} taskmng_cpu_sample_t;

typedef struct {
    int pid;
    unsigned long long total_ticks;
} taskmng_prev_process_t;

typedef struct {
    taskmng_cpu_sample_t aggregate_prev;
    taskmng_cpu_sample_t core_prev[TASKMNG_MAX_CPUS];
    int core_count;
    int cpu_count;
    int initialized;
    int has_snapshot;
    int ticks_per_second;
    long page_size_kb;
    unsigned long long total_diff;
    taskmng_prev_process_t prev_processes[TASKMNG_MAX_PROCESSES];
    int prev_process_count;
} taskmng_runtime_t;

typedef struct {
    int pid;
    int ppid;
    unsigned int uid;
    char user[32];
    char state;
    long priority;
    long nice_value;
    unsigned long virt_kb;
    unsigned long rss_kb;
    double cpu_percent;
    double mem_percent;
    unsigned long long total_ticks;
    char command[160];
} taskmng_process_t;

typedef struct {
    char hostname[64];
    char kernel[96];
    double uptime_seconds;
    double loadavg[3];
    double total_cpu_percent;
    double cpu_core_percent[TASKMNG_MAX_CPUS];
    int cpu_core_count;
    unsigned long mem_total_kb;
    unsigned long mem_available_kb;
    unsigned long swap_total_kb;
    unsigned long swap_free_kb;
    char gpu_usage_text[48];
    char gpu_memory_text[48];
    int process_count;
    int running_count;
    taskmng_process_t processes[TASKMNG_MAX_PROCESSES];
    int process_total;
} taskmng_snapshot_t;

typedef struct {
    int selected;
    int scroll;
    int frozen_pid;
    int filter_editing;
    char filter_text[160];
    char filter_input[160];
} taskmng_view_t;

void taskmng_runtime_init(taskmng_runtime_t *runtime);
void taskmng_view_init(taskmng_view_t *view);
int taskmng_collect_info(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot);
int taskmng_collect_processes(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot);
int taskmng_refresh_processes(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot);
int taskmng_collect(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot);
int taskmng_refresh(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot);
void taskmng_view_move(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int delta);
void taskmng_view_page(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int delta);
void taskmng_view_home(taskmng_view_t *view);
void taskmng_view_end(taskmng_view_t *view, const taskmng_snapshot_t *snapshot);
int taskmng_visible_total(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot);
const taskmng_process_t *taskmng_visible_process_at(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int visible_index);
int taskmng_selected_pid(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot);
int taskmng_selected_process(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot, taskmng_process_t *out);
void taskmng_format_selected_summary(const taskmng_view_t *view, const taskmng_snapshot_t *snapshot, char *dst, size_t dst_size);
int taskmng_send_signal_to_selected(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, int sig, char *status, size_t status_size);
int taskmng_toggle_freeze_selected(taskmng_view_t *view, const taskmng_snapshot_t *snapshot, char *status, size_t status_size);
int taskmng_handle_key(taskmng_runtime_t *runtime, taskmng_view_t *view, taskmng_snapshot_t *snapshot, int key, char *status, size_t status_size);
void taskmng_draw_panel(const taskmng_snapshot_t *snapshot, const taskmng_view_t *view, int x, int y, int width, int height, int compact_mode);

#endif
