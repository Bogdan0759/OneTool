#include "../src/internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int is_truthy(const js_runtime_value_t *v) {
    switch (v->kind) {
        case JS_RT_BOOL: return v->as.boolean;
        case JS_RT_NULL:
        case JS_RT_UNDEFINED: return 0;
        case JS_RT_NUMBER: return v->as.number != 0.0 && !isnan(v->as.number);
        case JS_RT_STRING: return v->as.string != NULL && v->as.string[0] != '\0';
        case JS_RT_FUNCTION:
        case JS_RT_NATIVE: return 1;
        default: return 0;
    }
}

static void vm_set_error(js_vm_t *vm, const char *msg) {
    free(vm->error);
    vm->error = js_strdup(msg);
}

static int vm_push(js_vm_t *vm, js_runtime_value_t value) {
    if (vm->stack_count >= JS_VM_STACK_MAX) {
        vm_set_error(vm, "vm stack overflow");
        return -1;
    }
    vm->stack[vm->stack_count++] = value;
    return 0;
}

static js_runtime_value_t vm_pop(js_vm_t *vm) {
    if (vm->stack_count <= 0) return js_runtime_make_undefined();
    return vm->stack[--vm->stack_count];
}

static js_runtime_value_t *vm_peek(js_vm_t *vm, int depth) {
    int idx = vm->stack_count - 1 - depth;
    if (idx < 0) return NULL;
    return &vm->stack[idx];
}

static uint8_t read_u8(js_call_frame_t *frame) {
    return frame->function->chunk.code[frame->ip++];
}

static uint16_t read_u16(js_call_frame_t *frame) {
    uint16_t hi = frame->function->chunk.code[frame->ip++];
    uint16_t lo = frame->function->chunk.code[frame->ip++];
    return (uint16_t)((hi << 8) | lo);
}

static int val_to_number(const js_runtime_value_t *v, double *out) {
    if (v->kind == JS_RT_NUMBER) { *out = v->as.number; return 0; }
    if (v->kind == JS_RT_BOOL) { *out = v->as.boolean ? 1.0 : 0.0; return 0; }
    if (v->kind == JS_RT_NULL) { *out = 0.0; return 0; }
    return -1;
}

void js_vm_init(js_vm_t *vm, js_global_env_t *globals) {
    memset(vm, 0, sizeof(*vm));
    vm->globals = globals;
}

void js_vm_deinit(js_vm_t *vm) {
    if (vm == NULL) return;
    for (int i = 0; i < vm->stack_count; i++) js_runtime_value_free(&vm->stack[i]);
    free(vm->error);
}

static int call_function(js_vm_t *vm, js_function_t *fn, int argc) {
    if (argc != fn->arity) {
        vm_set_error(vm, "wrong argument count");
        return -1;
    }
    if (vm->frame_count >= JS_VM_FRAMES_MAX) {
        vm_set_error(vm, "too many call frames");
        return -1;
    }
    js_call_frame_t *frame = &vm->frames[vm->frame_count++];
    frame->function = fn;
    frame->ip = 0;
    frame->slot_base = vm->stack_count - argc - 1;
    return 0;
}

static int call_value(js_vm_t *vm, js_runtime_value_t callee, int argc) {
    if (callee.kind == JS_RT_FUNCTION) {
        return call_function(vm, callee.as.function, argc);
    }
    if (callee.kind == JS_RT_NATIVE) {
        js_runtime_value_t result = callee.as.native->call(callee.as.native, argc,
            &vm->stack[vm->stack_count - argc], &vm->error);
        for (int i = 0; i < argc; i++) {
            js_runtime_value_free(&vm->stack[vm->stack_count - 1]);
            vm->stack_count--;
        }
        js_runtime_value_free(&vm->stack[vm->stack_count - 1]);
        vm->stack_count--;
        if (vm->error != NULL) return -1;
        return vm_push(vm, result);
    }
    vm_set_error(vm, "value is not callable");
    return -1;
}

js_vm_result_t js_vm_run(js_vm_t *vm, js_function_t *function) {
    js_vm_result_t result;
    memset(&result, 0, sizeof(result));
    result.value = js_runtime_make_undefined();

    if (vm_push(vm, js_runtime_make_function(function)) != 0) goto fail;
    if (call_function(vm, function, 0) != 0) goto fail;

    while (vm->frame_count > 0) {
        js_call_frame_t *frame = &vm->frames[vm->frame_count - 1];
        uint8_t op = read_u8(frame);
        switch (op) {
            case JS_OP_CONSTANT: {
                uint16_t idx = read_u16(frame);
                if (vm_push(vm, js_runtime_clone(&frame->function->chunk.constants[idx])) != 0) goto fail;
                break;
            }
            case JS_OP_UNDEFINED: if (vm_push(vm, js_runtime_make_undefined()) != 0) goto fail; break;
            case JS_OP_NULL: if (vm_push(vm, js_runtime_make_null()) != 0) goto fail; break;
            case JS_OP_TRUE: if (vm_push(vm, js_runtime_make_bool(1)) != 0) goto fail; break;
            case JS_OP_FALSE: if (vm_push(vm, js_runtime_make_bool(0)) != 0) goto fail; break;
            case JS_OP_POP: {
                js_runtime_value_t v = vm_pop(vm);
                js_runtime_value_free(&v);
                break;
            }
            case JS_OP_GET_GLOBAL: {
                uint16_t idx = read_u16(frame);
                const char *name = frame->function->chunk.constants[idx].as.string;
                js_runtime_value_t v;
                if (js_global_get(vm->globals, name, &v, &vm->error) != 0) goto fail;
                if (vm_push(vm, v) != 0) goto fail;
                break;
            }
            case JS_OP_DEFINE_GLOBAL: {
                uint16_t idx = read_u16(frame);
                const char *name = frame->function->chunk.constants[idx].as.string;
                js_runtime_value_t value = vm_pop(vm);
                if (js_global_define(vm->globals, name, value, 0, &vm->error) != 0) {
                    js_runtime_value_free(&value);
                    goto fail;
                }
                break;
            }
            case JS_OP_DEFINE_GLOBAL_CONST: {
                uint16_t idx = read_u16(frame);
                const char *name = frame->function->chunk.constants[idx].as.string;
                js_runtime_value_t value = vm_pop(vm);
                if (js_global_define(vm->globals, name, value, 1, &vm->error) != 0) {
                    js_runtime_value_free(&value);
                    goto fail;
                }
                break;
            }
            case JS_OP_SET_GLOBAL: {
                uint16_t idx = read_u16(frame);
                const char *name = frame->function->chunk.constants[idx].as.string;
                js_runtime_value_t *top = vm_peek(vm, 0);
                js_runtime_value_t copy = js_runtime_clone(top);
                if (js_global_set(vm->globals, name, copy, &vm->error) != 0) {
                    js_runtime_value_free(&copy);
                    goto fail;
                }
                break;
            }
            case JS_OP_GET_LOCAL: {
                uint16_t slot = read_u16(frame);
                if (vm_push(vm, js_runtime_clone(&vm->stack[frame->slot_base + slot])) != 0) goto fail;
                break;
            }
            case JS_OP_SET_LOCAL: {
                uint16_t slot = read_u16(frame);
                js_runtime_value_free(&vm->stack[frame->slot_base + slot]);
                vm->stack[frame->slot_base + slot] = js_runtime_clone(vm_peek(vm, 0));
                break;
            }
            case JS_OP_ADD:
            case JS_OP_SUB:
            case JS_OP_MUL:
            case JS_OP_DIV:
            case JS_OP_MOD:
            case JS_OP_EQ:
            case JS_OP_NEQ:
            case JS_OP_LT:
            case JS_OP_LTE:
            case JS_OP_GT:
            case JS_OP_GTE: {
                js_runtime_value_t b = vm_pop(vm);
                js_runtime_value_t a = vm_pop(vm);
                js_runtime_value_t out = js_runtime_make_undefined();
                if (op == JS_OP_ADD &&
                    (a.kind == JS_RT_STRING || b.kind == JS_RT_STRING)) {
                    const char *as = a.kind == JS_RT_STRING ? a.as.string : "";
                    const char *bs = b.kind == JS_RT_STRING ? b.as.string : "";
                    size_t len = strlen(as) + strlen(bs);
                    char *buf = (char *)malloc(len + 1);
                    if (buf == NULL) vm_set_error(vm, "out of memory");
                    else {
                        strcpy(buf, as);
                        strcat(buf, bs);
                        out.kind = JS_RT_STRING;
                        out.as.string = buf;
                    }
                } else if (op == JS_OP_EQ || op == JS_OP_NEQ) {
                    int eq = 0;
                    if (a.kind == b.kind) {
                        if (a.kind == JS_RT_NUMBER) eq = a.as.number == b.as.number;
                        else if (a.kind == JS_RT_BOOL) eq = a.as.boolean == b.as.boolean;
                        else if (a.kind == JS_RT_STRING) eq = strcmp(a.as.string, b.as.string) == 0;
                        else eq = 1;
                    }
                    out = js_runtime_make_bool(op == JS_OP_EQ ? eq : !eq);
                } else {
                    double da = 0.0, db = 0.0;
                    if (val_to_number(&a, &da) != 0 || val_to_number(&b, &db) != 0) {
                        vm_set_error(vm, "numeric operands required");
                    } else if (op == JS_OP_ADD) out = js_runtime_make_number(da + db);
                    else if (op == JS_OP_SUB) out = js_runtime_make_number(da - db);
                    else if (op == JS_OP_MUL) out = js_runtime_make_number(da * db);
                    else if (op == JS_OP_DIV) out = js_runtime_make_number(da / db);
                    else if (op == JS_OP_MOD) out = js_runtime_make_number(fmod(da, db));
                    else if (op == JS_OP_LT) out = js_runtime_make_bool(da < db);
                    else if (op == JS_OP_LTE) out = js_runtime_make_bool(da <= db);
                    else if (op == JS_OP_GT) out = js_runtime_make_bool(da > db);
                    else if (op == JS_OP_GTE) out = js_runtime_make_bool(da >= db);
                }
                js_runtime_value_free(&a);
                js_runtime_value_free(&b);
                if (vm->error != NULL) goto fail;
                if (vm_push(vm, out) != 0) goto fail;
                break;
            }
            case JS_OP_NEG:
            case JS_OP_NOT: {
                js_runtime_value_t v = vm_pop(vm);
                js_runtime_value_t out = js_runtime_make_undefined();
                if (op == JS_OP_NOT) out = js_runtime_make_bool(!is_truthy(&v));
                else {
                    double n = 0.0;
                    if (val_to_number(&v, &n) != 0) vm_set_error(vm, "numeric operand required");
                    else out = js_runtime_make_number(-n);
                }
                js_runtime_value_free(&v);
                if (vm->error != NULL) goto fail;
                if (vm_push(vm, out) != 0) goto fail;
                break;
            }
            case JS_OP_JUMP: {
                uint16_t off = read_u16(frame);
                frame->ip += off;
                break;
            }
            case JS_OP_JUMP_IF_FALSE: {
                uint16_t off = read_u16(frame);
                if (!is_truthy(vm_peek(vm, 0))) frame->ip += off;
                break;
            }
            case JS_OP_JUMP_IF_TRUE: {
                uint16_t off = read_u16(frame);
                if (is_truthy(vm_peek(vm, 0))) frame->ip += off;
                break;
            }
            case JS_OP_CALL: {
                uint8_t argc = read_u8(frame);
                js_runtime_value_t callee = js_runtime_clone(vm_peek(vm, argc));
                if (call_value(vm, callee, argc) != 0) {
                    js_runtime_value_free(&callee);
                    goto fail;
                }
                js_runtime_value_free(&callee);
                break;
            }
            case JS_OP_RETURN: {
                js_runtime_value_t ret = vm_pop(vm);
                int base = frame->slot_base;
                while (vm->stack_count > base) {
                    js_runtime_value_t drop = vm_pop(vm);
                    js_runtime_value_free(&drop);
                }
                vm->frame_count--;
                if (vm->frame_count == 0) {
                    result.value = ret;
                    return result;
                }
                if (vm_push(vm, ret) != 0) goto fail;
                break;
            }
        }
    }

fail:
    result.error = js_strdup(vm->error != NULL ? vm->error : "vm error");
    return result;
}
