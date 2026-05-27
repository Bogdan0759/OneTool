#include "../src/internal.h"

#include <stdlib.h>
#include <string.h>

void js_runtime_value_free(js_runtime_value_t *value) {
    if (value == NULL) return;
    if (value->kind == JS_RT_STRING) free(value->as.string);
    value->kind = JS_RT_UNDEFINED;
}

js_runtime_value_t js_runtime_make_undefined(void) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_UNDEFINED; return v;
}

js_runtime_value_t js_runtime_make_null(void) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_NULL; return v;
}

js_runtime_value_t js_runtime_make_bool(int v_) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_BOOL; v.as.boolean = v_ ? 1 : 0; return v;
}

js_runtime_value_t js_runtime_make_number(double v_) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_NUMBER; v.as.number = v_; return v;
}

js_runtime_value_t js_runtime_make_string(const char *s) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_STRING; v.as.string = js_strdup(s != NULL ? s : ""); return v;
}

js_runtime_value_t js_runtime_make_function(js_function_t *fn) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_FUNCTION; v.as.function = fn; return v;
}

js_runtime_value_t js_runtime_make_native(js_native_fn_t *native) {
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_NATIVE; v.as.native = native; return v;
}

js_runtime_value_t js_runtime_clone(const js_runtime_value_t *value) {
    if (value == NULL) return js_runtime_make_undefined();
    switch (value->kind) {
        case JS_RT_BOOL: return js_runtime_make_bool(value->as.boolean);
        case JS_RT_NUMBER: return js_runtime_make_number(value->as.number);
        case JS_RT_STRING: return js_runtime_make_string(value->as.string);
        case JS_RT_NULL: return js_runtime_make_null();
        case JS_RT_FUNCTION: return js_runtime_make_function(value->as.function);
        case JS_RT_NATIVE: return js_runtime_make_native(value->as.native);
        default: return js_runtime_make_undefined();
    }
}

void js_function_free(js_function_t *fn) {
    if (fn == NULL) return;
    free(fn->name);
    js_chunk_free(&fn->chunk);
    free(fn);
}
