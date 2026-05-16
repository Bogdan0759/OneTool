#include "internal.h"

#include <stdio.h>

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
