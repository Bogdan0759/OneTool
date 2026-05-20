#include "../core/internal.h"
#include "threadpool.h"

#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define SRAPI_VM_REG_COUNT 16

typedef struct {
    uint32_t *pixels;
    uint32_t pitch_bytes;
    srapi_shader_t *shader;
    int32_t rect_x0;
    int32_t rect_y0;
    uint32_t rect_width;
    uint32_t rect_height;
    uint32_t clip_x;
    uint32_t clip_y;
    uint32_t clip_width;
    uint32_t clip_height;
    uint32_t tile_rows;
} shade_job_t;

static const int32_t g_store_mask[16] __attribute__((aligned(32))) = {
    -1, -1, -1, -1, -1, -1, -1, -1,
     0,  0,  0,  0,  0,  0,  0,  0
};

static inline __m256 fract8(__m256 v) {
    return _mm256_sub_ps(v, _mm256_floor_ps(v));
}

static inline __m256 clamp01_8(__m256 v) {
    return _mm256_max_ps(_mm256_setzero_ps(),
                         _mm256_min_ps(_mm256_set1_ps(1.0f), v));
}

static inline __m256 abs8(__m256 v) {
    __m256i mask = _mm256_set1_epi32(0x7fffffff);
    return _mm256_and_ps(v, _mm256_castsi256_ps(mask));
}

typedef float (*scalar_unary_fn)(float);
typedef float (*scalar_binary_fn)(float, float);

static __m256 scalar_unary8(__m256 v, scalar_unary_fn fn) {
    float arr[8] __attribute__((aligned(32)));

    _mm256_store_ps(arr, v);
    arr[0] = fn(arr[0]);
    arr[1] = fn(arr[1]);
    arr[2] = fn(arr[2]);
    arr[3] = fn(arr[3]);
    arr[4] = fn(arr[4]);
    arr[5] = fn(arr[5]);
    arr[6] = fn(arr[6]);
    arr[7] = fn(arr[7]);
    return _mm256_load_ps(arr);
}

static __m256 scalar_binary8(__m256 a, __m256 b, scalar_binary_fn fn) {
    float aa[8] __attribute__((aligned(32)));
    float bb[8] __attribute__((aligned(32)));

    _mm256_store_ps(aa, a);
    _mm256_store_ps(bb, b);
    aa[0] = fn(aa[0], bb[0]);
    aa[1] = fn(aa[1], bb[1]);
    aa[2] = fn(aa[2], bb[2]);
    aa[3] = fn(aa[3], bb[3]);
    aa[4] = fn(aa[4], bb[4]);
    aa[5] = fn(aa[5], bb[5]);
    aa[6] = fn(aa[6], bb[6]);
    aa[7] = fn(aa[7], bb[7]);
    return _mm256_load_ps(aa);
}

static float scalar_sin(float v) { return sinf(v); }
static float scalar_cos(float v) { return cosf(v); }
static float scalar_pow(float a, float b) { return powf(a, b); }

static inline __m256 load_reg(const __m256 regs[SRAPI_VM_REG_COUNT], uint8_t idx) {
    return idx < SRAPI_VM_REG_COUNT ? regs[idx] : _mm256_setzero_ps();
}

static inline __m256 u32_bits_to_broadcast(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return _mm256_set1_ps(f);
}

static void emit_pixels(uint32_t *dst, int n_lanes,
                        __m256 fr, __m256 fg, __m256 fb_, __m256 fa) {
    __m256 v255 = _mm256_set1_ps(255.0f);
    __m256 vhalf = _mm256_set1_ps(0.5f);

    fr = _mm256_add_ps(_mm256_mul_ps(clamp01_8(fr), v255), vhalf);
    fg = _mm256_add_ps(_mm256_mul_ps(clamp01_8(fg), v255), vhalf);
    fb_ = _mm256_add_ps(_mm256_mul_ps(clamp01_8(fb_), v255), vhalf);
    fa = _mm256_add_ps(_mm256_mul_ps(clamp01_8(fa), v255), vhalf);

    __m256i ri = _mm256_cvttps_epi32(fr);
    __m256i gi = _mm256_cvttps_epi32(fg);
    __m256i bi = _mm256_cvttps_epi32(fb_);
    __m256i ai = _mm256_cvttps_epi32(fa);

    __m256i pixel = _mm256_or_si256(bi,
                    _mm256_or_si256(_mm256_slli_epi32(gi, 8),
                    _mm256_or_si256(_mm256_slli_epi32(ri, 16),
                                    _mm256_slli_epi32(ai, 24))));

    if (n_lanes >= 8) {
        _mm256_storeu_si256((__m256i *)dst, pixel);
    } else if (n_lanes > 0) {
        __m256i mask = _mm256_loadu_si256((const __m256i *)&g_store_mask[8 - n_lanes]);
        _mm256_maskstore_epi32((int *)dst, mask, pixel);
    }
}

static void run_chunk(uint32_t *dst, int n_lanes,
                      __m256 x_f, __m256 y_f, __m256 u_f, __m256 v_f,
                      __m256 width_f, __m256 height_f,
                      const srapi_shader_t *shader) {
    __m256 regs[SRAPI_VM_REG_COUNT];
    __m256 final_r = _mm256_set1_ps(1.0f);
    __m256 final_g = _mm256_setzero_ps();
    __m256 final_b = _mm256_set1_ps(1.0f);
    __m256 final_a = _mm256_set1_ps(1.0f);

    for (int i = 0; i < SRAPI_VM_REG_COUNT; i++) {
        regs[i] = _mm256_setzero_ps();
    }

    for (size_t pc = 0; pc < shader->inst_count; pc++) {
        const srapi_vm_inst_t *inst = &shader->insts[pc];
        uint8_t op = inst->op;
        uint8_t dst_idx = inst->dst;

        if (op == SRAPI_VM_END) {
            break;
        }
        if (op == SRAPI_VM_OUT_COLOR) {
            final_r = load_reg(regs, inst->a);
            final_g = load_reg(regs, inst->b);
            final_b = load_reg(regs, inst->c);
            final_a = load_reg(regs, inst->d);
            continue;
        }
        if (dst_idx >= SRAPI_VM_REG_COUNT) {
            continue;
        }

        __m256 a = load_reg(regs, inst->a);
        __m256 b = load_reg(regs, inst->b);

        switch (op) {
            case SRAPI_VM_MOV:
                regs[dst_idx] = a;
                break;
            case SRAPI_VM_ADD:
                regs[dst_idx] = _mm256_add_ps(a, b);
                break;
            case SRAPI_VM_SUB:
                regs[dst_idx] = _mm256_sub_ps(a, b);
                break;
            case SRAPI_VM_MUL:
                regs[dst_idx] = _mm256_mul_ps(a, b);
                break;
            case SRAPI_VM_DIV: {
                __m256 zero = _mm256_setzero_ps();
                __m256 safe = _mm256_blendv_ps(_mm256_set1_ps(1.0f), b, _mm256_cmp_ps(b, zero, _CMP_NEQ_OQ));
                __m256 quotient = _mm256_div_ps(a, safe);
                regs[dst_idx] = _mm256_blendv_ps(zero, quotient, _mm256_cmp_ps(b, zero, _CMP_NEQ_OQ));
                break;
            }
            case SRAPI_VM_MIN:
                regs[dst_idx] = _mm256_min_ps(a, b);
                break;
            case SRAPI_VM_MAX:
                regs[dst_idx] = _mm256_max_ps(a, b);
                break;
            case SRAPI_VM_POW:
                regs[dst_idx] = scalar_binary8(a, b, scalar_pow);
                break;
            case SRAPI_VM_LOAD_INPUT:
                switch (inst->a) {
                    case SRAPI_VM_INPUT_X:      regs[dst_idx] = x_f; break;
                    case SRAPI_VM_INPUT_Y:      regs[dst_idx] = y_f; break;
                    case SRAPI_VM_INPUT_U:      regs[dst_idx] = u_f; break;
                    case SRAPI_VM_INPUT_V:      regs[dst_idx] = v_f; break;
                    case SRAPI_VM_INPUT_WIDTH:  regs[dst_idx] = width_f; break;
                    case SRAPI_VM_INPUT_HEIGHT: regs[dst_idx] = height_f; break;
                    default:                    regs[dst_idx] = _mm256_setzero_ps(); break;
                }
                break;
            case SRAPI_VM_LOAD_UNIFORM:
                if (inst->a < shader->uniform_count) {
                    regs[dst_idx] = _mm256_set1_ps(shader->uniforms[inst->a]);
                } else {
                    regs[dst_idx] = _mm256_setzero_ps();
                }
                break;
            case SRAPI_VM_LOAD_CONST:
                regs[dst_idx] = u32_bits_to_broadcast(inst->imm);
                break;
            case SRAPI_VM_FRACT:
                regs[dst_idx] = fract8(a);
                break;
            case SRAPI_VM_CLAMP01:
                regs[dst_idx] = clamp01_8(a);
                break;
            case SRAPI_VM_SIN:
                regs[dst_idx] = scalar_unary8(a, scalar_sin);
                break;
            case SRAPI_VM_COS:
                regs[dst_idx] = scalar_unary8(a, scalar_cos);
                break;
            case SRAPI_VM_ABS:
                regs[dst_idx] = abs8(a);
                break;
            case SRAPI_VM_SQRT:
                regs[dst_idx] = _mm256_sqrt_ps(_mm256_max_ps(_mm256_setzero_ps(), a));
                break;
            case SRAPI_VM_MIX: {
                __m256 c = load_reg(regs, inst->c);
                __m256 t = clamp01_8(c);
                regs[dst_idx] = _mm256_add_ps(a, _mm256_mul_ps(_mm256_sub_ps(b, a), t));
                break;
            }
            default:
                break;
        }
    }

    emit_pixels(dst, n_lanes, final_r, final_g, final_b, final_a);
}

static void shade_tile_rows(const shade_job_t *job, uint32_t row_begin, uint32_t row_end) {
    const __m256 x_offsets = _mm256_set_ps(7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f);
    float inv_w = job->rect_width > 1 ? 1.0f / (float)(job->rect_width - 1) : 0.0f;
    float inv_h = job->rect_height > 1 ? 1.0f / (float)(job->rect_height - 1) : 0.0f;
    __m256 v_rect_x0 = _mm256_set1_ps((float)job->rect_x0);
    __m256 v_inv_w = _mm256_set1_ps(inv_w);
    __m256 v_width_f = _mm256_set1_ps((float)job->rect_width);
    __m256 v_height_f = _mm256_set1_ps((float)job->rect_height);
    uint32_t chunk_end_px = job->clip_x + job->clip_width;

    for (uint32_t py = row_begin; py < row_end; py++) {
        uint32_t *row = (uint32_t *)((uint8_t *)job->pixels + (size_t)py * job->pitch_bytes);
        float v_scalar = job->rect_height > 1
            ? ((float)(int32_t)py - (float)job->rect_y0) * inv_h
            : 0.0f;
        __m256 y_f = _mm256_set1_ps((float)(int32_t)py);
        __m256 v_f = _mm256_set1_ps(v_scalar);

        for (uint32_t px = job->clip_x; px < chunk_end_px; px += 8) {
            uint32_t remaining = chunk_end_px - px;
            int n_lanes = remaining >= 8 ? 8 : (int)remaining;
            __m256 x_f = _mm256_add_ps(_mm256_set1_ps((float)px), x_offsets);
            __m256 u_f = _mm256_mul_ps(_mm256_sub_ps(x_f, v_rect_x0), v_inv_w);

            run_chunk(row + px, n_lanes, x_f, y_f, u_f, v_f, v_width_f, v_height_f, job->shader);
        }
    }
}

static void shade_tile_fn(void *user, uint32_t tile_index) {
    const shade_job_t *job = (const shade_job_t *)user;
    uint32_t row_begin = job->clip_y + tile_index * job->tile_rows;
    uint32_t row_end = row_begin + job->tile_rows;
    uint32_t clip_end = job->clip_y + job->clip_height;

    if (row_end > clip_end) {
        row_end = clip_end;
    }
    if (row_begin >= row_end) {
        return;
    }
    shade_tile_rows(job, row_begin, row_end);
}

static srapi_result_t shade_scalar(const shade_job_t *job) {
    float inv_w = job->rect_width > 1 ? 1.0f / (float)(job->rect_width - 1) : 0.0f;
    float inv_h = job->rect_height > 1 ? 1.0f / (float)(job->rect_height - 1) : 0.0f;
    uint32_t cx1 = job->clip_x + job->clip_width;
    uint32_t cy1 = job->clip_y + job->clip_height;

    for (uint32_t py = job->clip_y; py < cy1; py++) {
        uint32_t *row = (uint32_t *)((uint8_t *)job->pixels + (size_t)py * job->pitch_bytes);

        for (uint32_t px = job->clip_x; px < cx1; px++) {
            srapi_color_t color;
            float inputs[6];
            srapi_result_t r;

            inputs[SRAPI_VM_INPUT_X] = (float)px;
            inputs[SRAPI_VM_INPUT_Y] = (float)py;
            inputs[SRAPI_VM_INPUT_U] = ((float)(int32_t)px - (float)job->rect_x0) * inv_w;
            inputs[SRAPI_VM_INPUT_V] = ((float)(int32_t)py - (float)job->rect_y0) * inv_h;
            inputs[SRAPI_VM_INPUT_WIDTH] = (float)job->rect_width;
            inputs[SRAPI_VM_INPUT_HEIGHT] = (float)job->rect_height;

            r = srapi_vm_run_fragment(job->shader, inputs, &color);
            if (r != SRAPI_OK) {
                return r;
            }
            row[px] = color;
        }
    }
    return SRAPI_OK;
}

srapi_result_t srapi_vm_shade_rect(
    uint32_t *pixels,
    uint32_t pitch_bytes,
    srapi_shader_t *shader,
    int32_t rect_x0,
    int32_t rect_y0,
    uint32_t rect_width,
    uint32_t rect_height,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    if (pixels == NULL || shader == NULL || pitch_bytes == 0) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (width == 0 || height == 0) {
        return SRAPI_OK;
    }

    shade_job_t job;
    job.pixels = pixels;
    job.pitch_bytes = pitch_bytes;
    job.shader = shader;
    job.rect_x0 = rect_x0;
    job.rect_y0 = rect_y0;
    job.rect_width = rect_width;
    job.rect_height = rect_height;
    job.clip_x = x;
    job.clip_y = y;
    job.clip_width = width;
    job.clip_height = height;
    job.tile_rows = height;

    srapi_shade_config_t cfg = srapi_get_shade_config();

    shader->run_count++;
    if (shader->run_count < 2) {
        srapi_debugf("vm shade_rect rect=(%d,%d %ux%u) clip=(%u,%u %ux%u) insts=%zu simd=%d threads=%d",
                     rect_x0, rect_y0, rect_width, rect_height,
                     x, y, width, height, shader->inst_count,
                     cfg.simd_enabled, cfg.threads_enabled);
    }

    if (!cfg.simd_enabled) {
        return shade_scalar(&job);
    }

    if (!cfg.threads_enabled) {
        shade_tile_rows(&job, y, y + height);
        return SRAPI_OK;
    }

    uint32_t tile_rows = 32;
    if (height < tile_rows) {
        tile_rows = height;
    }
    job.tile_rows = tile_rows;
    uint32_t tile_count = (height + tile_rows - 1) / tile_rows;

    srapi_vm_threadpool_dispatch(tile_count, shade_tile_fn, &job);
    return SRAPI_OK;
}
