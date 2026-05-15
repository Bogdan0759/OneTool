// ls tool: list directory entries
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void help(const char *tool) {
    printf("ls - list files\n");
    printf("usage: %s [-a] [-l] [-h] [path...]\n", tool);
    printf("options:\n");
    printf("  -a, --all     show hidden files\n");
    printf("  -l, --long    long listing\n");
    printf("  -h, --human   human readable sizes with -l\n");
    printf("  --help        show help\n");
}

static void mode_string(mode_t mode, char out[11]) {
    out[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : S_ISCHR(mode) ? 'c' :
             S_ISBLK(mode) ? 'b' : S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static void format_size(uintmax_t bytes, int human, char *out, size_t out_size) {
    static const char *units[] = {"B", "K", "M", "G", "T", "P"};
    double value = (double)bytes;
    size_t unit = 0;

    if (!human) {
        snprintf(out, out_size, "%" PRIuMAX, bytes);
        return;
    }
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        snprintf(out, out_size, "%" PRIuMAX "B", bytes);
    } else {
        snprintf(out, out_size, "%.1f%s", value, units[unit]);
    }
}

static int print_long_stat(const char *display_name, const struct stat *st, int human) {
    char mode[11];
    char size[32];
    char timebuf[32];
    struct passwd *pw;
    struct group *gr;
    struct tm tmv;

    mode_string(st->st_mode, mode);
    format_size((uintmax_t)st->st_size, human, size, sizeof(size));
    pw = getpwuid(st->st_uid);
    gr = getgrgid(st->st_gid);
    if (localtime_r(&st->st_mtime, &tmv) != NULL) {
        strftime(timebuf, sizeof(timebuf), "%b %d %H:%M", &tmv);
    } else {
        snprintf(timebuf, sizeof(timebuf), "?");
    }

    printf("%s %3ju %-8s %-8s %8s %s %s\n",
           mode,
           (uintmax_t)st->st_nlink,
           pw ? pw->pw_name : "?",
           gr ? gr->gr_name : "?",
           size,
           timebuf,
           display_name);
    return 0;
}

static int print_long_entry(const char *path, const char *name, int human) {
    struct stat st;
    char fullpath[4096];

    if (snprintf(fullpath, sizeof(fullpath), "%s/%s", path, name) >= (int)sizeof(fullpath)) {
        fprintf(stderr, "ls: path too long: %s/%s\n", path, name);
        return 1;
    }
    if (lstat(fullpath, &st) != 0) {
        fprintf(stderr, "ls: %s: %s\n", fullpath, strerror(errno));
        return 1;
    }
    return print_long_stat(name, &st, human);
}

static int list_dir(const char *path, int all, int long_mode, int human) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    int rc = 0;

    if (dir == NULL) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (!all && entry->d_name[0] == '.') {
            continue;
        }
        if (long_mode) {
            if (print_long_entry(path, entry->d_name, human) != 0) {
                rc = 1;
            }
        } else {
            printf("%s\n", entry->d_name);
        }
    }
    closedir(dir);
    return rc;
}

static int list_path(const char *path, int all, int long_mode, int human) {
    struct stat st;

    if (lstat(path, &st) != 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        return list_dir(path, all, long_mode, human);
    }
    if (long_mode) {
        return print_long_stat(path, &st, human);
    }
    printf("%s\n", path);
    return 0;
}

int main(int argc, char *argv[]) {
    int all = 0;
    int long_mode = 0;
    int human = 0;
    int saw_path = 0;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            help(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-a") == 0 || strcmp(arg, "--all") == 0) {
            all = 1;
            continue;
        }
        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--long") == 0) {
            long_mode = 1;
            continue;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--human") == 0) {
            human = 1;
            continue;
        }
        if (arg[0] == '-') {
            fprintf(stderr, "ls: unknown option %s\n", arg);
            return 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (arg[0] == '-') {
            continue;
        }
        saw_path = 1;
        if (list_path(arg, all, long_mode, human) != 0) {
            rc = 1;
        }
    }

    if (!saw_path) {
        rc = list_path(".", all, long_mode, human);
    }
    return rc;
}
