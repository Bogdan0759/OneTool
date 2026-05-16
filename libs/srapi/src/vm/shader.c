#include "internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *vm_op_name(uint8_t op) {
    switch (op) {
        case SRAPI_VM_END: return "END";
        case SRAPI_VM_MOV: return "MOV";
        case SRAPI_VM_ADD: return "ADD";
        case SRAPI_VM_SUB: return "SUB";
        case SRAPI_VM_MUL: return "MUL";
        case SRAPI_VM_DIV: return "DIV";
        case SRAPI_VM_MIN: return "MIN";
        case SRAPI_VM_MAX: return "MAX";
        case SRAPI_VM_LOAD_INPUT: return "LOAD_INPUT";
        case SRAPI_VM_LOAD_UNIFORM: return "LOAD_UNIFORM";
        case SRAPI_VM_OUT_COLOR: return "OUT_COLOR";
        case SRAPI_VM_LOAD_CONST: return "LOAD_CONST";
        case SRAPI_VM_FRACT: return "FRACT";
        case SRAPI_VM_CLAMP01: return "CLAMP01";
        case SRAPI_VM_MIX: return "MIX";
        default: return "?";
    }
}

static srapi_result_t vm_read_arg(
    const uint32_t *bytecode,
    size_t word_count,
    size_t *pc,
    uint8_t *out
) {
    uint32_t value;

    if (*pc >= word_count) {
        return SRAPI_ERROR_BAD_ARG;
    }

    value = bytecode[(*pc)++];
    if (value > UINT8_MAX) {
        srapi_set_error("shader: arg %u does not fit uint8", value);
        return SRAPI_ERROR_BAD_ARG;
    }

    *out = (uint8_t)value;
    return SRAPI_OK;
}

static srapi_result_t vm_decode_bytecode(
    const uint32_t *bytecode,
    size_t word_count,
    srapi_vm_inst_t **out_insts,
    size_t *out_count
) {
    srapi_vm_inst_t *insts;
    size_t pc = 0;
    size_t count = 0;

    insts = calloc(word_count, sizeof(*insts));
    if (insts == NULL) {
        return SRAPI_ERROR_OOM;
    }

    while (pc < word_count) {
        srapi_vm_inst_t *inst = &insts[count];
        uint32_t op = bytecode[pc++];

        if (op > UINT8_MAX) {
            srapi_set_error("shader: opcode %u does not fit uint8 at word %zu", op, pc - 1);
            free(insts);
            return SRAPI_ERROR_BAD_ARG;
        }

        inst->op = (uint8_t)op;
        switch (inst->op) {
            case SRAPI_VM_END:
                count++;
                pc = word_count;
                break;
            case SRAPI_VM_MOV:
            case SRAPI_VM_LOAD_INPUT:
            case SRAPI_VM_LOAD_UNIFORM:
            case SRAPI_VM_FRACT:
            case SRAPI_VM_CLAMP01:
                if (vm_read_arg(bytecode, word_count, &pc, &inst->dst) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->a) != SRAPI_OK) {
                    free(insts);
                    return SRAPI_ERROR_BAD_ARG;
                }
                count++;
                break;
            case SRAPI_VM_LOAD_CONST:
                if (vm_read_arg(bytecode, word_count, &pc, &inst->dst) != SRAPI_OK) {
                    free(insts);
                    return SRAPI_ERROR_BAD_ARG;
                }
                if (pc >= word_count) {
                    free(insts);
                    return SRAPI_ERROR_BAD_ARG;
                }
                inst->imm = bytecode[pc++];
                count++;
                break;
            case SRAPI_VM_ADD:
            case SRAPI_VM_SUB:
            case SRAPI_VM_MUL:
            case SRAPI_VM_DIV:
            case SRAPI_VM_MIN:
            case SRAPI_VM_MAX:
                if (vm_read_arg(bytecode, word_count, &pc, &inst->dst) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->a) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->b) != SRAPI_OK) {
                    free(insts);
                    return SRAPI_ERROR_BAD_ARG;
                }
                count++;
                break;
            case SRAPI_VM_MIX:
                if (vm_read_arg(bytecode, word_count, &pc, &inst->dst) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->a) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->b) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->c) != SRAPI_OK) {
                    free(insts);
                    return SRAPI_ERROR_BAD_ARG;
                }
                count++;
                break;
            case SRAPI_VM_OUT_COLOR:
                if (vm_read_arg(bytecode, word_count, &pc, &inst->a) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->b) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->c) != SRAPI_OK ||
                    vm_read_arg(bytecode, word_count, &pc, &inst->d) != SRAPI_OK) {
                    free(insts);
                    return SRAPI_ERROR_BAD_ARG;
                }
                count++;
                break;
            default:
                srapi_set_error("shader: unknown opcode %u at word %zu", op, pc - 1);
                free(insts);
                return SRAPI_ERROR_BAD_ARG;
        }
    }

    *out_insts = insts;
    *out_count = count;
    return SRAPI_OK;
}

static void vm_disasm_shader(const srapi_shader_t *shader) {
    for (size_t i = 0; i < shader->inst_count; i++) {
        const srapi_vm_inst_t *inst = &shader->insts[i];

        switch (inst->op) {
            case SRAPI_VM_END:
                srapi_debugf("shader inst[%zu] %s", i, vm_op_name(inst->op));
                break;
            case SRAPI_VM_MOV:
            case SRAPI_VM_LOAD_INPUT:
            case SRAPI_VM_LOAD_UNIFORM:
            case SRAPI_VM_FRACT:
            case SRAPI_VM_CLAMP01:
                srapi_debugf("shader inst[%zu] %s dst=%u a=%u",
                             i, vm_op_name(inst->op), inst->dst, inst->a);
                break;
            case SRAPI_VM_LOAD_CONST:
                srapi_debugf("shader inst[%zu] %s dst=%u imm=0x%08x",
                             i, vm_op_name(inst->op), inst->dst, inst->imm);
                break;
            case SRAPI_VM_MIX:
                srapi_debugf("shader inst[%zu] %s dst=%u a=%u b=%u t=%u",
                             i, vm_op_name(inst->op), inst->dst, inst->a, inst->b, inst->c);
                break;
            case SRAPI_VM_OUT_COLOR:
                srapi_debugf("shader inst[%zu] %s rgba=%u,%u,%u,%u",
                             i, vm_op_name(inst->op), inst->a, inst->b, inst->c, inst->d);
                break;
            default:
                srapi_debugf("shader inst[%zu] %s dst=%u a=%u b=%u",
                             i, vm_op_name(inst->op), inst->dst, inst->a, inst->b);
                break;
        }
    }
}

srapi_result_t srapi_create_shader(
    const uint32_t *bytecode,
    size_t word_count,
    const float *uniforms,
    size_t uniform_count,
    srapi_shader_t **out
) {
    srapi_shader_t *shader;

    if (bytecode == NULL || word_count == 0 || out == NULL) {
        return SRAPI_ERROR_BAD_ARG;
    }

    shader = calloc(1, sizeof(*shader));
    if (shader == NULL) {
        return SRAPI_ERROR_OOM;
    }

    shader->bytecode = calloc(word_count, sizeof(*shader->bytecode));
    if (shader->bytecode == NULL) {
        free(shader);
        return SRAPI_ERROR_OOM;
    }
    memcpy(shader->bytecode, bytecode, word_count * sizeof(*shader->bytecode));
    shader->word_count = word_count;

    if (vm_decode_bytecode(bytecode, word_count, &shader->insts, &shader->inst_count) != SRAPI_OK) {
        free(shader->bytecode);
        free(shader);
        return SRAPI_ERROR_BAD_ARG;
    }

    if (uniform_count > 0) {
        shader->uniforms = calloc(uniform_count, sizeof(*shader->uniforms));
        if (shader->uniforms == NULL) {
            free(shader->insts);
            free(shader->bytecode);
            free(shader);
            return SRAPI_ERROR_OOM;
        }
        if (uniforms != NULL) {
            memcpy(shader->uniforms, uniforms, uniform_count * sizeof(*shader->uniforms));
        }
        shader->uniform_count = uniform_count;
    }

    srapi_debugf("shader create words=%zu insts=%zu uniforms=%zu",
                 word_count, shader->inst_count, uniform_count);
    vm_disasm_shader(shader);
    *out = shader;
    return SRAPI_OK;
}

void srapi_destroy_shader(srapi_shader_t *shader) {
    if (shader == NULL) {
        return;
    }
    srapi_debugf("shader destroy words=%zu insts=%zu uniforms=%zu runs=%zu",
                 shader->word_count, shader->inst_count, shader->uniform_count, shader->run_count);
    free(shader->insts);
    free(shader->bytecode);
    free(shader->uniforms);
    free(shader);
}

srapi_result_t srapi_shader_set_uniform(srapi_shader_t *shader, size_t index, float value) {
    if (shader == NULL || index >= shader->uniform_count) {
        return SRAPI_ERROR_BAD_ARG;
    }

    shader->uniforms[index] = value;
    return SRAPI_OK;
}

srapi_result_t srapi_shader_set_uniforms(
    srapi_shader_t *shader,
    size_t first,
    const float *values,
    size_t count
) {
    if (shader == NULL || (values == NULL && count > 0)) {
        return SRAPI_ERROR_BAD_ARG;
    }
    if (first > shader->uniform_count || count > shader->uniform_count - first) {
        return SRAPI_ERROR_BAD_ARG;
    }

    memcpy(shader->uniforms + first, values, count * sizeof(*shader->uniforms));
    return SRAPI_OK;
}
