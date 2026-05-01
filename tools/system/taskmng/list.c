#include "taskmng.h"

#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_pid_name(const char *name) {
    if (name == NULL || *name == '\0') {
        return 0;
    }
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if (!isdigit((unsigned char)*cursor)) {
            return 0;
        }
    }
    return 1;
}

static unsigned long long find_previous_ticks(const taskmng_runtime_t *runtime, int pid) {
    for (int i = 0; i < runtime->prev_process_count; i++) {
        if (runtime->prev_processes[i].pid == pid) {
            return runtime->prev_processes[i].total_ticks;
        }
    }
    return 0;
}

static void remember_process_ticks(taskmng_runtime_t *runtime, int pid, unsigned long long total_ticks) {
    if (runtime->prev_process_count >= TASKMNG_MAX_PROCESSES) {
        return;
    }
    runtime->prev_processes[runtime->prev_process_count].pid = pid;
    runtime->prev_processes[runtime->prev_process_count].total_ticks = total_ticks;
    runtime->prev_process_count++;
}

static int read_uid_from_status(int pid, unsigned int *uid_out) {
    char path[64];
    char line[256];
    FILE *file = NULL;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (sscanf(line, "Uid: %u", uid_out) == 1) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static void read_username(unsigned int uid, char *dst, size_t dst_size) {
    struct passwd *pwd = getpwuid((uid_t)uid);

    if (pwd != NULL && pwd->pw_name != NULL) {
        snprintf(dst, dst_size, "%s", pwd->pw_name);
        return;
    }
    snprintf(dst, dst_size, "%u", uid);
}

static void read_command_line(int pid, const char *fallback, char *dst, size_t dst_size) {
    char path[64];
    FILE *file = NULL;
    size_t size = 0;

    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    file = fopen(path, "r");
    if (file == NULL) {
        snprintf(dst, dst_size, "%s", fallback);
        return;
    }

    size = fread(dst, 1, dst_size - 1, file);
    fclose(file);

    if (size == 0) {
        snprintf(dst, dst_size, "%s", fallback);
        return;
    }

    for (size_t i = 0; i + 1 < size; i++) {
        if (dst[i] == '\0') {
            dst[i] = ' ';
        }
    }
    dst[size] = '\0';

    while (size > 0 && (dst[size - 1] == '\0' || dst[size - 1] == ' ')) {
        dst[size - 1] = '\0';
        size--;
    }
    if (dst[0] == '\0') {
        snprintf(dst, dst_size, "%s", fallback);
    }
}

static int parse_process_entry(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot, int pid, taskmng_process_t *process) {
    char path[64];
    char buffer[4096];
    char command_name[160];
    char *lparen = NULL;
    char *rparen = NULL;
    char *rest = NULL;
    FILE *file = NULL;
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    unsigned long long starttime = 0;
    long rss_pages = 0;
    unsigned long vsize = 0;
    unsigned int uid = 0;
    unsigned long long previous_ticks = 0;
    unsigned long long total_ticks = 0;
    int field_count = 0;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    if (fgets(buffer, sizeof(buffer), file) == NULL) {
        fclose(file);
        return 0;
    }
    fclose(file);

    lparen = strchr(buffer, '(');
    rparen = strrchr(buffer, ')');
    if (lparen == NULL || rparen == NULL || rparen <= lparen) {
        return 0;
    }

    snprintf(command_name, sizeof(command_name), "%.*s", (int)(rparen - lparen - 1), lparen + 1);
    rest = rparen + 2;
    field_count = sscanf(
        rest,
        "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*llu %*llu %ld %ld %*ld %*ld %llu %lu %ld",
        &process->state,
        &process->ppid,
        &utime,
        &stime,
        &process->priority,
        &process->nice_value,
        &starttime,
        &vsize,
        &rss_pages
    );
    if (field_count < 9) {
        return 0;
    }

    process->pid = pid;
    process->uid = 0;
    if (read_uid_from_status(pid, &uid)) {
        process->uid = uid;
    }
    read_username(process->uid, process->user, sizeof(process->user));
    process->virt_kb = vsize / 1024UL;
    process->rss_kb = rss_pages > 0 ? (unsigned long)rss_pages * (unsigned long)runtime->page_size_kb : 0;
    process->mem_percent = snapshot->mem_total_kb > 0
        ? (double)process->rss_kb * 100.0 / (double)snapshot->mem_total_kb
        : 0.0;

    total_ticks = utime + stime;
    process->total_ticks = total_ticks;
    previous_ticks = find_previous_ticks(runtime, pid);
    if (runtime->total_diff > 0 && previous_ticks > 0 && total_ticks >= previous_ticks) {
        process->cpu_percent = (double)(total_ticks - previous_ticks) * 100.0 * (double)runtime->cpu_count / (double)runtime->total_diff;
    } else {
        process->cpu_percent = 0.0;
    }

    read_command_line(pid, command_name, process->command, sizeof(process->command));
    if (process->command[0] == '\0') {
        snprintf(process->command, sizeof(process->command), "%s", command_name);
    }

    (void)starttime;
    return 1;
}

static void mark_process_unavailable(taskmng_process_t *process) {
    if (process == NULL) {
        return;
    }

    process->state = 'X';
    process->virt_kb = 0;
    process->rss_kb = 0;
    process->cpu_percent = 0.0;
    process->mem_percent = 0.0;
    process->total_ticks = 0;
}

static int compare_processes(const void *left_ptr, const void *right_ptr) {
    const taskmng_process_t *left = (const taskmng_process_t *)left_ptr;
    const taskmng_process_t *right = (const taskmng_process_t *)right_ptr;

    if (left->cpu_percent < right->cpu_percent) return 1;
    if (left->cpu_percent > right->cpu_percent) return -1;
    if (left->mem_percent < right->mem_percent) return 1;
    if (left->mem_percent > right->mem_percent) return -1;
    if (left->rss_kb < right->rss_kb) return 1;
    if (left->rss_kb > right->rss_kb) return -1;
    return left->pid - right->pid;
}

int taskmng_collect_processes(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot) {
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    taskmng_prev_process_t new_prev[TASKMNG_MAX_PROCESSES];
    int new_prev_count = 0;

    if (runtime == NULL || snapshot == NULL) {
        return 0;
    }

    dir = opendir("/proc");
    if (dir == NULL) {
        return 0;
    }

    snapshot->process_total = 0;
    snapshot->running_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        taskmng_process_t process;

        if (!is_pid_name(entry->d_name)) {
            continue;
        }
        if (!parse_process_entry(runtime, snapshot, atoi(entry->d_name), &process)) {
            continue;
        }

        if (process.state == 'R') {
            snapshot->running_count++;
        }

        if (snapshot->process_total < TASKMNG_MAX_PROCESSES) {
            snapshot->processes[snapshot->process_total++] = process;
        }
        if (new_prev_count < TASKMNG_MAX_PROCESSES) {
            new_prev[new_prev_count].pid = process.pid;
            new_prev[new_prev_count].total_ticks = process.total_ticks;
            new_prev_count++;
        }
    }
    closedir(dir);

    qsort(snapshot->processes, (size_t)snapshot->process_total, sizeof(snapshot->processes[0]), compare_processes);

    runtime->prev_process_count = 0;
    for (int i = 0; i < new_prev_count; i++) {
        remember_process_ticks(runtime, new_prev[i].pid, new_prev[i].total_ticks);
    }
    return 1;
}

int taskmng_refresh_processes(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot) {
    taskmng_prev_process_t new_prev[TASKMNG_MAX_PROCESSES];
    int new_prev_count = 0;

    if (runtime == NULL || snapshot == NULL) {
        return 0;
    }

    snapshot->running_count = 0;
    for (int i = 0; i < snapshot->process_total; i++) {
        taskmng_process_t updated = snapshot->processes[i];

        if (!parse_process_entry(runtime, snapshot, snapshot->processes[i].pid, &updated)) {
            mark_process_unavailable(&snapshot->processes[i]);
            continue;
        }

        snapshot->processes[i] = updated;
        if (updated.state == 'R') {
            snapshot->running_count++;
        }
        if (new_prev_count < TASKMNG_MAX_PROCESSES) {
            new_prev[new_prev_count].pid = updated.pid;
            new_prev[new_prev_count].total_ticks = updated.total_ticks;
            new_prev_count++;
        }
    }

    runtime->prev_process_count = 0;
    for (int i = 0; i < new_prev_count; i++) {
        remember_process_ticks(runtime, new_prev[i].pid, new_prev[i].total_ticks);
    }
    return 1;
}

int taskmng_collect(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot) {
    if (!taskmng_collect_info(runtime, snapshot)) {
        return 0;
    }
    if (!taskmng_collect_processes(runtime, snapshot)) {
        return 0;
    }
    runtime->has_snapshot = 1;
    return 1;
}

int taskmng_refresh(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot) {
    if (runtime == NULL || snapshot == NULL) {
        return 0;
    }
    if (!runtime->has_snapshot) {
        return taskmng_collect(runtime, snapshot);
    }
    if (!taskmng_collect_info(runtime, snapshot)) {
        return 0;
    }
    if (!taskmng_refresh_processes(runtime, snapshot)) {
        return 0;
    }
    return 1;
}
