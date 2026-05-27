#include "internal.h"

#include <stdlib.h>
#include <string.h>

static void free_native_fn(js_native_fn_t *fn) {
    if (fn == NULL) return;
    free(fn->name);
    free(fn);
}

js_engine_t *js_engine_create(void) {
    js_engine_t *engine = (js_engine_t *)calloc(1, sizeof(*engine));
    if (engine == NULL) return NULL;
    js_global_env_init(&engine->globals);
    if (js_register_builtins(engine, &engine->globals) != 0) {
        js_global_env_free(&engine->globals);
        free(engine);
        return NULL;
    }
    return engine;
}

void js_engine_destroy(js_engine_t *engine) {
    if (engine == NULL) return;
    free(engine->last_string);
    for (int i = 0; i < engine->module_count; i++) {
        js_function_free(engine->modules[i]);
    }
    free(engine->modules);
    for (int i = 0; i < engine->host_fn_count; i++) {
        free_native_fn(engine->host_fns[i]);
    }
    free(engine->host_fns);
    js_global_env_free(&engine->globals);
    free(engine);
}

static void copy_error(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) return;
    if (src == NULL) src = "unknown error";
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int track_module(js_engine_t *engine, js_function_t *fn) {
    if (engine->module_count == engine->module_cap) {
        int want = engine->module_cap == 0 ? 8 : engine->module_cap * 2;
        js_function_t **p = (js_function_t **)realloc(engine->modules,
                                                      (size_t)want * sizeof(*p));
        if (p == NULL) return -1;
        engine->modules = p;
        engine->module_cap = want;
    }
    engine->modules[engine->module_count++] = fn;
    return 0;
}

int js_engine_set_host_api(js_engine_t *engine, const js_host_api_t *api,
                           void *user_data) {
    if (engine == NULL) return -1;
    engine->host_user_data = user_data;
    if (api == NULL) return 0;

    for (int i = 0; api[i].name != NULL; i++) {
        js_native_fn_t *fn = (js_native_fn_t *)calloc(1, sizeof(*fn));
        if (fn == NULL) return -1;
        fn->name = js_strdup(api[i].name);
        if (fn->name == NULL) {
            free(fn);
            return -1;
        }
        fn->arity = api[i].arity;
        fn->engine = engine;
        fn->user_data = user_data;
        fn->host_call = api[i].callback;
        fn->call = NULL;

        if (engine->host_fn_count == engine->host_fn_cap) {
            int want = engine->host_fn_cap == 0 ? 8 : engine->host_fn_cap * 2;
            js_native_fn_t **p = (js_native_fn_t **)realloc(engine->host_fns,
                                                            (size_t)want * sizeof(*p));
            if (p == NULL) {
                free_native_fn(fn);
                return -1;
            }
            engine->host_fns = p;
            engine->host_fn_cap = want;
        }
        engine->host_fns[engine->host_fn_count++] = fn;
        fn->call = js_native_host_bridge;
        if (js_global_define(&engine->globals, api[i].name,
                             js_runtime_make_native(fn), 1, NULL) != 0) {
            return -1;
        }
    }
    return 0;
}

js_eval_status_t js_engine_eval(js_engine_t *engine, const char *src,
                                js_eval_result_t *out,
                                char *err_buf, size_t err_cap) {
    if (out == NULL) return JS_EVAL_RUNTIME_ERROR;
    memset(out, 0, sizeof(*out));
    if (engine == NULL || src == NULL) {
        copy_error(err_buf, err_cap, "invalid arguments");
        return JS_EVAL_RUNTIME_ERROR;
    }

    js_parser_t parser;
    js_parser_init(&parser, src);
    js_ast_t *program = js_parse_program(&parser);
    if (program == NULL) {
        copy_error(err_buf, err_cap, parser.error != NULL ? parser.error : "parse error");
        js_parser_deinit(&parser);
        return JS_EVAL_PARSE_ERROR;
    }
    js_parser_deinit(&parser);

    js_compile_result_t compiled = js_compile_program(program);
    js_ast_free(program);
    if (compiled.error != NULL || compiled.function == NULL) {
        copy_error(err_buf, err_cap, compiled.error != NULL ? compiled.error : "compile error");
        free(compiled.error);
        return JS_EVAL_RUNTIME_ERROR;
    }
    if (track_module(engine, compiled.function) != 0) {
        js_function_free(compiled.function);
        copy_error(err_buf, err_cap, "out of memory");
        return JS_EVAL_OOM;
    }

    js_vm_t vm;
    js_vm_init(&vm, &engine->globals);
    js_vm_result_t exec = js_vm_run(&vm, compiled.function);
    js_vm_deinit(&vm);

    if (exec.error != NULL) {
        copy_error(err_buf, err_cap, exec.error);
        free(exec.error);
        js_runtime_value_free(&exec.value);
        return JS_EVAL_RUNTIME_ERROR;
    }

    free(engine->last_string);
    engine->last_string = NULL;

    switch (exec.value.kind) {
        case JS_RT_BOOL:
            out->kind = JS_VALUE_BOOL;
            out->boolean = exec.value.as.boolean;
            break;
        case JS_RT_NUMBER:
            out->kind = JS_VALUE_NUMBER;
            out->number = exec.value.as.number;
            break;
        case JS_RT_STRING:
            out->kind = JS_VALUE_STRING;
            engine->last_string = js_strdup(exec.value.as.string);
            out->string = engine->last_string;
            break;
        case JS_RT_NULL:
            out->kind = JS_VALUE_NULL;
            break;
        default:
            out->kind = JS_VALUE_UNDEFINED;
            break;
    }
    js_runtime_value_free(&exec.value);
    return JS_EVAL_OK;
}
