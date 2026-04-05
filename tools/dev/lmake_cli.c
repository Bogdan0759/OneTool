#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    (void)argc;
    char path[PATH_MAX];
    const char *self = argv[0];
    const char *slash = strrchr(self, '/');

    if (slash != NULL) {
        size_t dir_len = (size_t)(slash - self);
        if (dir_len + 1 + strlen("lmake") + 1 > sizeof(path)) {
            fprintf(stderr, "lmake: launcher path is too long\n");
            return 1;
        }
        memcpy(path, self, dir_len);
        path[dir_len] = '/';
        memcpy(path + dir_len + 1, "lmake", strlen("lmake") + 1);
    } else {
        if (snprintf(path, sizeof(path), "./lmake") >= (int)sizeof(path)) {
            fprintf(stderr, "lmake: launcher path is too long\n");
            return 1;
        }
    }

    argv[0] = "lmake";
    execv(path, argv);
    fprintf(stderr, "lmake: failed to start %s: %s\n", path, strerror(errno));
    return 1;
}
