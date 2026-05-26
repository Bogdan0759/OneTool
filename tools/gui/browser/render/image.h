#ifndef ONETOOL_TOOLS_GUI_BROWSER_RENDER_IMAGE_H
#define ONETOOL_TOOLS_GUI_BROWSER_RENDER_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decode an image from raw bytes into ARGB32 pixels (0xAARRGGBB,
 * matching ranal's RANAL_COLOR layout).
 *
 * Supported formats: JPEG, PNG (detected via magic bytes).
 *
 * On success *out_pixels is a malloc'd buffer of w*h uint32_t values.
 * Returns 0 on success, -1 on failure (unsupported format or decode error).
 */
int br_image_decode(const void *data, size_t len,
                    uint32_t **out_pixels, int *out_w, int *out_h);

#endif
