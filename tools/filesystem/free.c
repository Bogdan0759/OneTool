// free tool: print memory usage from /proc/meminfo
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t total;
    uint64_t free;
    uint64_t available;
    uint64_t buffers;
    uint64_t cached;
    uint64_t swap_total;
    uint64_t swap_free;
} meminfo_t;

static void help(const char *tool) {
    printf("free - show memory usage\n");
    printf("usage: %s [-b|-k|-m|-g|-h]\n", tool);
    printf("options:\n");
    printf("  -b       bytes\n");
    printf("  -k       KiB (default)\n");
    printf("  -m       MiB\n");
    printf("  -g       GiB\n");
    printf("  -h       human readable\n");
    printf("  --help   show help\n");
}

static int read_meminfo(meminfo_t *info) {
    FILE *fp = fopen("/proc/meminfo", "r");
    char key[64];
    char unit[16];
    uint64_t value;

    if (fp == NULL) {
        perror("free: /proc/meminfo");
        return 1;
    }
    memset(info, 0, sizeof(*info));
    while (fscanf(fp, "%63[^:]: %" SCNu64 " %15s\n", key, &value, unit) >= 2) {
        value *= 1024; 
        if (strcmp(key, "MemTotal") == 0) info->total = value;
        else if (strcmp(key, "MemFree") == 0) info->free = value;
        else if (strcmp(key, "MemAvailable") == 0) info->available = value;
        else if (strcmp(key, "Buffers") == 0) info->buffers = value;
        else if (strcmp(key, "Cached") == 0) info->cached = value;
        else if (strcmp(key, "SwapTotal") == 0) info->swap_total = value;
        else if (strcmp(key, "SwapFree") == 0) info->swap_free = value;
    }
    fclose(fp);
    return info->total == 0 ? 1 : 0;
}

static void format_value(uint64_t bytes, uint64_t divisor, int human, char *out, size_t out_size) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    size_t unit = 0;

    if (!human) {
        snprintf(out, out_size, "%" PRIu64, bytes / divisor);
        return;
    }
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) snprintf(out, out_size, "%" PRIu64 "B", bytes);
    else snprintf(out, out_size, "%.1f%s", value, units[unit]);
}

static void print_row(const char *name, uint64_t total, uint64_t used, uint64_t free_bytes,
                      uint64_t shared, uint64_t buff_cache, uint64_t available,
                      uint64_t divisor, int human) {
    char values[6][32];
    uint64_t raw[] = {total, used, free_bytes, shared, buff_cache, available};

    for (int i = 0; i < 6; i++) {
        format_value(raw[i], divisor, human, values[i], sizeof(values[i]));
    }
    printf("%-7s %12s %12s %12s %12s %12s %12s\n",
           name, values[0], values[1], values[2], values[3], values[4], values[5]);
}

int main(int argc, char *argv[]) {
    meminfo_t info;
    uint64_t divisor = 1024;
    int human = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-b") == 0) {
            divisor = 1;
        } else if (strcmp(argv[i], "-k") == 0) {
            divisor = 1024;
        } else if (strcmp(argv[i], "-m") == 0) {
            divisor = 1024 * 1024;
        } else if (strcmp(argv[i], "-g") == 0) {
            divisor = 1024 * 1024 * 1024ULL;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--human") == 0) {
            human = 1;
        } else {
            fprintf(stderr, "error: unknown option %s\n", argv[i]);
            return 1;
        }
    }

    if (read_meminfo(&info) != 0) {
        fprintf(stderr, "error: failed to read memory info\n");
        return 1;
    }

    uint64_t buff_cache = info.buffers + info.cached;
    uint64_t used = info.total > info.free + buff_cache ? info.total - info.free - buff_cache : 0;
    uint64_t swap_used = info.swap_total > info.swap_free ? info.swap_total - info.swap_free : 0;

    printf("%-7s %12s %12s %12s %12s %12s %12s\n", "", "total", "used", "free", "shared", "buff/cache", "available");
    print_row("Mem:", info.total, used, info.free, 0, buff_cache, info.available, divisor, human);
    print_row("Swap:", info.swap_total, swap_used, info.swap_free, 0, 0, 0, divisor, human);
    return 0;
}
