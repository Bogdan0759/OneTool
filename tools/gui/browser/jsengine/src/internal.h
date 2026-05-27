#ifndef ONETOOL_TOOLS_GUI_BROWSER_JSENGINE_INTERNAL_H
#define ONETOOL_TOOLS_GUI_BROWSER_JSENGINE_INTERNAL_H

#include "../include/jsengine.h"

#include <stddef.h>
#include <stdint.h>

typedef struct js_function js_function_t;
typedef struct js_native_fn js_native_fn_t;
typedef struct js_engine js_engine_t;

typedef enum {
    JS_RT_UNDEFINED = 0,
    JS_RT_NULL,
    JS_RT_BOOL,
    JS_RT_NUMBER,
    JS_RT_STRING,
    JS_RT_FUNCTION,
    JS_RT_NATIVE
} js_runtime_kind_t;

typedef struct {
    js_runtime_kind_t kind;
    union {
        int             boolean;
        double          number;
        char           *string;
        js_function_t  *function;
        js_native_fn_t *native;
    } as;
} js_runtime_value_t;

typedef struct {
    char             *name;
    js_runtime_value_t value;
    int               is_const;
} js_binding_t;

typedef struct {
    js_binding_t *items;
    int           count;
    int           cap;
} js_global_env_t;

typedef enum {
    JS_TOK_EOF = 0,
    JS_TOK_IDENT,
    JS_TOK_NUMBER,
    JS_TOK_STRING,
    JS_TOK_KW_LET,
    JS_TOK_KW_CONST,
    JS_TOK_KW_FUNCTION,
    JS_TOK_KW_RETURN,
    JS_TOK_KW_IF,
    JS_TOK_KW_ELSE,
    JS_TOK_KW_WHILE,
    JS_TOK_KW_TRUE,
    JS_TOK_KW_FALSE,
    JS_TOK_KW_NULL,
    JS_TOK_KW_UNDEFINED,
    JS_TOK_LPAREN,
    JS_TOK_RPAREN,
    JS_TOK_LBRACE,
    JS_TOK_RBRACE,
    JS_TOK_DOT,
    JS_TOK_COMMA,
    JS_TOK_SEMI,
    JS_TOK_PLUS,
    JS_TOK_MINUS,
    JS_TOK_STAR,
    JS_TOK_SLASH,
    JS_TOK_PERCENT,
    JS_TOK_BANG,
    JS_TOK_EQ,
    JS_TOK_EQEQ,
    JS_TOK_NEQ,
    JS_TOK_LT,
    JS_TOK_LTE,
    JS_TOK_GT,
    JS_TOK_GTE,
    JS_TOK_ANDAND,
    JS_TOK_OROR
} js_token_kind_t;

typedef struct {
    js_token_kind_t kind;
    const char     *start;
    size_t          len;
    double          number;
    int             line;
    int             col;
} js_token_t;

typedef struct {
    const char *src;
    const char *p;
    int         line;
    int         col;
} js_lexer_t;

void js_lex_init(js_lexer_t *lex, const char *src);
void js_lex_next(js_lexer_t *lex, js_token_t *out);

typedef enum {
    JS_AST_PROGRAM = 0,
    JS_AST_BLOCK,
    JS_AST_VAR_DECL,
    JS_AST_FUNC_DECL,
    JS_AST_IF_STMT,
    JS_AST_WHILE_STMT,
    JS_AST_RETURN_STMT,
    JS_AST_EXPR_STMT,
    JS_AST_ASSIGN_EXPR,
    JS_AST_BINARY_EXPR,
    JS_AST_UNARY_EXPR,
    JS_AST_CALL_EXPR,
    JS_AST_MEMBER_EXPR,
    JS_AST_IDENT,
    JS_AST_NUMBER,
    JS_AST_STRING,
    JS_AST_BOOL,
    JS_AST_NULL,
    JS_AST_UNDEFINED
} js_ast_kind_t;

typedef struct js_ast js_ast_t;

typedef struct {
    js_ast_t **items;
    int        count;
    int        cap;
} js_ast_list_t;

struct js_ast {
    js_ast_kind_t kind;
    union {
        js_ast_list_t list;
        struct {
            char    *name;
            int      is_const;
            js_ast_t *init;
        } var_decl;
        struct {
            char      *name;
            char     **params;
            int        param_count;
            js_ast_t  *body;
        } func_decl;
        struct {
            js_ast_t *cond;
            js_ast_t *then_branch;
            js_ast_t *else_branch;
        } if_stmt;
        struct {
            js_ast_t *cond;
            js_ast_t *body;
        } while_stmt;
        struct {
            js_ast_t *value;
        } return_stmt;
        struct {
            js_ast_t *expr;
        } expr_stmt;
        struct {
            char    *name;
            js_ast_t *value;
        } assign;
        struct {
            char    *op;
            js_ast_t *left;
            js_ast_t *right;
        } binary;
        struct {
            char    *op;
            js_ast_t *expr;
        } unary;
        struct {
            js_ast_t   *callee;
            js_ast_t  **args;
            int         arg_count;
        } call;
        struct {
            js_ast_t *object;
            char     *property;
        } member;
        struct {
            char *name;
        } ident;
        struct {
            double value;
        } number;
        struct {
            char *value;
        } string;
        struct {
            int value;
        } boolean;
    } as;
};

js_ast_t *js_ast_new(js_ast_kind_t kind);
void js_ast_free(js_ast_t *node);
int js_ast_list_push(js_ast_list_t *list, js_ast_t *node);

typedef struct {
    js_lexer_t lexer;
    js_token_t cur;
    js_token_t next;
    char      *error;
} js_parser_t;

void js_parser_init(js_parser_t *p, const char *src);
void js_parser_deinit(js_parser_t *p);
js_ast_t *js_parse_program(js_parser_t *p);

typedef enum {
    JS_OP_CONSTANT = 0,
    JS_OP_UNDEFINED,
    JS_OP_NULL,
    JS_OP_TRUE,
    JS_OP_FALSE,
    JS_OP_POP,
    JS_OP_GET_GLOBAL,
    JS_OP_DEFINE_GLOBAL,
    JS_OP_DEFINE_GLOBAL_CONST,
    JS_OP_SET_GLOBAL,
    JS_OP_GET_LOCAL,
    JS_OP_SET_LOCAL,
    JS_OP_ADD,
    JS_OP_SUB,
    JS_OP_MUL,
    JS_OP_DIV,
    JS_OP_MOD,
    JS_OP_NEG,
    JS_OP_NOT,
    JS_OP_EQ,
    JS_OP_NEQ,
    JS_OP_LT,
    JS_OP_LTE,
    JS_OP_GT,
    JS_OP_GTE,
    JS_OP_JUMP,
    JS_OP_JUMP_IF_FALSE,
    JS_OP_JUMP_IF_TRUE,
    JS_OP_CALL,
    JS_OP_RETURN
} js_opcode_t;

typedef struct {
    uint8_t          *code;
    int               code_count;
    int               code_cap;
    js_runtime_value_t *constants;
    int               const_count;
    int               const_cap;
} js_chunk_t;

struct js_function {
    char      *name;
    int        arity;
    int        local_count;
    js_chunk_t chunk;
};

typedef js_runtime_value_t (*js_native_call_t)(js_native_fn_t *self,
                                               int argc,
                                               js_runtime_value_t *argv,
                                               char **err);

struct js_native_fn {
    char            *name;
    int              arity;
    js_native_call_t call;
    js_engine_t     *engine;
    void            *user_data;
    js_native_callback_t host_call;
};

void js_chunk_init(js_chunk_t *chunk);
void js_chunk_free(js_chunk_t *chunk);
int js_chunk_write(js_chunk_t *chunk, uint8_t byte);
int js_chunk_write_u16(js_chunk_t *chunk, uint16_t value);
int js_chunk_add_constant(js_chunk_t *chunk, js_runtime_value_t value);

typedef struct {
    js_function_t *function;
    char          *error;
} js_compile_result_t;

js_compile_result_t js_compile_program(js_ast_t *program);

#define JS_VM_STACK_MAX   1024
#define JS_VM_FRAMES_MAX  64

typedef struct {
    js_function_t *function;
    int            ip;
    int            slot_base;
} js_call_frame_t;

typedef struct {
    js_global_env_t *globals;
    js_runtime_value_t stack[JS_VM_STACK_MAX];
    int               stack_count;
    js_call_frame_t   frames[JS_VM_FRAMES_MAX];
    int               frame_count;
    char             *error;
} js_vm_t;

typedef struct {
    js_runtime_value_t value;
    char              *error;
} js_vm_result_t;

void js_vm_init(js_vm_t *vm, js_global_env_t *globals);
void js_vm_deinit(js_vm_t *vm);
js_vm_result_t js_vm_run(js_vm_t *vm, js_function_t *function);

void js_function_free(js_function_t *fn);
void js_runtime_value_free(js_runtime_value_t *value);
js_runtime_value_t js_runtime_make_undefined(void);
js_runtime_value_t js_runtime_make_null(void);
js_runtime_value_t js_runtime_make_bool(int v);
js_runtime_value_t js_runtime_make_number(double v);
js_runtime_value_t js_runtime_make_string(const char *s);
js_runtime_value_t js_runtime_make_function(js_function_t *fn);
js_runtime_value_t js_runtime_make_native(js_native_fn_t *native);
js_runtime_value_t js_runtime_clone(const js_runtime_value_t *value);

void js_global_env_init(js_global_env_t *env);
void js_global_env_free(js_global_env_t *env);
int js_global_define(js_global_env_t *env, const char *name,
                     js_runtime_value_t value, int is_const, char **err);
int js_global_set(js_global_env_t *env, const char *name,
                  js_runtime_value_t value, char **err);
int js_global_get(js_global_env_t *env, const char *name,
                  js_runtime_value_t *out, char **err);

int js_register_builtins(js_engine_t *engine, js_global_env_t *env);
js_runtime_value_t js_native_host_bridge(js_native_fn_t *self, int argc,
                                         js_runtime_value_t *argv, char **err);

typedef struct js_engine {
    char           *last_string;
    js_global_env_t globals;
    js_function_t **modules;
    int             module_count;
    int             module_cap;
    js_native_fn_t **host_fns;
    int             host_fn_count;
    int             host_fn_cap;
    void           *host_user_data;
} js_engine_t;

char *js_strdup(const char *s);
char *js_strndup(const char *s, size_t len);
char *js_format_error(const char *fmt, const char *a, const char *b);

#endif
