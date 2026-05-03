#include <stdio.h>
#include <sys/mount.h>

int create_dir(const char *path);
int mount_fs(const char *source, const char *target, const char *fstype, unsigned long mountflags);
void print_status(const char *msg, int success);

void other_setup(void);
void size_setup(void);

#ifndef main
#define main cf
#endif

int main(int argc, char *argv[]) {
    int s;
    (void)argc;
    (void)argv;

    other_setup();
    size_setup();

    s = create_dir("/proc");
    print_status("create /proc", s == 0);
    s = mount_fs("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV);
    print_status("mounting /proc", s == 0);

    s = create_dir("/sys");
    print_status("creating /sys", s == 0);
    s = mount_fs("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV);
    print_status("mounting /sys", s == 0);

    s = create_dir("/dev");
    print_status("creating /dev", s == 0);
    s = mount_fs("devtmpfs", "/dev", "devtmpfs", MS_NOSUID | MS_NOEXEC);
    print_status("mounting /dev", s == 0);

    s = create_dir("/tmp");
    print_status("creating /tmp", s == 0);
    s = mount_fs("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV);
    print_status("mounting /tmp", s == 0);


    return 0;
}
