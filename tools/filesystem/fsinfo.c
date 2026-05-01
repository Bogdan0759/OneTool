// fsinfo tool: print the filesystem info
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <mntent.h>
#include <limits.h>

int main(int argc, char *argv[]) {
    printf("This tool is the part of OneTool project that published in MPL-2.0 license\n");

    if (argc != 2) {
        fprintf(stderr, "usage: %s <path>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    struct statvfs vfs;

    if (statvfs(path, &vfs) == -1) {
        perror("statvfs");
        return 1;
    }

    FILE *mtab = setmntent("/proc/mounts", "r");
    char best_fsname[256] = "unknown";
    char best_fstype[256] = "unknown";
    size_t max_match_len = 0;

    if (mtab) {
        char resolved_path[PATH_MAX];
        if (realpath(path, resolved_path)) {
            struct mntent *ent;
            while ((ent = getmntent(mtab)) != NULL) {
                size_t dir_len = strlen(ent->mnt_dir);
                if (strncmp(resolved_path, ent->mnt_dir, dir_len) == 0) {
                    if (ent->mnt_dir[dir_len - 1] == '/' || resolved_path[dir_len] == '/' || resolved_path[dir_len] == '\0') {
                        if (dir_len > max_match_len) {
                            max_match_len = dir_len;
                            strncpy(best_fsname, ent->mnt_fsname, sizeof(best_fsname) - 1);
                            strncpy(best_fstype, ent->mnt_type, sizeof(best_fstype) - 1);
                        }
                    }
                }
            }
        }
        endmntent(mtab);
    }

    unsigned long long inodes_total = vfs.f_files;
    unsigned long long inodes_available = vfs.f_favail;
    unsigned long long inodes_free = vfs.f_ffree;
    unsigned long long inodes_used = inodes_total - inodes_free;
    unsigned long long inodes_usage = 0;

    if (inodes_total > 0) {
        inodes_usage = (inodes_used * 100) / inodes_total;
    }

    printf("fs name: %s\n", best_fstype);
    printf("inodes total: %llu\n", inodes_total);
    printf("inodes used: %llu\n", inodes_used);
    printf("inodes available: %llu\n", inodes_available);
    printf("inodes usage: %llu%%\n", inodes_usage);
    printf("inodes name: %s\n", best_fsname);

    return 0;
}