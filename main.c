#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int lm(int argc, char *argv[]);
int ex(int argc, char *argv[]);
int dn(int argc, char *argv[]);
int lk(int argc, char *argv[]);
int rb(int argc, char *argv[]);
int sd(int argc, char *argv[]);
char version[32] = "0.2.4";

void show_help() {
    printf("OneTool %s\n", version);
    printf("usage: %s <tool> [args] -to fd/path\n", "onetool");
    printf("\n");
    printf("available tools:\n");
    printf("  lastmod - print the last modification time of a file\n");
    printf("  exec - execute file (optional: -i interpreter)\n");
    printf("  down - HTTP downloader (curl-like)\n");
    printf("  lmake - run bundled lmake build tool\n");
    printf("  reboot - reboot the system (optional: -t seconds)\n");
    printf("  shutdown - power off the system (optional: -t seconds)\n");
    printf("\n");
    printf("global options:\n");
    printf("  -to fd/path - redirect stdout and stderr to file\n");
}


int main(int argc, char *argv[]) {
    char *tool_argv[argc + 1];
    int tool_argc = 1;
    const char *to_target = NULL;

    if (argc < 2) {
        show_help();
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-to") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for -to\n");
                return 1;
            }
            to_target = argv[++i];
            continue;
        }

        tool_argv[tool_argc++] = argv[i];
    }
    tool_argv[tool_argc] = NULL;

    if (to_target != NULL && strcmp(to_target, "fd") != 0) {
        int out_fd = open(to_target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "open(%s) failed: %s\n", to_target, strerror(errno));
            return 1;
        }
        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
            close(out_fd);
            return 1;
        }
        if (dup2(out_fd, STDERR_FILENO) < 0) {
            fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
            close(out_fd);
            return 1;
        }
        close(out_fd);
    }

    if (strcmp(argv[1], "lastmod") == 0) {
        tool_argv[0] = argv[1];
        return lm(tool_argc, tool_argv);
    }
    if (strcmp(argv[1], "exec") == 0) {
        tool_argv[0] = argv[1];
        return ex(tool_argc, tool_argv);
    }
    if (strcmp(argv[1], "down") == 0) {
        tool_argv[0] = argv[1];
        return dn(tool_argc, tool_argv);
    }
    if (strcmp(argv[1], "lmake") == 0) {
        tool_argv[0] = argv[0];
        return lk(tool_argc, tool_argv);
    }
    if (strcmp(argv[1], "reboot") == 0) {
        tool_argv[0] = argv[1];
        return rb(tool_argc, tool_argv);
    }
    if (strcmp(argv[1], "shutdown") == 0) {
        tool_argv[0] = argv[1];
        return sd(tool_argc, tool_argv);
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "help") == 0 || strcmp(argv[1], "list") == 0) {
        show_help();
        return 0;
    }

    fprintf(stderr, "unknown tool: %s\n", argv[1]);
    fprintf(stderr, "try: %s --help\n", argv[0]);
    return 1;
}
