#include <stdio.h>
#include <string.h>

int lm(int argc, char *argv[]);
int rb(int argc, char *argv[]);
int sd(int argc, char *argv[]);
char version[32] = "0.2";

void show_help() {
    printf("OneTool %s\n", version);
    printf("usage: %s <tool> [args]\n", "onetool");
    printf("\n");
    printf("available tools:\n");
    printf("  lastmod - print the last modification time of a file\n");
    printf("  reboot - reboot the system (optional: -t seconds)\n");
    printf("  shutdown - power off the system (optional: -t seconds)\n");
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        show_help();
        return 1;
    }

    if (strcmp(argv[1], "lastmod") == 0) {
        return lm(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "reboot") == 0) {
        return rb(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "shutdown") == 0) {
        return sd(argc - 1, argv + 1);
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
