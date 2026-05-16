#include "internal.h"

#include <string.h>

#define SRAPI_VM_REG_COUNT 16
#define SRAPI_VM_INPUT_COUNT 6
#define SRAPI_VM_VERTEX_INPUT_COUNT 7

static float vm_get_reg(const float regs[SRAPI_VM_REG_COUNT], uint32_t index) {
    return index < SRAPI_VM_REG_COUNT ? regs[index] : 0.0f;
}

static float vm_u32_to_float(uint32_t bits) {
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float vm_clamp01(float value) {
    if (value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value;
}

static float vm_fract(float value) {
    int whole = (int)value;
    float out = value - (float)whole;

    if (out < 0.0f) {
        out += 1.0f;
    }
    return out;
}

static uint8_t clamp_byte(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

srapi_result_t srapi_vm_run_fragment(
    srapi_shader_t *shader,
    const float inputs[6],
    srapi_color_t *out_color
) {
    float regs[SRAPI_VM_REG_COUNT];
    int trace;

    if (shader == NULL || shader->insts == NULL || out_color == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(regs, 0, sizeof(regs));
    *out_color = srapi_rgba(255, 0, 255, 255);
    trace = shader->run_count < 2;

    if (trace) {
        srapi_debugf("vm run[%zu] in x=%.2f y=%.2f u=%.3f v=%.3f w=%.2f h=%.2f",
                     shader->run_count,
                     inputs[SRAPI_VM_INPUT_X], inputs[SRAPI_VM_INPUT_Y],
                     inputs[SRAPI_VM_INPUT_U], inputs[SRAPI_VM_INPUT_V],
                     inputs[SRAPI_VM_INPUT_WIDTH], inputs[SRAPI_VM_INPUT_HEIGHT]);
    }
    shader->run_count++;

    for (size_t pc = 0; pc < shader->inst_count; pc++) {
        const srapi_vm_inst_t *inst = &shader->insts[pc];
        uint8_t op = inst->op;

        switch (op) {
            case SRAPI_VM_END:
                return SRAPI_OK;
            case SRAPI_VM_MOV:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_get_reg(regs, inst->a);
                }
                break;
            case SRAPI_VM_ADD:
            case SRAPI_VM_SUB:
            case SRAPI_VM_MUL:
            case SRAPI_VM_DIV:
            case SRAPI_VM_MIN:
            case SRAPI_VM_MAX:
                if (inst->dst >= SRAPI_VM_REG_COUNT) break;
                if (op == SRAPI_VM_ADD) regs[inst->dst] = vm_get_reg(regs, inst->a) + vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_SUB) regs[inst->dst] = vm_get_reg(regs, inst->a) - vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_MUL) regs[inst->dst] = vm_get_reg(regs, inst->a) * vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_DIV) regs[inst->dst] = vm_get_reg(regs, inst->b) != 0.0f ? vm_get_reg(regs, inst->a) / vm_get_reg(regs, inst->b) : 0.0f;
                else if (op == SRAPI_VM_MIN) regs[inst->dst] = vm_get_reg(regs, inst->a) < vm_get_reg(regs, inst->b) ? vm_get_reg(regs, inst->a) : vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_MAX) regs[inst->dst] = vm_get_reg(regs, inst->a) > vm_get_reg(regs, inst->b) ? vm_get_reg(regs, inst->a) : vm_get_reg(regs, inst->b);
                break;
            case SRAPI_VM_LOAD_INPUT:
                if (inst->dst < SRAPI_VM_REG_COUNT && inst->a < SRAPI_VM_INPUT_COUNT) {
                    regs[inst->dst] = inputs[inst->a];
                }
                break;
            case SRAPI_VM_LOAD_UNIFORM:
                if (inst->dst < SRAPI_VM_REG_COUNT && inst->a < shader->uniform_count) {
                    regs[inst->dst] = shader->uniforms[inst->a];
                }
                break;
            case SRAPI_VM_LOAD_CONST:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_u32_to_float(inst->imm);
                }
                break;
            case SRAPI_VM_FRACT:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_fract(vm_get_reg(regs, inst->a));
                }
                break;
            case SRAPI_VM_CLAMP01:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_clamp01(vm_get_reg(regs, inst->a));
                }
                break;
            case SRAPI_VM_MIX:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    float a = vm_get_reg(regs, inst->a);
                    float b = vm_get_reg(regs, inst->b);
                    float t = vm_clamp01(vm_get_reg(regs, inst->c));

                    regs[inst->dst] = a + (b - a) * t;
                }
                break;
            case SRAPI_VM_OUT_COLOR:
                *out_color = srapi_rgba(
                    clamp_byte(vm_get_reg(regs, inst->a)),
                    clamp_byte(vm_get_reg(regs, inst->b)),
                    clamp_byte(vm_get_reg(regs, inst->c)),
                    clamp_byte(vm_get_reg(regs, inst->d))
                );
                if (trace) {
                    srapi_debugf("vm run[%zu] out rgba=%08x",
                                 shader->run_count - 1, (unsigned int)*out_color);
                }
                break;
            default:
                srapi_set_error("vm: unknown opcode %u at inst %zu", op, pc);
                return SRAPI_ERROR_BAD_ARG;
        }
    }

    return SRAPI_OK;
}

srapi_result_t srapi_vm_run_vertex(
    srapi_shader_t *shader,
    const float inputs[7],
    float out_position[2],
    srapi_color_t *out_color
) {
    float regs[SRAPI_VM_REG_COUNT];
    int trace;

    if (shader == NULL || shader->insts == NULL || out_position == NULL || out_color == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(regs, 0, sizeof(regs));
    out_position[0] = inputs[SRAPI_VM_INPUT_VERTEX_X];
    out_position[1] = inputs[SRAPI_VM_INPUT_VERTEX_Y];
    *out_color = srapi_rgba(
        clamp_byte(inputs[SRAPI_VM_INPUT_VERTEX_R]),
        clamp_byte(inputs[SRAPI_VM_INPUT_VERTEX_G]),
        clamp_byte(inputs[SRAPI_VM_INPUT_VERTEX_B]),
        clamp_byte(inputs[SRAPI_VM_INPUT_VERTEX_A])
    );
    trace = shader->run_count < 2;

    if (trace) {
        srapi_debugf("vm vertex run[%zu] in x=%.2f y=%.2f rgba=%.2f %.2f %.2f %.2f index=%.0f",
                     shader->run_count,
                     inputs[SRAPI_VM_INPUT_VERTEX_X], inputs[SRAPI_VM_INPUT_VERTEX_Y],
                     inputs[SRAPI_VM_INPUT_VERTEX_R], inputs[SRAPI_VM_INPUT_VERTEX_G],
                     inputs[SRAPI_VM_INPUT_VERTEX_B], inputs[SRAPI_VM_INPUT_VERTEX_A],
                     inputs[SRAPI_VM_INPUT_VERTEX_INDEX]);
    }
    shader->run_count++;

    for (size_t pc = 0; pc < shader->inst_count; pc++) {
        const srapi_vm_inst_t *inst = &shader->insts[pc];
        uint8_t op = inst->op;

        switch (op) {
            case SRAPI_VM_END:
                return SRAPI_OK;
            case SRAPI_VM_MOV:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_get_reg(regs, inst->a);
                }
                break;
            case SRAPI_VM_ADD:
            case SRAPI_VM_SUB:
            case SRAPI_VM_MUL:
            case SRAPI_VM_DIV:
            case SRAPI_VM_MIN:
            case SRAPI_VM_MAX:
                if (inst->dst >= SRAPI_VM_REG_COUNT) break;
                if (op == SRAPI_VM_ADD) regs[inst->dst] = vm_get_reg(regs, inst->a) + vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_SUB) regs[inst->dst] = vm_get_reg(regs, inst->a) - vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_MUL) regs[inst->dst] = vm_get_reg(regs, inst->a) * vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_DIV) regs[inst->dst] = vm_get_reg(regs, inst->b) != 0.0f ? vm_get_reg(regs, inst->a) / vm_get_reg(regs, inst->b) : 0.0f;
                else if (op == SRAPI_VM_MIN) regs[inst->dst] = vm_get_reg(regs, inst->a) < vm_get_reg(regs, inst->b) ? vm_get_reg(regs, inst->a) : vm_get_reg(regs, inst->b);
                else if (op == SRAPI_VM_MAX) regs[inst->dst] = vm_get_reg(regs, inst->a) > vm_get_reg(regs, inst->b) ? vm_get_reg(regs, inst->a) : vm_get_reg(regs, inst->b);
                break;
            case SRAPI_VM_LOAD_INPUT:
                if (inst->dst < SRAPI_VM_REG_COUNT && inst->a < SRAPI_VM_VERTEX_INPUT_COUNT) {
                    regs[inst->dst] = inputs[inst->a];
                }
                break;
            case SRAPI_VM_LOAD_UNIFORM:
                if (inst->dst < SRAPI_VM_REG_COUNT && inst->a < shader->uniform_count) {
                    regs[inst->dst] = shader->uniforms[inst->a];
                }
                break;
            case SRAPI_VM_LOAD_CONST:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_u32_to_float(inst->imm);
                }
                break;
            case SRAPI_VM_FRACT:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_fract(vm_get_reg(regs, inst->a));
                }
                break;
            case SRAPI_VM_CLAMP01:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    regs[inst->dst] = vm_clamp01(vm_get_reg(regs, inst->a));
                }
                break;
            case SRAPI_VM_MIX:
                if (inst->dst < SRAPI_VM_REG_COUNT) {
                    float a = vm_get_reg(regs, inst->a);
                    float b = vm_get_reg(regs, inst->b);
                    float t = vm_clamp01(vm_get_reg(regs, inst->c));

                    regs[inst->dst] = a + (b - a) * t;
                }
                break;
            case SRAPI_VM_OUT_POSITION:
                out_position[0] = vm_get_reg(regs, inst->a);
                out_position[1] = vm_get_reg(regs, inst->b);
                if (trace) {
                    srapi_debugf("vm vertex run[%zu] out pos=%.2f,%.2f",
                                 shader->run_count - 1, out_position[0], out_position[1]);
                }
                break;
            case SRAPI_VM_OUT_COLOR:
                *out_color = srapi_rgba(
                    clamp_byte(vm_get_reg(regs, inst->a)),
                    clamp_byte(vm_get_reg(regs, inst->b)),
                    clamp_byte(vm_get_reg(regs, inst->c)),
                    clamp_byte(vm_get_reg(regs, inst->d))
                );
                if (trace) {
                    srapi_debugf("vm vertex run[%zu] out rgba=%08x",
                                 shader->run_count - 1, (unsigned int)*out_color);
                }
                break;
            default:
                srapi_set_error("vm: unknown opcode %u at inst %zu", op, pc);
                return SRAPI_ERROR_BAD_ARG;
        }
    }

    return SRAPI_OK;
}
