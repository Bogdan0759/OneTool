#include "internal.h"

#include <stdlib.h>
#include <string.h>

void js_chunk_init(js_chunk_t *chunk) {
    memset(chunk, 0, sizeof(*chunk));
}

void js_chunk_free(js_chunk_t *chunk) {
    if (chunk == NULL) return;
    free(chunk->code);
    for (int i = 0; i < chunk->const_count; i++) {
        js_runtime_value_free(&chunk->constants[i]);
    }
    free(chunk->constants);
    memset(chunk, 0, sizeof(*chunk));
}

int js_chunk_write(js_chunk_t *chunk, uint8_t byte) {
    if (chunk->code_count == chunk->code_cap) {
        int want = chunk->code_cap == 0 ? 64 : chunk->code_cap * 2;
        uint8_t *p = (uint8_t *)realloc(chunk->code, (size_t)want);
        if (p == NULL) return -1;
        chunk->code = p;
        chunk->code_cap = want;
    }
    chunk->code[chunk->code_count++] = byte;
    return 0;
}

int js_chunk_write_u16(js_chunk_t *chunk, uint16_t value) {
    if (js_chunk_write(chunk, (uint8_t)((value >> 8) & 0xFF)) != 0) return -1;
    if (js_chunk_write(chunk, (uint8_t)(value & 0xFF)) != 0) return -1;
    return 0;
}

int js_chunk_add_constant(js_chunk_t *chunk, js_runtime_value_t value) {
    if (chunk->const_count == chunk->const_cap) {
        int want = chunk->const_cap == 0 ? 16 : chunk->const_cap * 2;
        js_runtime_value_t *p = (js_runtime_value_t *)realloc(
            chunk->constants, (size_t)want * sizeof(*p));
        if (p == NULL) return -1;
        chunk->constants = p;
        chunk->const_cap = want;
    }
    chunk->constants[chunk->const_count] = value;
    return chunk->const_count++;
}
