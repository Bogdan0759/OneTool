#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zstd.h>

#define SRVID_MAGIC  "SRVD"
#define SRVID_VERSION  1u
#define SRVID_PIXFMT_BGRA8888  0u
#define SRVID_HEADER_SIZE  32u

static void usage(const char *tool) {
    printf("srvid2mp4 - convert .srvid recording to a video container via ffmpeg\n");
    printf("usage: %s <input.srvid> <output.mp4> [--codec libx264] [--crf 18] [--ffmpeg PATH]\n", tool);
}

static int read_u32_le(FILE *fp, uint32_t *out) {
    uint8_t b[4];

    if (fread(b, 1, 4, fp) != 4) {
        return 0;
    }
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

static int read_header(FILE *fp, uint32_t *w, uint32_t *h, uint32_t *fps_millihz) {
    char magic[4];
    uint32_t version;
    uint32_t pixfmt;
    uint8_t reserved[8];

    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, SRVID_MAGIC, 4) != 0) {
        fprintf(stderr, "srvid2mp4: not a .srvid file (bad magic)\n");
        return 0;
    }
    if (!read_u32_le(fp, &version) || version != SRVID_VERSION) {
        fprintf(stderr, "srvid2mp4: unsupported version %u (want %u)\n", version, SRVID_VERSION);
        return 0;
    }
    if (!read_u32_le(fp, w) || !read_u32_le(fp, h) ||
        !read_u32_le(fp, fps_millihz) || !read_u32_le(fp, &pixfmt)) {
        fprintf(stderr, "srvid2mp4: short header\n");
        return 0;
    }
    if (pixfmt != SRVID_PIXFMT_BGRA8888) {
        fprintf(stderr, "srvid2mp4: unsupported pixel format %u\n", pixfmt);
        return 0;
    }
    if (fread(reserved, 1, 8, fp) != 8) {
        fprintf(stderr, "srvid2mp4: short reserved area\n");
        return 0;
    }
    if (*w == 0 || *h == 0 || *fps_millihz == 0) {
        fprintf(stderr, "srvid2mp4: invalid header (w=%u h=%u fps_mhz=%u)\n", *w, *h, *fps_millihz);
        return 0;
    }
    return 1;
}

static int has_ext(const char *path, const char *ext) {
    size_t pl = strlen(path);
    size_t el = strlen(ext);

    if (pl < el) {
        return 0;
    }
    return strcasecmp(path + pl - el, ext) == 0;
}

int main(int argc, char *argv[]) {
    const char *input = NULL;
    const char *output = NULL;
    const char *codec = NULL;
    const char *crf = "18";
    const char *ffmpeg = "ffmpeg";
    FILE *fin;
    FILE *pipe_fp;
    uint32_t w;
    uint32_t h;
    uint32_t fps_millihz;
    char cmd[1024];
    void *in_buf = NULL;
    void *out_buf = NULL;
    size_t in_cap = 0;
    size_t raw_size;
    uint64_t frames = 0;
    int rc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            codec = argv[++i];
        } else if (strcmp(argv[i], "--crf") == 0 && i + 1 < argc) {
            crf = argv[++i];
        } else if (strcmp(argv[i], "--ffmpeg") == 0 && i + 1 < argc) {
            ffmpeg = argv[++i];
        } else if (input == NULL) {
            input = argv[i];
        } else if (output == NULL) {
            output = argv[i];
        } else {
            fprintf(stderr, "srvid2mp4: unexpected arg %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (input == NULL || output == NULL) {
        usage(argv[0]);
        return 1;
    }

    if (codec == NULL) {
        codec = has_ext(output, ".webm") ? "libvpx-vp9" : "libx264";
    }

    fin = fopen(input, "rb");
    if (fin == NULL) {
        fprintf(stderr, "srvid2mp4: cannot open %s: %s\n", input, strerror(errno));
        return 1;
    }
    if (!read_header(fin, &w, &h, &fps_millihz)) {
        fclose(fin);
        return 1;
    }

    raw_size = (size_t)w * (size_t)h * 4u;
    out_buf = malloc(raw_size);
    if (out_buf == NULL) {
        fprintf(stderr, "srvid2mp4: out of memory for frame buffer (%zu bytes)\n", raw_size);
        fclose(fin);
        return 1;
    }

    snprintf(cmd, sizeof(cmd),
        "%s -y -hide_banner -loglevel warning"
        " -f rawvideo -pixel_format bgra -video_size %ux%u -framerate %u/1000"
        " -i pipe:0 -c:v %s -crf %s -pix_fmt yuv420p \"%s\"",
        ffmpeg, w, h, fps_millihz, codec, crf, output);

    pipe_fp = popen(cmd, "w");
    if (pipe_fp == NULL) {
        fprintf(stderr, "srvid2mp4: popen ffmpeg failed: %s\n", strerror(errno));
        free(out_buf);
        fclose(fin);
        return 1;
    }

    printf("srvid2mp4: %s -> %s | %ux%u @ %u.%03u fps | codec=%s\n",
           input, output, w, h, fps_millihz / 1000, fps_millihz % 1000, codec);

    for (;;) {
        uint32_t csize;
        size_t got;

        if (!read_u32_le(fin, &csize)) {
            break;
        }
        if (csize == 0 || csize > 256u * 1024u * 1024u) {
            fprintf(stderr, "srvid2mp4: frame %" PRIu64 ": bogus compressed size %u\n", frames, csize);
            break;
        }
        if (csize > in_cap) {
            void *grow = realloc(in_buf, csize);
            if (grow == NULL) {
                fprintf(stderr, "srvid2mp4: out of memory (%u bytes)\n", csize);
                break;
            }
            in_buf = grow;
            in_cap = csize;
        }
        if (fread(in_buf, 1, csize, fin) != csize) {
            fprintf(stderr, "srvid2mp4: frame %" PRIu64 ": short read\n", frames);
            break;
        }
        got = ZSTD_decompress(out_buf, raw_size, in_buf, csize);
        if (ZSTD_isError(got)) {
            fprintf(stderr, "srvid2mp4: frame %" PRIu64 ": zstd decompress: %s\n",
                    frames, ZSTD_getErrorName(got));
            break;
        }
        if (got != raw_size) {
            fprintf(stderr, "srvid2mp4: frame %" PRIu64 ": size mismatch (got %zu want %zu)\n",
                    frames, got, raw_size);
            break;
        }
        if (fwrite(out_buf, 1, raw_size, pipe_fp) != raw_size) {
            fprintf(stderr, "srvid2mp4: write to ffmpeg failed: %s\n", strerror(errno));
            break;
        }
        frames++;
        if ((frames & 0x1f) == 0) {
            printf("\rsrvid2mp4: %" PRIu64 " frames", frames);
            fflush(stdout);
        }
    }
    printf("\rsrvid2mp4: %" PRIu64 " frames total\n", frames);

    rc = pclose(pipe_fp);
    free(in_buf);
    free(out_buf);
    fclose(fin);

    if (rc == -1) {
        fprintf(stderr, "srvid2mp4: pclose failed: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) {
        fprintf(stderr, "srvid2mp4: ffmpeg exited with code %d\n", WEXITSTATUS(rc));
        return 1;
    }
    if (frames == 0) {
        fprintf(stderr, "srvid2mp4: no frames written\n");
        return 1;
    }
    return 0;
}
