#include <sys/stat.h>
#include <errno.h>

int create_dir(const char *path) {
    if (mkdir(path, 0755) == -1) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}
