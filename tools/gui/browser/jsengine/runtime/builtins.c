#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>

static js_eval_result_t runtime_to_public(const js_runtime_value_t *v) {
    js_eval_result_t out;
    out.kind = JS_VALUE_UNDEFINED;
    out.boolean = 0;
    out.number = 0.0;
    out.string = NULL;
    if (v == NULL) return out;
    switch (v->kind) {
        case JS_RT_BOOL:
            out.kind = JS_VALUE_BOOL;
            out.boolean = v->as.boolean;
            break;
        case JS_RT_NUMBER:
            out.kind = JS_VALUE_NUMBER;
            out.number = v->as.number;
            break;
        case JS_RT_STRING:
            out.kind = JS_VALUE_STRING;
            out.string = v->as.string;
            break;
        case JS_RT_NULL:
            out.kind = JS_VALUE_NULL;
            break;
        default:
            out.kind = JS_VALUE_UNDEFINED;
            break;
    }
    return out;
}

static js_runtime_value_t public_to_runtime(const js_eval_result_t *v) {
    if (v == NULL) return js_runtime_make_undefined();
    switch (v->kind) {
        case JS_VALUE_BOOL: return js_runtime_make_bool(v->boolean);
        case JS_VALUE_NUMBER: return js_runtime_make_number(v->number);
        case JS_VALUE_STRING: return js_runtime_make_string(v->string);
        case JS_VALUE_NULL: return js_runtime_make_null();
        default: return js_runtime_make_undefined();
    }
}

static js_runtime_value_t native_print(js_native_fn_t *self, int argc,
                                       js_runtime_value_t *argv, char **err) {
    (void)self;
    (void)err;
    for (int i = 0; i < argc; i++) {
        if (i > 0) printf(" ");
        switch (argv[i].kind) {
            case JS_RT_BOOL: printf("%s", argv[i].as.boolean ? "true" : "false"); break;
            case JS_RT_NUMBER: printf("%g", argv[i].as.number); break;
            case JS_RT_STRING: printf("%s", argv[i].as.string); break;
            case JS_RT_NULL: printf("null"); break;
            case JS_RT_UNDEFINED: printf("undefined"); break;
            case JS_RT_FUNCTION: printf("[function]"); break;
            case JS_RT_NATIVE: printf("[native]"); break;
        }
    }
    printf("\n");
    return js_runtime_make_undefined();
}

js_runtime_value_t js_native_host_bridge(js_native_fn_t *self, int argc,
                                         js_runtime_value_t *argv, char **err) {
    if (self == NULL || self->host_call == NULL) {
        if (err != NULL) *err = js_strdup("host callback missing");
        return js_runtime_make_undefined();
    }

    js_eval_result_t pub_argv[16];
    if (argc > (int)(sizeof(pub_argv) / sizeof(pub_argv[0]))) {
        if (err != NULL) *err = js_strdup("too many host arguments");
        return js_runtime_make_undefined();
    }
    for (int i = 0; i < argc; i++) pub_argv[i] = runtime_to_public(&argv[i]);

    char host_err[256] = "";
    js_eval_result_t out = self->host_call(self->user_data, argc, pub_argv,
                                           host_err, sizeof(host_err));
    if (host_err[0] != '\0' && err != NULL) {
        *err = js_strdup(host_err);
        return js_runtime_make_undefined();
    }
    return public_to_runtime(&out);
}

int js_register_builtins(js_engine_t *engine, js_global_env_t *env) {
    js_native_fn_t *print_fn = (js_native_fn_t *)calloc(1, sizeof(*print_fn));
    if (print_fn == NULL) return -1;
    print_fn->name = js_strdup("print");
    print_fn->arity = -1;
    print_fn->call = native_print;
    print_fn->engine = engine;
    return js_global_define(env, "print", js_runtime_make_native(print_fn), 1, NULL);
}
