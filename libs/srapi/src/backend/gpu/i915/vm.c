#include "vm.h"

#include <string.h>

#define SRAPI_I915_VM_REG_COUNT 16

static float reg_get(const float regs[SRAPI_I915_VM_REG_COUNT], uint32_t index) {
    return index < SRAPI_I915_VM_REG_COUNT ? regs[index] : 0.0f;
}

static float u32_to_float(uint32_t bits) {
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float clamp01(float value) {
    if (value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value;
}

static float fractf_local(float value) {
    int whole = (int)value;
    float out = value - (float)whole;

    return out < 0.0f ? out + 1.0f : out;
}

static uint8_t byte_from_float(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static srapi_result_t run_fragment_gpu_vm(
    srapi_shader_t *shader,
    const float inputs[6],
    srapi_color_t *out_color
) {
    float regs[SRAPI_I915_VM_REG_COUNT];

    if (shader == NULL || shader->insts == NULL || out_color == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memset(regs, 0, sizeof(regs));
    *out_color = srapi_rgba(255, 0, 255, 255);

    for (size_t pc = 0; pc < shader->inst_count; pc++) {
        const srapi_vm_inst_t *inst = &shader->insts[pc];

        switch (inst->op) {
            case SRAPI_VM_END:
                return SRAPI_OK;
            case SRAPI_VM_MOV:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    regs[inst->dst] = reg_get(regs, inst->a);
                }
                break;
            case SRAPI_VM_ADD:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) regs[inst->dst] = reg_get(regs, inst->a) + reg_get(regs, inst->b);
                break;
            case SRAPI_VM_SUB:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) regs[inst->dst] = reg_get(regs, inst->a) - reg_get(regs, inst->b);
                break;
            case SRAPI_VM_MUL:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) regs[inst->dst] = reg_get(regs, inst->a) * reg_get(regs, inst->b);
                break;
            case SRAPI_VM_DIV:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    float b = reg_get(regs, inst->b);
                    regs[inst->dst] = b != 0.0f ? reg_get(regs, inst->a) / b : 0.0f;
                }
                break;
            case SRAPI_VM_MIN:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    float a = reg_get(regs, inst->a);
                    float b = reg_get(regs, inst->b);
                    regs[inst->dst] = a < b ? a : b;
                }
                break;
            case SRAPI_VM_MAX:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    float a = reg_get(regs, inst->a);
                    float b = reg_get(regs, inst->b);
                    regs[inst->dst] = a > b ? a : b;
                }
                break;
            case SRAPI_VM_LOAD_INPUT:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT && inst->a < 6) {
                    regs[inst->dst] = inputs[inst->a];
                }
                break;
            case SRAPI_VM_LOAD_UNIFORM:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT && inst->a < shader->uniform_count) {
                    regs[inst->dst] = shader->uniforms[inst->a];
                }
                break;
            case SRAPI_VM_LOAD_CONST:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    regs[inst->dst] = u32_to_float(inst->imm);
                }
                break;
            case SRAPI_VM_FRACT:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    regs[inst->dst] = fractf_local(reg_get(regs, inst->a));
                }
                break;
            case SRAPI_VM_CLAMP01:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    regs[inst->dst] = clamp01(reg_get(regs, inst->a));
                }
                break;
            case SRAPI_VM_MIX:
                if (inst->dst < SRAPI_I915_VM_REG_COUNT) {
                    float a = reg_get(regs, inst->a);
                    float b = reg_get(regs, inst->b);
                    float t = clamp01(reg_get(regs, inst->c));
                    regs[inst->dst] = a + (b - a) * t;
                }
                break;
            case SRAPI_VM_OUT_COLOR:
                *out_color = srapi_rgba(
                    byte_from_float(reg_get(regs, inst->a)),
                    byte_from_float(reg_get(regs, inst->b)),
                    byte_from_float(reg_get(regs, inst->c)),
                    byte_from_float(reg_get(regs, inst->d))
                );
                break;
            default:
                srapi_set_error("i915 vm: opcode %u is not implemented", inst->op);
                return SRAPI_ERROR_UNSUPPORTED;
        }
    }

    return SRAPI_OK;
}

static srapi_result_t shade_pixels(
    uint32_t *pixels,
    uint32_t pitch,
    srapi_shader_t *shader,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    if (pixels == NULL || pitch == 0 || shader == NULL || op == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    for (uint32_t py = y; py < y + height; py++) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)py * pitch);

        for (uint32_t px = x; px < x + width; px++) {
            srapi_color_t color;
            float inputs[6];
            srapi_result_t r;

            inputs[SRAPI_VM_INPUT_X] = (float)px;
            inputs[SRAPI_VM_INPUT_Y] = (float)py;
            inputs[SRAPI_VM_INPUT_U] = op->width > 1 ? (float)((int32_t)px - op->x0) / (float)(op->width - 1) : 0.0f;
            inputs[SRAPI_VM_INPUT_V] = op->height > 1 ? (float)((int32_t)py - op->y0) / (float)(op->height - 1) : 0.0f;
            inputs[SRAPI_VM_INPUT_WIDTH] = (float)op->width;
            inputs[SRAPI_VM_INPUT_HEIGHT] = (float)op->height;

            r = run_fragment_gpu_vm(shader, inputs, &color);
            if (r != SRAPI_OK) {
                return r;
            }
            row[px] = color;
        }
    }

    return SRAPI_OK;
}

srapi_result_t srapi_i915_vm_shade_image(
    srapi_device_t *device,
    srapi_image_t *target,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    (void)device;

    if (target == NULL || target->data == NULL || op == NULL || op->shader == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_debugf("i915 vm shade image mapped %u,%u %ux%u", x, y, width, height);
    return shade_pixels(target->data, target->pitch, op->shader, op, x, y, width, height);
}

srapi_result_t srapi_i915_vm_shade_framebuffer(
    srapi_device_t *device,
    srapi_framebuffer_t *target,
    const srapi_command_t *op,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height
) {
    (void)device;

    if (target == NULL || target->pixels == NULL || op == NULL || op->shader == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    srapi_debugf("i915 vm shade framebuffer mapped %u,%u %ux%u", x, y, width, height);
    return shade_pixels(target->pixels, target->pitch, op->shader, op, x, y, width, height);
}
