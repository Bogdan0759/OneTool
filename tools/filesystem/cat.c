// cat tool: print files
#include <errno.h>
#include <stdio.h>
#include <string.h>

static void help(const char *tool) {
    printf("cat - print files content\n");
    printf("usage: %s -n file\n", tool);
    printf("options:\n");
    printf("  -n, --number  number output lines\n");
    printf("  --help        show help\n");
}

static int print_stream(FILE *fp, const char *name, int number, int *line_no) {
    int ch;
    int at_line_start = 1;

    while ((ch = fgetc(fp)) != EOF) {
        if (number && at_line_start) {
            printf("%6d\t", (*line_no)++);
        }
        putchar(ch);
        at_line_start = ch == '\n';
    }
    if (ferror(fp)) {
        fprintf(stderr, "%s: read error\n", name);
        return 1;
    }
    return 0;
}

static int print_file(const char *path, int number, int *line_no) {
    FILE *fp;
    int rc;

    if (strcmp(path, "-") == 0) {
        return print_stream(stdin, "stdin", number, line_no);
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: %s: %s\n", path, strerror(errno));
        return 1;
    }
    rc = print_stream(fp, path, number, line_no);
    fclose(fp);
    return rc;
}

int main(int argc, char *argv[]) {
    int number = 0;
    int saw_file = 0;
    int line_no = 1;
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            help(argv[0]);
            return 0;
        }
        if (strcmp(arg, "-n") == 0 || strcmp(arg, "--number") == 0) {
            number = 1;
            continue;
        }
        if (arg[0] == '-' && strcmp(arg, "-") != 0) {
            fprintf(stderr, "error: unknown option %s\n", arg);
            return 1;
        }
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-n") == 0 || strcmp(arg, "--number") == 0) {
            continue;
        }
        saw_file = 1;
        if (print_file(arg, number, &line_no) != 0) {
            rc = 1;
        }
    }

    if (!saw_file) {
        rc = print_stream(stdin, "stdin", number, &line_no);
    }
    return rc;
}
