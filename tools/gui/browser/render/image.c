/*
 * Image decoder for the browser: JPEG via libjpeg, PNG via libpng.
 *
 * Outputs ARGB32 (0xFFRRGGBB) matching the ranal pixel format so we can
 * blit directly into surfaces.
 */
#include "image.h"

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <png.h>
#include <jpeglib.h>

#define IMG_MAX_DIM  4096

/* ---- JPEG ---------------------------------------------------------- */

struct jpeg_err_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf               escape;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    struct jpeg_err_mgr *e = (struct jpeg_err_mgr *)cinfo->err;
    longjmp(e->escape, 1);
}

static int decode_jpeg(const void *data, size_t len,
                       uint32_t **out_pixels, int *out_w, int *out_h) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if (setjmp(jerr.escape)) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (const unsigned char *)data, (unsigned long)len);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    uint32_t *pixels = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (pixels == NULL) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    int stride = w * (int)cinfo.output_components;
    unsigned char *row = (unsigned char *)malloc((size_t)stride);
    if (row == NULL) {
        free(pixels);
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }

    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rows[1] = { row };
        jpeg_read_scanlines(&cinfo, rows, 1);
        uint32_t *dst = pixels + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint8_t r = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];
            dst[x] = (0xFFu << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | (uint32_t)b;
        }
        y++;
    }

    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ---- PNG ----------------------------------------------------------- */

typedef struct {
    const unsigned char *data;
    size_t               len;
    size_t               pos;
} png_mem_reader_t;

static void png_mem_read_fn(png_structp png_ptr, png_bytep out, png_size_t count) {
    png_mem_reader_t *r = (png_mem_reader_t *)png_get_io_ptr(png_ptr);
    if (r->pos + count > r->len) {
        png_error(png_ptr, "read past end");
        return;
    }
    memcpy(out, r->data + r->pos, count);
    r->pos += count;
}

static int decode_png(const void *data, size_t len,
                      uint32_t **out_pixels, int *out_w, int *out_h) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                             NULL, NULL, NULL);
    if (png == NULL) return -1;

    png_infop info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    png_mem_reader_t reader = { (const unsigned char *)data, len, 0 };
    png_set_read_fn(png, &reader, png_mem_read_fn);

    png_read_info(png, info);

    int w = (int)png_get_image_width(png, info);
    int h = (int)png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (w <= 0 || h <= 0 || w > IMG_MAX_DIM || h > IMG_MAX_DIM) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    size_t rowbytes = png_get_rowbytes(png, info);
    uint32_t *pixels = (uint32_t *)malloc((size_t)w * h * sizeof(uint32_t));
    if (pixels == NULL) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    png_bytep *rows = (png_bytep *)malloc((size_t)h * sizeof(png_bytep));
    if (rows == NULL) {
        free(pixels);
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }
    for (int y = 0; y < h; y++) {
        rows[y] = (png_bytep)malloc(rowbytes);
        if (rows[y] == NULL) {
            for (int j = 0; j < y; j++) free(rows[j]);
            free(rows);
            free(pixels);
            png_destroy_read_struct(&png, &info, NULL);
            return -1;
        }
    }

    png_read_image(png, rows);

    for (int y = 0; y < h; y++) {
        const unsigned char *src = rows[y];
        uint32_t *dst = pixels + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | (uint32_t)b;
        }
        free(rows[y]);
    }
    free(rows);

    png_destroy_read_struct(&png, &info, NULL);

    *out_pixels = pixels;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* ---- Public API ---------------------------------------------------- */

int br_image_decode(const void *data, size_t len,
                    uint32_t **out_pixels, int *out_w, int *out_h) {
    if (data == NULL || len < 4 || out_pixels == NULL) return -1;

    const unsigned char *p = (const unsigned char *)data;

    /* PNG: 89 50 4E 47 */
    if (len >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G')
        return decode_png(data, len, out_pixels, out_w, out_h);

    /* JPEG: FF D8 FF */
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return decode_jpeg(data, len, out_pixels, out_w, out_h);

    return -1;
}
