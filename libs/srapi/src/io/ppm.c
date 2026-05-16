#include "internal.h"

#include <stdint.h>
#include <stdio.h>

static void write_u16_le(FILE *fp, uint16_t value) {
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    fwrite(bytes, 1, sizeof(bytes), fp);
}

static void write_u32_le(FILE *fp, uint32_t value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
    fwrite(bytes, 1, sizeof(bytes), fp);
}

srapi_result_t srapi_save_ppm(const srapi_framebuffer_t *fb, const char *path) {
    FILE *fp;

    if (fb == NULL || fb->pixels == NULL || path == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return SRAPI_ERROR;
    }

    srapi_debugf("ppm save %s %ux%u", path, fb->width, fb->height);
    fprintf(fp, "P6\n%u %u\n255\n", fb->width, fb->height);
    for (uint32_t y = 0; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t color = fb->pixels[y * (fb->pitch / sizeof(uint32_t)) + x];
            unsigned char rgb[3];

            rgb[0] = (unsigned char)((color >> 16) & 0xff);
            rgb[1] = (unsigned char)((color >> 8) & 0xff);
            rgb[2] = (unsigned char)(color & 0xff);
            fwrite(rgb, 1, sizeof(rgb), fp);
        }
    }

    fclose(fp);
    return SRAPI_OK;
}

srapi_result_t srapi_save_bmp(const srapi_framebuffer_t *fb, const char *path) {
    FILE *fp;
    uint32_t row_bytes;
    uint32_t data_size;
    uint32_t file_size;
    unsigned char pad[3] = { 0, 0, 0 };

    if (fb == NULL || fb->pixels == NULL || path == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (fb->width > (UINT32_MAX - 3u) / 3u) {
        return SRAPI_ERROR_OVERFLOW;
    }

    row_bytes = ((uint32_t)((uint64_t)fb->width * 3u) + 3u) & ~3u;
    if (fb->height != 0 && row_bytes > UINT32_MAX / fb->height) {
        return SRAPI_ERROR_OVERFLOW;
    }
    data_size = row_bytes * fb->height;
    if (data_size > UINT32_MAX - 54u) {
        return SRAPI_ERROR_OVERFLOW;
    }
    file_size = 54u + data_size;

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return SRAPI_ERROR;
    }

    srapi_debugf("bmp save %s %ux%u", path, fb->width, fb->height);

    fwrite("BM", 1, 2, fp);
    write_u32_le(fp, file_size);
    write_u16_le(fp, 0);
    write_u16_le(fp, 0);
    write_u32_le(fp, 54);

    write_u32_le(fp, 40);
    write_u32_le(fp, fb->width);
    write_u32_le(fp, fb->height);
    write_u16_le(fp, 1);
    write_u16_le(fp, 24);
    write_u32_le(fp, 0);
    write_u32_le(fp, data_size);
    write_u32_le(fp, 2835);
    write_u32_le(fp, 2835);
    write_u32_le(fp, 0);
    write_u32_le(fp, 0);

    for (uint32_t row = 0; row < fb->height; row++) {
        uint32_t y = fb->height - 1u - row;
        uint32_t written = fb->width * 3u;

        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t color = fb->pixels[y * (fb->pitch / sizeof(uint32_t)) + x];
            unsigned char bgr[3];

            bgr[0] = (unsigned char)(color & 0xff);
            bgr[1] = (unsigned char)((color >> 8) & 0xff);
            bgr[2] = (unsigned char)((color >> 16) & 0xff);
            fwrite(bgr, 1, sizeof(bgr), fp);
        }
        if (row_bytes > written) {
            fwrite(pad, 1, row_bytes - written, fp);
        }
    }

    fclose(fp);
    return SRAPI_OK;
}
