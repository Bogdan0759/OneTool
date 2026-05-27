#include "../src/internal.h"

#include <stdlib.h>
#include <string.h>

static js_object_prop_t *find_prop(js_object_t *obj, const char *name) {
    if (obj == NULL || name == NULL) return NULL;
    for (int i = 0; i < obj->count; i++) {
        if (strcmp(obj->props[i].name, name) == 0) return &obj->props[i];
    }
    return NULL;
}

js_object_t *js_object_create(void) {
    js_object_t *obj = (js_object_t *)calloc(1, sizeof(js_object_t));
    if (obj != NULL) obj->refs = 1;
    return obj;
}

void js_object_free(js_object_t *obj) {
    if (obj == NULL) return;
    obj->refs--;
    if (obj->refs > 0) return;
    for (int i = 0; i < obj->count; i++) {
        free(obj->props[i].name);
        js_runtime_value_free(&obj->props[i].value);
    }
    free(obj->props);
    free(obj);
}

int js_object_define(js_object_t *obj, const char *name,
                     js_runtime_value_t value, int is_const, char **err) {
    if (obj == NULL || name == NULL) return -1;
    if (find_prop(obj, name) != NULL) {
        if (err != NULL) *err = js_format_error("property already defined: %s", name, NULL);
        return -1;
    }
    if (obj->count == obj->cap) {
        int want = obj->cap == 0 ? 8 : obj->cap * 2;
        js_object_prop_t *p = (js_object_prop_t *)realloc(obj->props,
                                                          (size_t)want * sizeof(*p));
        if (p == NULL) return -1;
        obj->props = p;
        obj->cap = want;
    }
    obj->props[obj->count].name = js_strdup(name);
    obj->props[obj->count].value = value;
    obj->props[obj->count].is_const = is_const;
    obj->count++;
    return 0;
}

int js_object_set(js_object_t *obj, const char *name,
                  js_runtime_value_t value, char **err) {
    js_object_prop_t *prop = find_prop(obj, name);
    if (prop == NULL) return js_object_define(obj, name, value, 0, err);
    if (prop->is_const) {
        if (err != NULL) *err = js_format_error("cannot assign to const property: %s", name, NULL);
        return -1;
    }
    js_runtime_value_free(&prop->value);
    prop->value = value;
    return 0;
}

int js_object_get(js_object_t *obj, const char *name,
                  js_runtime_value_t *out, char **err) {
    js_object_prop_t *prop = find_prop(obj, name);
    if (prop == NULL) {
        if (out != NULL) *out = js_runtime_make_undefined();
        return 0;
    }
    *out = js_runtime_clone(&prop->value);
    return 0;
}

void js_runtime_value_free(js_runtime_value_t *value) {
    if (value == NULL) return;
    if (value->kind == JS_RT_STRING) free(value->as.string);
    if (value->kind == JS_RT_OBJECT) js_object_free(value->as.object);
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

js_runtime_value_t js_runtime_make_object(js_object_t *obj) {
    if (obj != NULL) obj->refs++;
    js_runtime_value_t v; memset(&v, 0, sizeof(v)); v.kind = JS_RT_OBJECT; v.as.object = obj; return v;
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
        case JS_RT_OBJECT: return js_runtime_make_object(value->as.object);
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
