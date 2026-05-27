#ifndef ONETOOL_TOOLS_GUI_BROWSER_JSENGINE_H
#define ONETOOL_TOOLS_GUI_BROWSER_JSENGINE_H

#include <stddef.h>

typedef struct js_engine js_engine_t;
typedef struct js_host_api js_host_api_t;

typedef enum {
    JS_EVAL_OK = 0,
    JS_EVAL_PARSE_ERROR,
    JS_EVAL_RUNTIME_ERROR,
    JS_EVAL_OOM
} js_eval_status_t;

typedef enum {
    JS_VALUE_UNDEFINED = 0,
    JS_VALUE_NULL,
    JS_VALUE_BOOL,
    JS_VALUE_NUMBER,
    JS_VALUE_STRING
} js_value_kind_t;

typedef struct {
    js_value_kind_t kind;
    int boolean;
    double number;
    const char *string;
} js_eval_result_t;

typedef js_eval_result_t (*js_native_callback_t)(void *user_data,
                                                 int argc,
                                                 const js_eval_result_t *argv,
                                                 char *err_buf,
                                                 size_t err_cap);

js_engine_t *js_engine_create(void);
void js_engine_destroy(js_engine_t *engine);
int js_engine_set_host_api(js_engine_t *engine, const js_host_api_t *api,
                           void *user_data);

js_eval_status_t js_engine_eval(js_engine_t *engine, const char *src,
                                js_eval_result_t *out,
                                char *err_buf, size_t err_cap);

struct js_host_api {
    const char          *name;
    int                  arity;
    js_native_callback_t callback;
};

#endif
