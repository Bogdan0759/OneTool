#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LPACK_MAGIC "LPACK01"
typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t flags;
    uint64_t script_offset;
    uint64_t script_size;
    uint64_t interp_offset;
    uint64_t interp_size;
    uint32_t interp_mode;
} lpack_footer_t;

typedef struct {
    const char *input_script;
    const char *output_file;
    const char *interpreter_file;
} lpack_opts_t;

static void lpack_help(const char *tool) {
    printf("usage: %s file.lua -i interpreter> ptions\n", tool);
    printf("options:\n");
    printf("  -i interpreter   path to lua intewpreter\n");
    printf("  -o output         output ELF file\n");
    printf("  -h, --help          show this help\n");
}

static int parse_args(int argc, char *argv[], lpack_opts_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->output_file = "a.out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            lpack_help(argv[0]);
            return 2;
        }
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for -o\n");
                return 1;
            }
            opts->output_file = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "-i") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for -i\n");
                return 1;
            }
            opts->interpreter_file = argv[++i];
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return 1;
        }
        if (opts->input_script != NULL) {
            fprintf(stderr, "only one input file is allowed\n");
            return 1;
        }
        opts->input_script = argv[i];
    }

    if (opts->input_script == NULL) {
        fprintf(stderr, "input file is required\n");
        return 1;
    }
    if (opts->interpreter_file == NULL) {
        fprintf(stderr, "-i <interpreter> is required\n");
        return 1;
    }
    return 0;
}

static int file_size_and_mode(const char *path, uint64_t *size_out, mode_t *mode_out) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return 1;
    }
    *size_out = (uint64_t)st.st_size;
    *mode_out = st.st_mode;
    return 0;
}

static int copy_append_file(int dst_fd, const char *src_path, uint64_t *bytes_written) {
    int src_fd = -1;
    char buf[8192];
    uint64_t total = 0;

    src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        return 1;
    }

    for (;;) {
        ssize_t n = read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(src_fd);
            return 1;
        }
        if (n == 0) {
            break;
        }

        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t wn = write(dst_fd, buf + off, (size_t)n - off);
            if (wn < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(src_fd);
                return 1;
            }
            off += (size_t)wn;
        }
        total += (uint64_t)n;
    }

    close(src_fd);
    *bytes_written = total;
    return 0;
}

int main(int argc, char *argv[]) {
    lpack_opts_t opts;
    int rc;
    char runtime_bin[2048];
    const char *self = argv[0];
    const char *slash = strrchr(self, '/');
    int out_fd = -1;
    uint64_t base_size = 0;
    lpack_footer_t footer;
    uint64_t written = 0;
    uint64_t interp_size = 0;
    mode_t interp_mode = 0755;

    rc = parse_args(argc, argv, &opts);
    if (rc == 2) {
        return 0;
    }
    if (rc != 0) {
        return 1;
    }

    if (access("tools/dev/packagers/lpack/runtime.bin", R_OK) == 0) {
        snprintf(runtime_bin, sizeof(runtime_bin), "tools/dev/packagers/lpack/runtime.bin");
    } else if (access("./tools/dev/packagers/lpack/runtime.bin", R_OK) == 0) {
        snprintf(runtime_bin, sizeof(runtime_bin), "./tools/dev/packagers/lpack/runtime.bin");
    } else if (slash != NULL) {
        size_t dir_len = (size_t)(slash - self);
        if (dir_len + strlen("/tools/dev/packagers/lpack/runtime.bin") + 1 > sizeof(runtime_bin)) {
            fprintf(stderr, "path too long\n");
            return 1;
        }
        memcpy(runtime_bin, self, dir_len);
        runtime_bin[dir_len] = '\0';
        strcat(runtime_bin, "/tools/dev/packagers/lpack/runtime.bin");
    } else {
        fprintf(stderr, "cannot locate runtime.bin\n");
        return 1;
    }

    if (file_size_and_mode(opts.interpreter_file, &interp_size, &interp_mode) != 0) {
        fprintf(stderr, "cant stat interpreter '%s': %s\n", opts.interpreter_file, strerror(errno));
        return 1;
    }

    out_fd = open(opts.output_file, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out_fd < 0) {
        fprintf(stderr, "cant open output %s\n", strerror(errno));
        return 1;
    }

    if (copy_append_file(out_fd, runtime_bin, &written) != 0) {
        fprintf(stderr, "failed to copy runtime '%s': %s\n", runtime_bin, strerror(errno));
        close(out_fd);
        return 1;
    }
    base_size = written;
    if (base_size == 0) {
        fprintf(stderr, "runtime binary is empty %s\n", runtime_bin);
        close(out_fd);
        return 1;
    }

    memset(&footer, 0, sizeof(footer));
    memcpy(footer.magic, LPACK_MAGIC, 7);
    footer.version = 1;
    footer.flags = 0;

    footer.script_offset = base_size;
    if (copy_append_file(out_fd, opts.input_script, &written) != 0) {
        fprintf(stderr, "failed to append script '%s': %s\n", opts.input_script, strerror(errno));
        close(out_fd);
        return 1;
    }
    footer.script_size = written;

    footer.interp_offset = footer.script_offset + footer.script_size;
    if (copy_append_file(out_fd, opts.interpreter_file, &written) != 0) {
        fprintf(stderr, "failed to append interpreter '%s': %s\n", opts.interpreter_file, strerror(errno));
        close(out_fd);
        return 1;
    }
    footer.interp_size = written;
    footer.interp_mode = (uint32_t)(interp_mode & 07777);

    if (write(out_fd, &footer, sizeof(footer)) != (ssize_t)sizeof(footer)) {
        fprintf(stderr, "failed to write footer: %s\n", strerror(errno));
        close(out_fd);
        return 1;
    }

    close(out_fd);
    if (chmod(opts.output_file, 0755) != 0) {
        fprintf(stderr, "cant chmod output: %s\n", strerror(errno));
        return 1;
    }



    return 0;
}
