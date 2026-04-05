#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    const char *path = NULL;
    const char *interpreter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "usage: %s path -i interpreter\n", argv[0]);
                return 1;
            }
            interpreter = argv[++i];
            continue;
        }

        if (path == NULL) {
            path = argv[i];
            continue;
        }

        fprintf(stderr, "usage: %s path -i interpreter]\n", argv[0]);
        return 1;
    }

    if (path == NULL) {
        fprintf(stderr, "usage: %s path -i interpreter\n", argv[0]);
        return 1;
    }

    if (interpreter != NULL) {
        char *exec_argv[3];
        exec_argv[0] = (char *)interpreter;
        exec_argv[1] = (char *)path;
        exec_argv[2] = NULL;
        execv(interpreter, exec_argv);
        fprintf(stderr, "exec failed (%s): %s\n", interpreter, strerror(errno));
        return 1;
    }

    char *exec_argv[2];
    exec_argv[0] = (char *)path;
    exec_argv[1] = NULL;
    execv(path, exec_argv);
    fprintf(stderr, "exec failed (%s): %s\n", path, strerror(errno));
    return 1;
}
