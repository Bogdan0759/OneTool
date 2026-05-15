// fsize tool: print regular file sizes
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static void help(const char *tool) {
    printf("fsize - print file size\n");
    printf("usage: %s -h file\n", tool);
    printf("options:\n");
    printf("  -h, --human   human readable output\n");
    printf("  --help        show help\n");
}

static void format_human(uint64_t bytes, char *out, size_t out_size) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TiB", "PB"};
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0) {
        snprintf(out, out_size, "%" PRIu64 " B", bytes);
    } else {
        snprintf(out, out_size, "%.1f %s", value, units[unit]);
    }
}

static int print_file_size(const char *path, int human) {
    struct stat st;

    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: %s %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: %s is not regular file\n", path);
        return 1;
    }

    if (human) {
        char buf[64];
        format_human((uint64_t)st.st_size, buf, sizeof(buf));
        printf("%s %s\n", buf, path);
    } else {
        printf("%" PRIu64 " %s\n", (uint64_t)st.st_size, path);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int human = 0;
    int saw_file = 0;
    int rc = 0;

    if (argc == 1) {
        fprintf(stderr, "usage: %s -h file\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0) {
            help(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--human") == 0) {
            human = 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--human") == 0) {
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "error: unknow option %s\n", arg);
            return 1;
        }

        saw_file = 1;
        if (print_file_size(arg, human) != 0) {
            rc = 1;
        }
    }

    if (!saw_file) {
        fprintf(stderr, "usage: %s -h file\n", argv[0]);
        return 1;
    }
    return rc;
}
