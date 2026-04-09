#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LMAGIC "LPACK01"

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

static int read_exact_at(int fd, void *buf, size_t len, off_t off) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (char *)buf + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }
        if (n == 0) {
            return 1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int copy_range(int src_fd, off_t src_off, uint64_t size, const char *out_path, mode_t mode) {
    int out_fd;
    char buf[8192];
    uint64_t remain = size;

    out_fd = open(out_path, O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (out_fd < 0) {
        return 1;
    }

    while (remain > 0) {
        size_t want = remain > sizeof(buf) ? sizeof(buf) : (size_t)remain;
        if (read_exact_at(src_fd, buf, want, src_off) != 0) {
            close(out_fd);
            return 1;
        }
        src_off += (off_t)want;
        remain -= (uint64_t)want;

        size_t written = 0;
        while (written < want) {
            ssize_t n = write(out_fd, buf + written, want - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(out_fd);
                return 1;
            }
            written += (size_t)n;
        }
    }

    if (fchmod(out_fd, mode) != 0) {
        close(out_fd);
        return 1;
    }

    close(out_fd);
    return 0;
}

int main(int argc, char *argv[]) {
    int self_fd = -1;
    struct stat st;
    lpack_footer_t footer;
    off_t footer_off;
    char script_tpl[] = "/tmp/lpack_script_XXXXXX";
    char interp_tpl[] = "/tmp/lpack_interp_XXXXXX";
    int script_fd = -1;
    int interp_fd = -1;
    char *script_path = NULL;
    char *interp_path = NULL;
    const char *chosen_interp = NULL;
    char **exec_argv = NULL;
    int rc = 1;

    self_fd = open("/proc/self/exe", O_RDONLY);
    if (self_fd < 0) {
        fprintf(stderr, "cannot open self: %s\n", strerror(errno));
        goto cleanup;
    }
    if (fstat(self_fd, &st) != 0) {
        fprintf(stderr, "fstat failed: %s\n", strerror(errno));
        goto cleanup;
    }
    if ((uint64_t)st.st_size < (uint64_t)sizeof(footer)) {
        fprintf(stderr, "packed footer not found\n");
        goto cleanup;
    }

    footer_off = (off_t)((uint64_t)st.st_size - (uint64_t)sizeof(footer));
    if (read_exact_at(self_fd, &footer, sizeof(footer), footer_off) != 0) {
        fprintf(stderr, "failed to read footer\n");
        goto cleanup;
    }
    if (memcmp(footer.magic, LMAGIC, 7) != 0) {
        fprintf(stderr, "invalid package signature\n");
        goto cleanup;
    }
    if (footer.script_size == 0) {
        fprintf(stderr, "script payload is empty\n");
        goto cleanup;
    }

    script_fd = mkstemp(script_tpl);
    if (script_fd < 0) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        goto cleanup;
    }
    close(script_fd);
    script_fd = -1;
    script_path = script_tpl;
    if (copy_range(self_fd, (off_t)footer.script_offset, footer.script_size, script_path, 0600) != 0) {
        fprintf(stderr, "failed to unpack script\n");
        goto cleanup;
    }

    if (footer.interp_size == 0) {
        fprintf(stderr, "interpreter is not embedded\n");
        goto cleanup;
    }
    interp_fd = mkstemp(interp_tpl);
    if (interp_fd < 0) {
        fprintf(stderr, "mkstemp failed %s\n", strerror(errno));
        goto cleanup;
    }
    close(interp_fd);
    interp_fd = -1;
    interp_path = interp_tpl;
    if (copy_range(self_fd, (off_t)footer.interp_offset, footer.interp_size, interp_path,
                   footer.interp_mode ? (mode_t)footer.interp_mode : 0755) != 0) {
        fprintf(stderr, "faile unpack interpreter\n");
        goto cleanup;
    }
    chosen_interp = interp_path;

    exec_argv = (char **)calloc((size_t)argc + 2, sizeof(char *));
    if (exec_argv == NULL) {
        fprintf(stderr, "out of memory\n");
        goto cleanup;
    }
    exec_argv[0] = (char *)chosen_interp;
    exec_argv[1] = script_path;
    for (int i = 1; i < argc; i++) {
        exec_argv[i + 1] = argv[i];
    }
    exec_argv[argc + 1] = NULL;

    execvp(chosen_interp, exec_argv);
    fprintf(stderr, "failed to start interpreter '%s': %s\n", chosen_interp, strerror(errno));

cleanup:
    if (script_fd >= 0) close(script_fd);
    if (interp_fd >= 0) close(interp_fd);
    if (self_fd >= 0) close(self_fd);
    if (exec_argv != NULL) free(exec_argv);
    return rc;
}
