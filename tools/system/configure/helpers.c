#include <stdio.h>
#include <errno.h>
#include <string.h>

void print_status(const char *msg, int success) {
    if (success) {
        printf("[OK] %s\n", msg);
    } else {
        printf("[FAIL] %s: %s\n", msg, strerror(errno));
    }
}
