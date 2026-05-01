#include "taskmng.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

static int read_cpu_line(const char *line, taskmng_cpu_sample_t *sample) {
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;

    if (sscanf(
            line,
            "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
            &user,
            &nice,
            &system,
            &idle,
            &iowait,
            &irq,
            &softirq,
            &steal
        ) < 4) {
        return 0;
    }

    sample->total = user + nice + system + idle + iowait + irq + softirq + steal;
    sample->idle = idle + iowait;
    return 1;
}

static double calculate_cpu_percent(taskmng_cpu_sample_t previous, taskmng_cpu_sample_t current) {
    unsigned long long total_diff = current.total >= previous.total ? current.total - previous.total : 0;
    unsigned long long idle_diff = current.idle >= previous.idle ? current.idle - previous.idle : 0;

    if (total_diff == 0 || idle_diff > total_diff) {
        return 0.0;
    }
    return (double)(total_diff - idle_diff) * 100.0 / (double)total_diff;
}

static void copy_short_text(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

void taskmng_runtime_init(taskmng_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }
    memset(runtime, 0, sizeof(*runtime));
    runtime->ticks_per_second = (int)sysconf(_SC_CLK_TCK);
    if (runtime->ticks_per_second <= 0) {
        runtime->ticks_per_second = 100;
    }
    runtime->page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (runtime->page_size_kb <= 0) {
        runtime->page_size_kb = 4;
    }
}

int taskmng_collect_info(taskmng_runtime_t *runtime, taskmng_snapshot_t *snapshot) {
    FILE *file = NULL;
    char line[512];
    struct utsname uts;
    taskmng_cpu_sample_t aggregate_now = {0, 0};
    taskmng_cpu_sample_t core_now[TASKMNG_MAX_CPUS];
    int core_count = 0;

    if (runtime == NULL || snapshot == NULL) {
        return 0;
    }

    memset(snapshot, 0, offsetof(taskmng_snapshot_t, processes));
    copy_short_text(snapshot->gpu_usage_text, sizeof(snapshot->gpu_usage_text), "N/A");
    copy_short_text(snapshot->gpu_memory_text, sizeof(snapshot->gpu_memory_text), "N/A");

    if (gethostname(snapshot->hostname, sizeof(snapshot->hostname)) != 0) {
        copy_short_text(snapshot->hostname, sizeof(snapshot->hostname), "unknown");
    }
    if (uname(&uts) == 0) {
        snprintf(snapshot->kernel, sizeof(snapshot->kernel), "%s %s", uts.sysname, uts.release);
    } else {
        copy_short_text(snapshot->kernel, sizeof(snapshot->kernel), "unknown");
    }

    file = fopen("/proc/uptime", "r");
    if (file != NULL) {
        fscanf(file, "%lf", &snapshot->uptime_seconds);
        fclose(file);
    }

    file = fopen("/proc/loadavg", "r");
    if (file != NULL) {
        int total_tasks = 0;
        fscanf(
            file,
            "%lf %lf %lf %d/%d",
            &snapshot->loadavg[0],
            &snapshot->loadavg[1],
            &snapshot->loadavg[2],
            &snapshot->running_count,
            &total_tasks
        );
        snapshot->process_count = total_tasks;
        fclose(file);
    }

    file = fopen("/proc/meminfo", "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            if (sscanf(line, "MemTotal: %lu kB", &snapshot->mem_total_kb) == 1) {
                continue;
            }
            if (sscanf(line, "MemAvailable: %lu kB", &snapshot->mem_available_kb) == 1) {
                continue;
            }
            if (sscanf(line, "SwapTotal: %lu kB", &snapshot->swap_total_kb) == 1) {
                continue;
            }
            if (sscanf(line, "SwapFree: %lu kB", &snapshot->swap_free_kb) == 1) {
                continue;
            }
        }
        fclose(file);
    }

    file = fopen("/proc/stat", "r");
    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "cpu ", 4) == 0) {
            if (read_cpu_line(line, &aggregate_now)) {
                snapshot->total_cpu_percent = runtime->initialized
                    ? calculate_cpu_percent(runtime->aggregate_prev, aggregate_now)
                    : 0.0;
                runtime->total_diff = aggregate_now.total >= runtime->aggregate_prev.total
                    ? aggregate_now.total - runtime->aggregate_prev.total
                    : 0;
                runtime->aggregate_prev = aggregate_now;
            }
        } else if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '9') {
            if (core_count < TASKMNG_MAX_CPUS && read_cpu_line(line, &core_now[core_count])) {
                snapshot->cpu_core_percent[core_count] = runtime->initialized
                    ? calculate_cpu_percent(runtime->core_prev[core_count], core_now[core_count])
                    : 0.0;
                runtime->core_prev[core_count] = core_now[core_count];
                core_count++;
            }
        }
    }
    fclose(file);

    snapshot->cpu_core_count = core_count;
    runtime->core_count = core_count;
    runtime->cpu_count = core_count > 0 ? core_count : 1;
    runtime->initialized = 1;
    return 1;
}
