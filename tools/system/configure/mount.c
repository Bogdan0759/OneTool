#include <sys/mount.h>
#include <errno.h>
#include <stddef.h>

int mount_fs(const char *source, const char *target, const char *fstype, unsigned long mountflags) {
    if (mount(source, target, fstype, mountflags, NULL) == -1) {
        if (errno != EBUSY) {
            return -1;
        }
    }
    return 0;
}
