#ifndef ONETOOL_LIBS_GUI_SWM_BUFFER_H
#define ONETOOL_LIBS_GUI_SWM_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct swm_buffer swm_buffer_t;

swm_buffer_t *swm_buffer_create(uint32_t kind, int fd, uint32_t width, uint32_t height, uint32_t stride, size_t size);
void swm_buffer_destroy(swm_buffer_t *buffer);

uint32_t swm_buffer_kind(const swm_buffer_t *buffer);
uint32_t swm_buffer_width(const swm_buffer_t *buffer);
uint32_t swm_buffer_height(const swm_buffer_t *buffer);
uint32_t swm_buffer_stride(const swm_buffer_t *buffer);
size_t swm_buffer_size(const swm_buffer_t *buffer);
const void *swm_buffer_pixels(const swm_buffer_t *buffer);

int swm_buffer_begin_cpu_read(swm_buffer_t *buffer);
void swm_buffer_end_cpu_read(swm_buffer_t *buffer);

#endif
