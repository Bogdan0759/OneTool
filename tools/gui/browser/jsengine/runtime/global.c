#include "../src/internal.h"

#include <stdlib.h>
#include <string.h>

void js_global_env_init(js_global_env_t *env) {
    memset(env, 0, sizeof(*env));
}

void js_global_env_free(js_global_env_t *env) {
    if (env == NULL) return;
    for (int i = 0; i < env->count; i++) {
        free(env->items[i].name);
        js_runtime_value_free(&env->items[i].value);
    }
    free(env->items);
    memset(env, 0, sizeof(*env));
}

static js_binding_t *find_binding(js_global_env_t *env, const char *name) {
    for (int i = 0; i < env->count; i++) {
        if (strcmp(env->items[i].name, name) == 0) return &env->items[i];
    }
    return NULL;
}

int js_global_define(js_global_env_t *env, const char *name,
                     js_runtime_value_t value, int is_const, char **err) {
    if (find_binding(env, name) != NULL) {
        if (err != NULL) *err = js_format_error("identifier already defined: %s", name, NULL);
        return -1;
    }
    if (env->count == env->cap) {
        int want = env->cap == 0 ? 8 : env->cap * 2;
        js_binding_t *p = (js_binding_t *)realloc(env->items,
                                                  (size_t)want * sizeof(*p));
        if (p == NULL) return -1;
        env->items = p;
        env->cap = want;
    }
    env->items[env->count].name = js_strdup(name);
    env->items[env->count].value = value;
    env->items[env->count].is_const = is_const;
    env->count++;
    return 0;
}

int js_global_set(js_global_env_t *env, const char *name,
                  js_runtime_value_t value, char **err) {
    js_binding_t *b = find_binding(env, name);
    if (b == NULL) {
        if (err != NULL) *err = js_format_error("unknown identifier: %s", name, NULL);
        return -1;
    }
    if (b->is_const) {
        if (err != NULL) *err = js_format_error("cannot assign to const: %s", name, NULL);
        return -1;
    }
    js_runtime_value_free(&b->value);
    b->value = value;
    return 0;
}

int js_global_get(js_global_env_t *env, const char *name,
                  js_runtime_value_t *out, char **err) {
    js_binding_t *b = find_binding(env, name);
    if (b == NULL) {
        if (err != NULL) *err = js_format_error("unknown identifier: %s", name, NULL);
        return -1;
    }
    *out = js_runtime_clone(&b->value);
    return 0;
}
