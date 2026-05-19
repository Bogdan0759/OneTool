#include "compiler.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef SRAPI_I915_MI_BATCH_BUFFER_END
#define SRAPI_I915_MI_BATCH_BUFFER_END 0x05000000u
#endif

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
} gen9_inst_t;

static void emit_mov_reg(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200001; // mov (1) rDst:f, rSrc0:f
    inst[1] = 0x0008a200;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8);
    inst[3] = 0;
    *pc += 4;
}

static void emit_mov_imm(uint32_t *code, uint32_t *pc, uint32_t dst_reg, float val) {
    uint32_t *inst = &code[*pc];
    union { float f; uint32_t u; } u;
    u.f = val;
    inst[0] = 0x07200001; // mov (1) rDst:f, imm:f
    inst[1] = 0x0008a200;
    inst[2] = ((dst_reg & 0xff) << 8);
    inst[3] = u.u;
    *pc += 4;
}

static void emit_add(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200040; // add (1) rDst:f, rSrc0:f, rSrc1:f
    inst[1] = 0x2222201d;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8) | ((src1_reg & 0xff) << 16);
    inst[3] = 0;
    *pc += 4;
}

static void emit_sub(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200042; // sub (1) rDst:f, rSrc0:f, rSrc1:f
    inst[1] = 0x2222201d;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8) | ((src1_reg & 0xff) << 16);
    inst[3] = 0;
    *pc += 4;
}

static void emit_mul(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200041; // mul (1) rDst:f, rSrc0:f, rSrc1:f
    inst[1] = 0x2222201d;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8) | ((src1_reg & 0xff) << 16);
    inst[3] = 0;
    *pc += 4;
}

static void emit_math(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg, uint32_t math_fn) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200038; // math (1) rDst:f, rSrc0:f, rSrc1:f
    inst[1] = 0x2222201d;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8) | ((src1_reg & 0xff) << 16);
    inst[3] = math_fn;
    *pc += 4;
}

static void emit_div(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    emit_math(code, pc, dst_reg, src0_reg, src1_reg, 9); // FDIV
}

static void emit_pow(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    emit_math(code, pc, dst_reg, src0_reg, src1_reg, 10); // POW
}

static void emit_sqrt(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    emit_math(code, pc, dst_reg, src0_reg, 0, 4); // SQRT
}

static void emit_sin(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    emit_math(code, pc, dst_reg, src0_reg, 0, 6); // SIN
}

static void emit_cos(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    emit_math(code, pc, dst_reg, src0_reg, 0, 7); // COS
}

static void emit_sel(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg, uint32_t cond) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200045 | (cond << 14); // sel
    inst[1] = 0x2222201d;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8) | ((src1_reg & 0xff) << 16);
    inst[3] = 0;
    *pc += 4;
}

static void emit_min(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    emit_sel(code, pc, dst_reg, src0_reg, src1_reg, 1); // Less Than
}

static void emit_max(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg, uint32_t src1_reg) {
    emit_sel(code, pc, dst_reg, src0_reg, src1_reg, 3); // Greater Than
}

static void emit_frc(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee20004c; // frc (1) rDst:f, rSrc0:f
    inst[1] = 0x0008a200;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8);
    inst[3] = 0;
    *pc += 4;
}

static void emit_clamp01(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xef200001; // mov.sat
    inst[1] = 0x0008a200;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8);
    inst[3] = 0;
    *pc += 4;
}

static void emit_abs(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src0_reg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200001; // mov with Absolute modifier
    inst[1] = 0x4008a200;
    inst[2] = (src0_reg & 0xff) | ((dst_reg & 0xff) << 8);
    inst[3] = 0;
    *pc += 4;
}

static void emit_load_uniform(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t uniform_idx) {
    uint32_t global_subreg = 19 + uniform_idx;
    uint32_t src_reg = global_subreg / 8;
    uint32_t src_subreg = global_subreg % 8;

    uint32_t *inst = &code[*pc];
    inst[0] = 0xee200001; // mov
    inst[1] = 0x0008a200 | ((src_subreg & 7) << 6);
    inst[2] = (src_reg & 0xff) | ((dst_reg & 0xff) << 8);
    inst[3] = 0;
    *pc += 4;
}

static void emit_load_input(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t input_idx) {
    if (input_idx == SRAPI_VM_INPUT_X) {
        emit_mov_reg(code, pc, dst_reg, 18);
    } else if (input_idx == SRAPI_VM_INPUT_Y) {
        emit_mov_reg(code, pc, dst_reg, 19);
    } else if (input_idx == SRAPI_VM_INPUT_U) {
        emit_mov_reg(code, pc, dst_reg, 20);
    } else if (input_idx == SRAPI_VM_INPUT_V) {
        emit_mov_reg(code, pc, dst_reg, 21);
    } else if (input_idx == SRAPI_VM_INPUT_WIDTH) {
        uint32_t *inst = &code[*pc];
        inst[0] = 0xee200001;
        inst[1] = 0x0008a200 | (1 << 6);
        inst[2] = 2 | ((dst_reg & 0xff) << 8);
        inst[3] = 0;
        *pc += 4;
    } else if (input_idx == SRAPI_VM_INPUT_HEIGHT) {
        uint32_t *inst = &code[*pc];
        inst[0] = 0xee200001;
        inst[1] = 0x0008a200 | (2 << 6);
        inst[2] = 2 | ((dst_reg & 0xff) << 8);
        inst[3] = 0;
        *pc += 4;
    }
}

static void emit_mix(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t a_reg, uint32_t b_reg, uint32_t t_reg) {
    emit_clamp01(code, pc, 19, t_reg);
    emit_sub(code, pc, 20, b_reg, a_reg);
    emit_mul(code, pc, 20, 20, 19);
    emit_add(code, pc, dst_reg, a_reg, 20);
}

static void emit_mov_convert_u32_to_float(uint32_t *code, uint32_t *pc, uint32_t dst_reg, uint32_t src_reg, uint32_t src_subreg) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0x07200001; // GRF dst:f (7), GRF src:ud (0)
    inst[1] = 0x0008a200 | ((src_subreg & 7) << 6);
    inst[2] = (src_reg & 0xff) | ((dst_reg & 0xff) << 8);
    inst[3] = 0;
    *pc += 4;
}

static void emit_thread_end(uint32_t *code, uint32_t *pc) {
    uint32_t *inst = &code[*pc];
    inst[0] = 0x0000000c; // send
    inst[1] = 0x00000200;
    inst[2] = 0x00000000;
    inst[3] = 0x02000010; // Thread Spawner message
    *pc += 4;
}

static void emit_out_color(uint32_t *code, uint32_t *pc, uint32_t r_reg, uint32_t g_reg, uint32_t b_reg, uint32_t a_reg) {
    // 1. Multiply with saturation
    emit_mov_imm(code, pc, 19, 255.0f);
    emit_mul(code, pc, 18, r_reg, 19); // sat enabled in instruction logic? Let's just mul and clamp
    emit_clamp01(code, pc, 18, 18);
    emit_mul(code, pc, 18, 18, 19);
    
    emit_mul(code, pc, 20, g_reg, 19);
    emit_clamp01(code, pc, 20, 20);
    emit_mul(code, pc, 20, 20, 19);

    emit_mul(code, pc, 21, b_reg, 19);
    emit_clamp01(code, pc, 21, 21);
    emit_mul(code, pc, 21, 21, 19);

    emit_mul(code, pc, 22, a_reg, 19);
    emit_clamp01(code, pc, 22, 22);
    emit_mul(code, pc, 22, 22, 19);

    // 2. Convert to UD
    uint32_t *inst = &code[*pc];
    inst[0] = 0x00200001; // mov (1) r18:ud, r18:f
    inst[1] = 0x0008a200;
    inst[2] = 18 | (18 << 8);
    inst[3] = 0;
    *pc += 4;

    inst = &code[*pc];
    inst[0] = 0x00200001; // mov (1) r20:ud, r20:f
    inst[1] = 0x0008a200;
    inst[2] = 20 | (20 << 8);
    inst[3] = 0;
    *pc += 4;

    inst = &code[*pc];
    inst[0] = 0x00200001; // mov (1) r21:ud, r21:f
    inst[1] = 0x0008a200;
    inst[2] = 21 | (21 << 8);
    inst[3] = 0;
    *pc += 4;

    inst = &code[*pc];
    inst[0] = 0x00200001; // mov (1) r22:ud, r22:f
    inst[1] = 0x0008a200;
    inst[2] = 22 | (22 << 8);
    inst[3] = 0;
    *pc += 4;

    // 3. Bitwise pack into r18:ud: R | (G << 8) | (B << 16) | (A << 24)
    // shl (1) r20:ud, r20:ud, 8
    inst = &code[*pc];
    inst[0] = 0x00200059; // shl
    inst[1] = 0x0008a200;
    inst[2] = 20 | (20 << 8);
    inst[3] = 8;
    *pc += 4;

    // shl (1) r21:ud, r21:ud, 16
    inst = &code[*pc];
    inst[0] = 0x00200059; // shl
    inst[1] = 0x0008a200;
    inst[2] = 21 | (21 << 8);
    inst[3] = 16;
    *pc += 4;

    // shl (1) r22:ud, r22:ud, 24
    inst = &code[*pc];
    inst[0] = 0x00200059; // shl
    inst[1] = 0x0008a200;
    inst[2] = 22 | (22 << 8);
    inst[3] = 24;
    *pc += 4;

    // or r18, r18, r20
    inst = &code[*pc];
    inst[0] = 0x00200056; // or
    inst[1] = 0x2222201d;
    inst[2] = 18 | (18 << 8) | (20 << 16);
    inst[3] = 0;
    *pc += 4;

    // or r18, r18, r21
    inst = &code[*pc];
    inst[0] = 0x00200056; // or
    inst[1] = 0x2222201d;
    inst[2] = 18 | (18 << 8) | (21 << 16);
    inst[3] = 0;
    *pc += 4;

    // or r18, r18, r22
    inst = &code[*pc];
    inst[0] = 0x00200056; // or
    inst[1] = 0x2222201d;
    inst[2] = 18 | (18 << 8) | (22 << 16);
    inst[3] = 0;
    *pc += 4;

    // 4. Calculate memory address: pixel_address = base_address + Y * pitch + X * 4
    // Y * pitch
    inst = &code[*pc];
    inst[0] = 0x00200041; // mul
    inst[1] = 0x2222201d;
    inst[2] = 2 | (19 << 8) | (10 << 16); // r0.2 * r1.2 (pitch) -> r19:ud
    inst[3] = 0;
    *pc += 4;

    // X * 4
    inst = &code[*pc];
    inst[0] = 0x00200059; // shl
    inst[1] = 0x0008a200 | (1 << 6); // r0.1
    inst[2] = (20 << 8); // shl r0.1, 2 -> r20:ud
    inst[3] = 2;
    *pc += 4;

    // Y * pitch + X * 4
    inst = &code[*pc];
    inst[0] = 0x00200040; // add
    inst[1] = 0x2222201d;
    inst[2] = 19 | (19 << 8) | (20 << 16); // r19 + r20 -> r19
    inst[3] = 0;
    *pc += 4;

    // Branchless bounds check on Y: if Y >= height, offset = 0
    // sub (1) r20:d, r0.2:ud, r10.4:ud
    inst = &code[*pc];
    inst[0] = 0x00200042; // sub
    inst[1] = 0x2222201d | (2 << 6) | (4 << 21); // r0.2 and r10.4
    inst[2] = 10 | (20 << 8) | (0 << 16);
    inst[3] = 0;
    *pc += 4;

    // asr (1) r20:d, r20:d, 31
    inst = &code[*pc];
    inst[0] = 0x0120005a; // asr
    inst[1] = 0x0008a200;
    inst[2] = 20 | (20 << 8);
    inst[3] = 31;
    *pc += 4;

    // and (1) r19:ud, r19:ud, r20:ud
    inst = &code[*pc];
    inst[0] = 0x00200050; // and
    inst[1] = 0x2222201d;
    inst[2] = 20 | (19 << 8) | (19 << 16);
    inst[3] = 0;
    *pc += 4;

    // base_address + offset
    inst = &code[*pc];
    inst[0] = 0xfec00040; // add 64-bit UQ
    inst[1] = 0x2222201d;
    inst[2] = 8 | (19 << 8) | (19 << 16); // r1.0:uq + r19:uq -> r19:uq
    inst[3] = 0;
    *pc += 4;

    // 5. Send stateless memory write message
    // Message payload setup: we place the computed 64-bit address in r19 (bytes 0-7) and the packed color in r19 (bytes 8-11).
    // The packed color is already in r18. Let's move it to r19 subregister 2 (byte 8).
    inst = &code[*pc];
    inst[0] = 0x00200001; // mov
    inst[1] = 0x0008a200 | (2 << 16); // subreg 2
    inst[2] = 18 | (19 << 8); // mov r18 -> r19.2
    inst[3] = 0;
    *pc += 4;

    // send (1) null, r19, data port write
    inst = &code[*pc];
    inst[0] = 0x0000000c; // send
    inst[1] = 0x00000200;
    inst[2] = 19; // src r19
    inst[3] = 0x021A3000; // Message descriptor for 1 DWord write
    *pc += 4;
}

srapi_result_t srapi_i915_compile_shader(
    srapi_device_t *device,
    srapi_shader_t *shader,
    srapi_buffer_t **out_buffer
) {
    if (shader == NULL || out_buffer == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }
    *out_buffer = NULL;

    // Allocate host buffer for compiled instructions
    uint32_t *code = calloc(4096, sizeof(uint32_t));
    if (code == NULL) {
        return SRAPI_ERROR_OOM;
    }
    uint32_t pc = 0;

    // Shader Setup Calculations:
    // px (float) = convert X
    emit_mov_convert_u32_to_float(code, &pc, 18, 0, 1);
    // py (float) = convert Y
    emit_mov_convert_u32_to_float(code, &pc, 19, 0, 2);
    // u (float) = (px - x0) * inv_width_minus_1
    emit_load_uniform(code, &pc, 20, 0); // we will store x0 in r1.5 which maps to user uniform -14?
    // Let's just load parameters from r1/r2 using load_input logic:
    // sub px, x0 (r1.5) -> r20
    uint32_t *inst = &code[pc];
    inst[0] = 0xee200042; // sub
    inst[1] = 0x2222201d | (5 << 21); // subreg 5 of r1
    inst[2] = 18 | (20 << 8) | (1 << 16); // r18 - r1.5 -> r20
    inst[3] = 0;
    pc += 4;
    // mul r20, r20, inv_width_minus_1 (r1.7) -> r20
    inst = &code[pc];
    inst[0] = 0xee200041; // mul
    inst[1] = 0x2222201d | (7 << 21); // subreg 7 of r1
    inst[2] = 20 | (20 << 8) | (1 << 16); // r20 * r1.7 -> r20
    inst[3] = 0;
    pc += 4;

    // v (float) = (py - y0) * inv_height_minus_1
    // sub py, y0 (r1.6) -> r21
    inst = &code[pc];
    inst[0] = 0xee200042; // sub
    inst[1] = 0x2222201d | (6 << 21); // subreg 6 of r1
    inst[2] = 19 | (21 << 8) | (1 << 16); // r19 - r1.6 -> r21
    inst[3] = 0;
    pc += 4;
    // mul r21, r21, inv_height_minus_1 (r2.0) -> r21
    inst = &code[pc];
    inst[0] = 0xee200041; // mul
    inst[1] = 0x2222201d | (0 << 21); // subreg 0 of r2
    inst[2] = 21 | (21 << 8) | (2 << 16); // r21 * r2.0 -> r21
    inst[3] = 0;
    pc += 4;

    // Translate all bytecode instructions
    for (size_t i = 0; i < shader->inst_count; i++) {
        const srapi_vm_inst_t *inst = &shader->insts[i];
        uint32_t dst = 3 + inst->dst;
        uint32_t a = 3 + inst->a;
        uint32_t b = 3 + inst->b;

        switch (inst->op) {
            case SRAPI_VM_END:
                emit_thread_end(code, &pc);
                break;
            case SRAPI_VM_MOV:
                emit_mov_reg(code, &pc, dst, a);
                break;
            case SRAPI_VM_ADD:
                emit_add(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_SUB:
                emit_sub(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_MUL:
                emit_mul(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_DIV:
                emit_div(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_MIN:
                emit_min(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_MAX:
                emit_max(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_POW:
                emit_pow(code, &pc, dst, a, b);
                break;
            case SRAPI_VM_LOAD_INPUT:
                emit_load_input(code, &pc, dst, inst->a);
                break;
            case SRAPI_VM_LOAD_UNIFORM:
                emit_load_uniform(code, &pc, dst, inst->a);
                break;
            case SRAPI_VM_LOAD_CONST:
                {
                    float val;
                    memcpy(&val, &inst->imm, sizeof(val));
                    emit_mov_imm(code, &pc, dst, val);
                }
                break;
            case SRAPI_VM_FRACT:
                emit_frc(code, &pc, dst, a);
                break;
            case SRAPI_VM_CLAMP01:
                emit_clamp01(code, &pc, dst, a);
                break;
            case SRAPI_VM_SIN:
                emit_sin(code, &pc, dst, a);
                break;
            case SRAPI_VM_COS:
                emit_cos(code, &pc, dst, a);
                break;
            case SRAPI_VM_ABS:
                emit_abs(code, &pc, dst, a);
                break;
            case SRAPI_VM_SQRT:
                emit_sqrt(code, &pc, dst, a);
                break;
            case SRAPI_VM_MIX:
                emit_mix(code, &pc, dst, a, b, 3 + inst->c);
                break;
            case SRAPI_VM_OUT_COLOR:
                emit_out_color(code, &pc, a, b, 3 + inst->c, 3 + inst->d);
                break;
            default:
                break;
        }
    }

    if (device->fd < 0 || device->gpu_driver != 915) {
        // Return dummy compiled buffer in host memory
        srapi_buffer_t *buf = calloc(1, sizeof(*buf));
        if (buf == NULL) {
            free(code);
            return SRAPI_ERROR_OOM;
        }
        buf->device = device;
        buf->backend = SRAPI_BACKEND_GPU;
        buf->size = pc * sizeof(uint32_t);
        buf->data = code;
        *out_buffer = buf;
        return SRAPI_OK;
    }

    // Allocate GPU buffer
    srapi_buffer_t *gpu_buf = NULL;
    srapi_result_t r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = pc * sizeof(uint32_t),
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = code,
        },
        &gpu_buf
    );
    free(code);

    if (r != SRAPI_OK) {
        return r;
    }

    srapi_debugf("i915 shader compiled: insts=%zu size=%zu bytes", shader->inst_count, gpu_buf->size);
    *out_buffer = gpu_buf;
    return SRAPI_OK;
}

srapi_result_t srapi_i915_run_shader_gpu(
    srapi_device_t *device,
    srapi_buffer_t *compiled_shader,
    srapi_shader_t *shader,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t dst_handle,
    uint64_t dst_size,
    uint32_t dst_pitch
) {
    if (device == NULL || compiled_shader == NULL || shader == NULL || op == NULL ||
        device->fd < 0 || device->gpu_driver != 915) {
        return SRAPI_ERROR_UNSUPPORTED;
    }

    // 1. Allocate dynamic parameter buffer
    struct param_buffer {
        uint64_t dst_addr;
        uint32_t pitch;
        uint32_t width;
        uint32_t height;
        float x0;
        float y0;
        float inv_width_minus_1;
        float inv_height_minus_1;
        float width_f;
        float height_f;
        float uniforms[32];
    } params;

    memset(&params, 0, sizeof(params));
    params.dst_addr = 0; // Filled by relocation!
    params.pitch = dst_pitch;
    params.width = op->width;
    params.height = op->height;
    params.x0 = (float)op->x0;
    params.y0 = (float)op->y0;
    params.inv_width_minus_1 = op->width > 1 ? 1.0f / (float)(op->width - 1) : 0.0f;
    params.inv_height_minus_1 = op->height > 1 ? 1.0f / (float)(op->height - 1) : 0.0f;
    params.width_f = (float)op->width;
    params.height_f = (float)op->height;

    for (size_t i = 0; i < shader->uniform_count && i < 32; i++) {
        params.uniforms[i] = shader->uniforms[i];
    }

    srapi_buffer_t *param_buf = NULL;
    srapi_result_t r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = sizeof(params),
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = &params,
        },
        &param_buf
    );
    if (r != SRAPI_OK) {
        return r;
    }

    // 2. Build GPGPU Walker batch buffer
    uint32_t batch_words[128];
    memset(batch_words, 0, sizeof(batch_words));
    int idx = 0;

    batch_words[idx++] = 0x69000004; // PIPELINE_SELECT (Media)

    int state_base_addr_idx = idx;
    batch_words[idx++] = 0x61010011; // STATE_BASE_ADDRESS
    batch_words[idx++] = 0x00000001; // General State
    batch_words[idx++] = 0x00000001; // Surface State
    batch_words[idx++] = 0x00000001; // Dynamic State
    batch_words[idx++] = 0x00000001; // Indirect Object
    batch_words[idx++] = 0x00000001; // Instruction Base (Relocated to compiled_shader)
    batch_words[idx++] = 0xfffff001;
    batch_words[idx++] = 0xfffff001;
    batch_words[idx++] = 0xfffff001;
    batch_words[idx++] = 0xfffff001;
    batch_words[idx++] = 0xfffff001;
    while (idx < state_base_addr_idx + 19) {
        batch_words[idx++] = 0;
    }

    batch_words[idx++] = 0x70000007; // MEDIA_VFE_STATE
    batch_words[idx++] = 0;
    batch_words[idx++] = 0x00A00000;
    batch_words[idx++] = 0;
    batch_words[idx++] = 0;
    batch_words[idx++] = 0;
    batch_words[idx++] = 0;
    batch_words[idx++] = 0;
    batch_words[idx++] = 0;

    int interface_desc_idx = idx;
    idx += 8; // Reserved for embedded Interface Descriptor table

    batch_words[idx++] = 0x70020002; // MEDIA_INTERFACE_DESCRIPTOR_LOAD
    batch_words[idx++] = 0;
    batch_words[idx++] = 32;
    batch_words[idx++] = 0; // GPU address of interface descriptor (relocated to batch buffer)

    uint32_t start_group_x = x / 16;
    uint32_t start_group_y = y / 16;
    uint32_t group_width = (x + width + 15) / 16 - start_group_x;
    uint32_t group_height = (y + height + 15) / 16 - start_group_y;

    int walker_idx = idx;
    batch_words[idx++] = 0x7105000d; // GPGPU_WALKER
    batch_words[idx++] = 0; // ID
    batch_words[idx++] = sizeof(params); // Indirect Data length
    batch_words[idx++] = 0; // Indirect Data Address (relocated to param_buf)
    batch_words[idx++] = 0xC0000000; // SIMD8
    batch_words[idx++] = start_group_x; // Thread Group ID Starting X
    batch_words[idx++] = group_width; // Thread Group ID X Dimension
    batch_words[idx++] = start_group_y; // Thread Group ID Starting Y
    batch_words[idx++] = group_height; // Thread Group ID Y Dimension
    batch_words[idx++] = 0; // Thread Group ID Starting Z
    batch_words[idx++] = 1; // Thread Group ID Z Dimension
    batch_words[idx++] = 0xffffffff; // Right Execution Mask
    batch_words[idx++] = 0xffffffff; // Bottom Execution Mask
    batch_words[idx++] = 15 | (15 << 10) | (0 << 20); // Local X/Y/Z Dimension (16x16x1)
    batch_words[idx++] = 0;

    batch_words[idx++] = SRAPI_I915_MI_BATCH_BUFFER_END;

    // Interface Descriptor Table
    batch_words[interface_desc_idx + 0] = 0; // Kernel Start Pointer
    batch_words[interface_desc_idx + 1] = 0;
    batch_words[interface_desc_idx + 2] = 0;
    batch_words[interface_desc_idx + 3] = 0;
    batch_words[interface_desc_idx + 4] = 2; // Load constants
    batch_words[interface_desc_idx + 5] = 0;
    batch_words[interface_desc_idx + 6] = 0;
    batch_words[interface_desc_idx + 7] = 0;

    srapi_buffer_t *batch = NULL;
    r = srapi_i915_create_buffer(
        device,
        &(srapi_buffer_desc_t){
            .size = idx * sizeof(uint32_t),
            .usage = SRAPI_BUFFER_STORAGE,
            .initial_data = batch_words,
        },
        &batch
    );
    if (r != SRAPI_OK) {
        srapi_gpu_destroy_buffer(param_buf);
        free(param_buf);
        return r;
    }

    // 3. Set up relocations
    srapi_i915_relocation_t param_reloc;
    memset(&param_reloc, 0, sizeof(param_reloc));
    param_reloc.target_index = 0; // Destination buffer
    param_reloc.offset = 0;
    param_reloc.read_domains = SRAPI_I915_DOMAIN_RENDER;
    param_reloc.write_domain = SRAPI_I915_DOMAIN_RENDER;

    srapi_i915_relocation_t batch_relocs[3];
    memset(batch_relocs, 0, sizeof(batch_relocs));

    // reloc[0]: Instruction Base Address
    batch_relocs[0].target_index = 1; // compiled_shader
    batch_relocs[0].offset = (state_base_addr_idx + 5) * sizeof(uint32_t);
    batch_relocs[0].read_domains = SRAPI_I915_DOMAIN_INSTRUCTION;

    // reloc[1]: Interface Descriptor Table
    batch_relocs[1].target_index = 3; // batch itself
    batch_relocs[1].offset = (interface_desc_idx + 8 + 3) * sizeof(uint32_t);
    batch_relocs[1].delta = interface_desc_idx * sizeof(uint32_t);
    batch_relocs[1].read_domains = SRAPI_I915_DOMAIN_INSTRUCTION;

    // reloc[2]: Indirect Data Address
    batch_relocs[2].target_index = 2; // param_buf
    batch_relocs[2].offset = (walker_idx + 3) * sizeof(uint32_t);
    batch_relocs[2].read_domains = SRAPI_I915_DOMAIN_INSTRUCTION;

    // 4. Construct executive objects
    srapi_buffer_t dst_buffer;
    memset(&dst_buffer, 0, sizeof(dst_buffer));
    dst_buffer.device = device;
    dst_buffer.backend = SRAPI_BACKEND_GPU;
    dst_buffer.gpu_handle = dst_handle;
    dst_buffer.gpu_size = dst_size;
    dst_buffer.size = dst_size;
    dst_buffer.gpu_memory = 1;

    srapi_i915_exec_object_t objs[4];
    memset(objs, 0, sizeof(objs));

    objs[0].buffer = &dst_buffer;
    objs[0].alignment = 64;
    objs[0].flags = SRAPI_I915_OBJECT_WRITE | SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

    objs[1].buffer = compiled_shader;
    objs[1].alignment = 64;
    objs[1].flags = SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

    objs[2].buffer = param_buf;
    objs[2].relocations = &param_reloc;
    objs[2].relocation_count = 1;
    objs[2].alignment = 64;
    objs[2].flags = SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

    objs[3].buffer = batch;
    objs[3].relocations = batch_relocs;
    objs[3].relocation_count = 3;
    objs[3].flags = SRAPI_I915_OBJECT_SUPPORTS_48B_ADDRESS;

    srapi_i915_exec_desc_t exec;
    memset(&exec, 0, sizeof(exec));
    exec.batch = batch;
    exec.batch_len = idx * sizeof(uint32_t);
    exec.flags = SRAPI_I915_EXEC_RENDER;
    exec.objects = objs;
    exec.object_count = 4;

    r = srapi_i915_exec(device, &exec);
    srapi_debugf("i915 shader run: %ux%u at %u,%u dst_handle=%u exec_res=%d", width, height, x, y, dst_handle, r);

    srapi_gpu_destroy_buffer(batch);
    free(batch);
    srapi_gpu_destroy_buffer(param_buf);
    free(param_buf);

    return r;
}
