#include <errno.h>
#include <linux/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int delay = 0;

    if (argc == 1) {
        delay = 0;
    } else if (argc == 3 && strcmp(argv[1], "-t") == 0) {
        char *end = NULL;
        long value = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || value < 0) {
            fprintf(stderr, "invalid timer value: %s\n", argv[2]);
            return 1;
        }
        if (value > 2147483647L) {
            fprintf(stderr, "timer value is too large\n");
            return 1;
        }
        delay = (int)value;
    } else {
        fprintf(stderr, "usage: %s -t seconds(int)\n", argv[0]);
        return 1;
    }

    if (delay > 0) {
        sleep((unsigned int)delay);
    }

    sync();
    if (reboot(LINUX_REBOOT_CMD_RESTART) != 0) {
        fprintf(stderr, "reboot failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}
