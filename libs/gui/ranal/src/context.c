#include "internal.h"

#include <stdlib.h>
#include <string.h>

ranal_surface_t *ranal_surface_create(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0 || g_ranal == NULL || g_ranal->ctx == NULL) {
        return NULL;
    }
    struct ranal_surface *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    srapi_framebuffer_t *fb = NULL;
    srapi_framebuffer_desc_t desc = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
    };
    if (srapi_create_framebuffer(g_ranal->ctx, &desc, &fb) != SRAPI_OK) {
        free(s);
        return NULL;
    }
    s->width = width;
    s->height = height;
    s->pitch = (int32_t)srapi_framebuffer_pitch(fb);
    s->pixels = srapi_framebuffer_pixels(fb);
    s->fb = fb;
    s->owns_fb = 1;
    return s;
}

void ranal_surface_destroy(ranal_surface_t *surface) {
    if (surface == NULL) {
        return;
    }
    if (surface->owns_fb && surface->fb != NULL) {
        srapi_destroy_framebuffer(surface->fb);
    }
    free(surface);
}

uint32_t *ranal_surface_pixels(ranal_surface_t *surface) {
    return surface != NULL ? surface->pixels : NULL;
}

int32_t ranal_surface_width(const ranal_surface_t *surface) {
    return surface != NULL ? surface->width : 0;
}

int32_t ranal_surface_height(const ranal_surface_t *surface) {
    return surface != NULL ? surface->height : 0;
}

int32_t ranal_surface_pitch(const ranal_surface_t *surface) {
    return surface != NULL ? surface->pitch : 0;
}

void ranal_blit_surface(const ranal_surface_t *src, int32_t dst_x, int32_t dst_y) {
    if (src == NULL || g_ranal == NULL || src->pixels == NULL) {
        return;
    }
    srapi_framebuffer_t *dst_fb = NULL;
    if (g_ranal->targets_drm && g_ranal->drm != NULL) {
        dst_fb = srapi_drm_backbuffer(g_ranal->drm);
    } else if (g_ranal->target != NULL) {
        dst_fb = g_ranal->target->fb;
    }
    if (dst_fb == NULL) {
        return;
    }
    uint32_t *dst_pixels = srapi_framebuffer_pixels(dst_fb);
    int32_t dst_w = (int32_t)srapi_framebuffer_width(dst_fb);
    int32_t dst_h = (int32_t)srapi_framebuffer_height(dst_fb);
    int32_t dst_pitch = (int32_t)srapi_framebuffer_pitch(dst_fb);
    if (dst_pixels == NULL) {
        return;
    }

    int32_t sx0 = 0;
    int32_t sy0 = 0;
    int32_t w = src->width;
    int32_t h = src->height;
    if (dst_x < 0) { sx0 = -dst_x; w -= sx0; dst_x = 0; }
    if (dst_y < 0) { sy0 = -dst_y; h -= sy0; dst_y = 0; }
    if (dst_x + w > dst_w) w = dst_w - dst_x;
    if (dst_y + h > dst_h) h = dst_h - dst_y;
    if (w <= 0 || h <= 0) {
        return;
    }
    int32_t src_pitch_px = src->pitch / 4;
    int32_t dst_pitch_px = dst_pitch / 4;
    for (int32_t row = 0; row < h; row++) {
        const uint32_t *src_row = src->pixels + (sy0 + row) * src_pitch_px + sx0;
        uint32_t *dst_row = dst_pixels + (dst_y + row) * dst_pitch_px + dst_x;
        memcpy(dst_row, src_row, (size_t)w * 4u);
    }
    g_ranal->dirty = 1;
}

ranal_context_t *ranal_default_context(void) {
    return g_ranal;
}

ranal_context_t *ranal_get_current(void) {
    return g_ranal;
}

void ranal_set_current(ranal_context_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    g_ranal = ctx;
}

ranal_context_t *ranal_context_create_offscreen(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    struct ranal_context *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }
    if (srapi_create_context(&(srapi_context_desc_t){
            .width = (uint32_t)width,
            .height = (uint32_t)height,
            .backend = SRAPI_BACKEND_CPU,
        }, &ctx->ctx) != SRAPI_OK) {
        free(ctx);
        return NULL;
    }
    if (srapi_create_cmd_buffer(ctx->ctx, &ctx->cmd) != SRAPI_OK) {
        srapi_destroy_context(ctx->ctx);
        free(ctx);
        return NULL;
    }
    ctx->width = width;
    ctx->height = height;
    ctx->theme = &ranal_theme_dark;
    ctx->targets_drm = 0;

    ranal_context_t *prev = g_ranal;
    g_ranal = ctx;
    ctx->target = ranal_surface_create(width, height);
    g_ranal = prev;
    if (ctx->target == NULL) {
        srapi_destroy_cmd_buffer(ctx->cmd);
        srapi_destroy_context(ctx->ctx);
        free(ctx);
        return NULL;
    }

    ctx->root = calloc(1, sizeof(*ctx->root));
    if (ctx->root == NULL) {
        ranal_surface_destroy(ctx->target);
        srapi_destroy_cmd_buffer(ctx->cmd);
        srapi_destroy_context(ctx->ctx);
        free(ctx);
        return NULL;
    }
    ctx->root->kind = RANAL_WIDGET_PANEL;
    ctx->root->visible = 1;
    ctx->root->enabled = 1;
    ctx->root->width = width;
    ctx->root->height = height;
    ctx->root->layout = RANAL_LAYOUT_ABSOLUTE;

    srapi_clock_init(&ctx->frame_clock);
    srapi_clock_init(&ctx->total_clock);
    ctx->dirty = 1;
    ctx->initialized = 1;
    return ctx;
}

void ranal_context_destroy(ranal_context_t *ctx) {
    if (ctx == NULL || ctx == ranal_default_context()) {
        return;
    }
    ranal_context_t *prev = g_ranal;
    g_ranal = ctx;
    if (ctx->root != NULL) {
        ranal_widget_free_recursive_(ctx->root);
    }
    while (ctx->popups != NULL) {
        ranal_widget_t *p = ctx->popups;
        ctx->popups = p->next_sibling;
        ranal_widget_free_recursive_(p);
    }
    if (ctx->target != NULL) {
        ranal_surface_destroy(ctx->target);
    }
    if (ctx->cmd != NULL) {
        srapi_destroy_cmd_buffer(ctx->cmd);
    }
    if (ctx->ctx != NULL) {
        srapi_destroy_context(ctx->ctx);
    }
    g_ranal = (prev == ctx) ? ranal_default_context() : prev;
    free(ctx);
}

int ranal_context_frame(ranal_context_t *ctx) {
    if (ctx == NULL) {
        return 1;
    }
    ranal_context_t *prev = g_ranal;
    g_ranal = ctx;
    int rc = ranal_frame();
    g_ranal = prev;
    return rc;
}

ranal_widget_t *ranal_context_root(ranal_context_t *ctx) {
    return ctx != NULL ? ctx->root : NULL;
}

ranal_surface_t *ranal_context_surface(ranal_context_t *ctx) {
    return ctx != NULL ? ctx->target : NULL;
}

#include <sprot/client.h>

ranal_widget_t *ranal_popup(int32_t x, int32_t y) {
    if (g_ranal == NULL || !g_ranal->initialized) {
        return NULL;
    }
    ranal_widget_t *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->kind = RANAL_WIDGET_PANEL;
    p->visible = 1;
    p->enabled = 1;
    p->x = x;
    p->y = y;
    p->width = 100;
    p->height = 100;
    p->auto_w = 1;
    p->auto_h = 1;
    p->layout = RANAL_LAYOUT_VERT;
    p->padding = 6;
    p->spacing = 4;
    p->next_sibling = g_ranal->popups;
    g_ranal->popups = p;
    g_ranal->dirty = 1;

    return p;
}

void ranal_popup_close(ranal_widget_t *popup) {
    if (popup == NULL || g_ranal == NULL) {
        return;
    }
    ranal_widget_t **link = &g_ranal->popups;
    while (*link != NULL) {
        if (*link == popup) {
            *link = popup->next_sibling;
            popup->next_sibling = NULL;
            if (popup->sprot_popup_surface != NULL) {
                sprot_destroy_surface((sprot_surface_t *)popup->sprot_popup_surface);
                popup->sprot_popup_surface = NULL;
            }
            if (popup->popup_target != NULL) {
                ranal_surface_destroy(popup->popup_target);
                popup->popup_target = NULL;
            }
            ranal_widget_free_recursive_(popup);
            g_ranal->dirty = 1;
            return;
        }
        link = &(*link)->next_sibling;
    }
}
