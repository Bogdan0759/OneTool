#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    int   slot;
    int   is_const;
} js_local_t;

typedef struct {
    js_function_t *fn;
    js_local_t    *locals;
    int            local_count;
    int            local_cap;
    char          *error;
} js_compiler_t;

static int emit_op(js_compiler_t *c, js_opcode_t op) {
    return js_chunk_write(&c->fn->chunk, (uint8_t)op);
}

static int emit_u16(js_compiler_t *c, uint16_t value) {
    return js_chunk_write_u16(&c->fn->chunk, value);
}

static int add_constant(js_compiler_t *c, js_runtime_value_t value) {
    return js_chunk_add_constant(&c->fn->chunk, value);
}

static int add_local(js_compiler_t *c, const char *name, int is_const) {
    if (c->local_count == c->local_cap) {
        int want = c->local_cap == 0 ? 8 : c->local_cap * 2;
        js_local_t *p = (js_local_t *)realloc(c->locals, (size_t)want * sizeof(*p));
        if (p == NULL) return -1;
        c->locals = p;
        c->local_cap = want;
    }
    c->locals[c->local_count].name = js_strdup(name);
    c->locals[c->local_count].slot = c->local_count + 1;
    c->locals[c->local_count].is_const = is_const;
    c->fn->local_count = c->local_count + 2;
    return c->local_count++;
}

static js_local_t *find_local(js_compiler_t *c, const char *name) {
    for (int i = c->local_count - 1; i >= 0; i--) {
        if (strcmp(c->locals[i].name, name) == 0) return &c->locals[i];
    }
    return NULL;
}

static void compiler_error(js_compiler_t *c, const char *msg) {
    free(c->error);
    c->error = js_strdup(msg);
}

static int compile_expr(js_compiler_t *c, js_ast_t *node);
static int compile_stmt(js_compiler_t *c, js_ast_t *node);
static js_compile_result_t compile_function_body(const char *name,
                                                 char **params, int param_count,
                                                 js_ast_t *body);
static int patch_u16(js_compiler_t *c, int at, uint16_t value);

static int emit_const_string(js_compiler_t *c, const char *s) {
    int idx = add_constant(c, js_runtime_make_string(s));
    if (idx < 0) return -1;
    if (emit_op(c, JS_OP_CONSTANT) != 0) return -1;
    return emit_u16(c, (uint16_t)idx);
}

static int compile_member_name(js_compiler_t *c, js_ast_t *node) {
    if (node->kind == JS_AST_IDENT) {
        return emit_const_string(c, node->as.ident.name);
    }
    if (node->kind == JS_AST_MEMBER_EXPR) {
        size_t need = strlen(node->as.member.property) + 64;
        char *buf = (char *)malloc(need);
        if (buf == NULL) return -1;
        if (node->as.member.object->kind == JS_AST_IDENT) {
            snprintf(buf, need, "%s.%s",
                     node->as.member.object->as.ident.name,
                     node->as.member.property);
        } else {
            free(buf);
            compiler_error(c, "unsupported member target");
            return -1;
        }
        int rc = emit_const_string(c, buf);
        free(buf);
        return rc;
    }
    compiler_error(c, "unsupported callee");
    return -1;
}

static int compile_expr(js_compiler_t *c, js_ast_t *node) {
    switch (node->kind) {
        case JS_AST_NUMBER: {
            int idx = add_constant(c, js_runtime_make_number(node->as.number.value));
            if (idx < 0) return -1;
            if (emit_op(c, JS_OP_CONSTANT) != 0) return -1;
            return emit_u16(c, (uint16_t)idx);
        }
        case JS_AST_STRING:
            return emit_const_string(c, node->as.string.value);
        case JS_AST_BOOL:
            return emit_op(c, node->as.boolean.value ? JS_OP_TRUE : JS_OP_FALSE);
        case JS_AST_NULL:
            return emit_op(c, JS_OP_NULL);
        case JS_AST_UNDEFINED:
            return emit_op(c, JS_OP_UNDEFINED);
        case JS_AST_IDENT: {
            js_local_t *local = find_local(c, node->as.ident.name);
            if (local != NULL) {
                if (emit_op(c, JS_OP_GET_LOCAL) != 0) return -1;
                return emit_u16(c, (uint16_t)local->slot);
            }
            int idx = add_constant(c, js_runtime_make_string(node->as.ident.name));
            if (idx < 0) return -1;
            if (emit_op(c, JS_OP_GET_GLOBAL) != 0) return -1;
            return emit_u16(c, (uint16_t)idx);
        }
        case JS_AST_OBJECT_LITERAL:
            if (emit_op(c, JS_OP_NEW_OBJECT) != 0) return -1;
            for (int i = 0; i < node->as.object.count; i++) {
                if (emit_op(c, JS_OP_DUP) != 0) return -1;
                if (emit_const_string(c, node->as.object.keys[i]) != 0) return -1;
                if (compile_expr(c, node->as.object.values[i]) != 0) return -1;
                if (emit_op(c, JS_OP_SET_PROP) != 0) return -1;
            }
            return 0;
        case JS_AST_ASSIGN_EXPR: {
            if (compile_expr(c, node->as.assign.value) != 0) return -1;
            js_local_t *local = find_local(c, node->as.assign.name);
            if (local != NULL) {
                if (local->is_const) {
                    compiler_error(c, "cannot assign to const local");
                    return -1;
                }
                if (emit_op(c, JS_OP_SET_LOCAL) != 0) return -1;
                return emit_u16(c, (uint16_t)local->slot);
            }
            int idx = add_constant(c, js_runtime_make_string(node->as.assign.name));
            if (idx < 0) return -1;
            if (emit_op(c, JS_OP_SET_GLOBAL) != 0) return -1;
            return emit_u16(c, (uint16_t)idx);
        }
        case JS_AST_UNARY_EXPR:
            if (compile_expr(c, node->as.unary.expr) != 0) return -1;
            if (strcmp(node->as.unary.op, "!") == 0) return emit_op(c, JS_OP_NOT);
            if (strcmp(node->as.unary.op, "-") == 0) return emit_op(c, JS_OP_NEG);
            return 0;
        case JS_AST_BINARY_EXPR:
            if (strcmp(node->as.binary.op, "&&") == 0) {
                if (compile_expr(c, node->as.binary.left) != 0) return -1;
                if (emit_op(c, JS_OP_JUMP_IF_FALSE) != 0) return -1;
                int end_patch = c->fn->chunk.code_count;
                if (emit_u16(c, 0) != 0) return -1;
                if (emit_op(c, JS_OP_POP) != 0) return -1;
                if (compile_expr(c, node->as.binary.right) != 0) return -1;
                patch_u16(c, end_patch,
                          (uint16_t)(c->fn->chunk.code_count - end_patch - 2));
                return 0;
            }
            if (strcmp(node->as.binary.op, "||") == 0) {
                if (compile_expr(c, node->as.binary.left) != 0) return -1;
                if (emit_op(c, JS_OP_JUMP_IF_TRUE) != 0) return -1;
                int end_patch = c->fn->chunk.code_count;
                if (emit_u16(c, 0) != 0) return -1;
                if (emit_op(c, JS_OP_POP) != 0) return -1;
                if (compile_expr(c, node->as.binary.right) != 0) return -1;
                patch_u16(c, end_patch,
                          (uint16_t)(c->fn->chunk.code_count - end_patch - 2));
                return 0;
            }
            if (compile_expr(c, node->as.binary.left) != 0) return -1;
            if (compile_expr(c, node->as.binary.right) != 0) return -1;
            if (strcmp(node->as.binary.op, "+") == 0) return emit_op(c, JS_OP_ADD);
            if (strcmp(node->as.binary.op, "-") == 0) return emit_op(c, JS_OP_SUB);
            if (strcmp(node->as.binary.op, "*") == 0) return emit_op(c, JS_OP_MUL);
            if (strcmp(node->as.binary.op, "/") == 0) return emit_op(c, JS_OP_DIV);
            if (strcmp(node->as.binary.op, "%") == 0) return emit_op(c, JS_OP_MOD);
            if (strcmp(node->as.binary.op, "==") == 0) return emit_op(c, JS_OP_EQ);
            if (strcmp(node->as.binary.op, "!=") == 0) return emit_op(c, JS_OP_NEQ);
            if (strcmp(node->as.binary.op, "<") == 0) return emit_op(c, JS_OP_LT);
            if (strcmp(node->as.binary.op, "<=") == 0) return emit_op(c, JS_OP_LTE);
            if (strcmp(node->as.binary.op, ">") == 0) return emit_op(c, JS_OP_GT);
            if (strcmp(node->as.binary.op, ">=") == 0) return emit_op(c, JS_OP_GTE);
            compiler_error(c, "unsupported binary operator");
            return -1;
        case JS_AST_CALL_EXPR:
            if (node->as.call.callee->kind == JS_AST_MEMBER_EXPR) {
                if (compile_member_name(c, node->as.call.callee) != 0) return -1;
            } else {
                if (compile_expr(c, node->as.call.callee) != 0) return -1;
            }
            for (int i = 0; i < node->as.call.arg_count; i++) {
                if (compile_expr(c, node->as.call.args[i]) != 0) return -1;
            }
            if (emit_op(c, JS_OP_CALL) != 0) return -1;
            return js_chunk_write(&c->fn->chunk, (uint8_t)node->as.call.arg_count);
        case JS_AST_MEMBER_EXPR:
            if (compile_expr(c, node->as.member.object) != 0) return -1;
            if (emit_const_string(c, node->as.member.property) != 0) return -1;
            return emit_op(c, JS_OP_GET_PROP);
        default:
            compiler_error(c, "unsupported expression");
            return -1;
    }
}

static int patch_u16(js_compiler_t *c, int at, uint16_t value) {
    c->fn->chunk.code[at] = (uint8_t)((value >> 8) & 0xFF);
    c->fn->chunk.code[at + 1] = (uint8_t)(value & 0xFF);
    return 0;
}

static int compile_stmt(js_compiler_t *c, js_ast_t *node) {
    switch (node->kind) {
        case JS_AST_PROGRAM:
        case JS_AST_BLOCK:
            for (int i = 0; i < node->as.list.count; i++) {
                if (compile_stmt(c, node->as.list.items[i]) != 0) return -1;
            }
            return 0;
        case JS_AST_VAR_DECL:
            if (node->as.var_decl.init != NULL) {
                if (compile_expr(c, node->as.var_decl.init) != 0) return -1;
            } else if (emit_op(c, JS_OP_UNDEFINED) != 0) return -1;
            if (c->fn->name != NULL) {
                int local_index = add_local(c, node->as.var_decl.name,
                                            node->as.var_decl.is_const);
                if (local_index < 0) return -1;
                if (emit_op(c, JS_OP_SET_LOCAL) != 0) return -1;
                if (emit_u16(c, (uint16_t)c->locals[local_index].slot) != 0) return -1;
                return emit_op(c, JS_OP_POP);
            } else {
                int idx = add_constant(c, js_runtime_make_string(node->as.var_decl.name));
                if (idx < 0) return -1;
                if (emit_op(c, node->as.var_decl.is_const
                               ? JS_OP_DEFINE_GLOBAL_CONST
                               : JS_OP_DEFINE_GLOBAL) != 0) return -1;
                return emit_u16(c, (uint16_t)idx);
            }
        case JS_AST_EXPR_STMT:
            if (compile_expr(c, node->as.expr_stmt.expr) != 0) return -1;
            return emit_op(c, JS_OP_POP);
        case JS_AST_RETURN_STMT:
            if (node->as.return_stmt.value != NULL) {
                if (compile_expr(c, node->as.return_stmt.value) != 0) return -1;
            } else if (emit_op(c, JS_OP_UNDEFINED) != 0) return -1;
            return emit_op(c, JS_OP_RETURN);
        case JS_AST_IF_STMT: {
            if (compile_expr(c, node->as.if_stmt.cond) != 0) return -1;
            if (emit_op(c, JS_OP_JUMP_IF_FALSE) != 0) return -1;
            int else_patch = c->fn->chunk.code_count;
            if (emit_u16(c, 0) != 0) return -1;
            if (emit_op(c, JS_OP_POP) != 0) return -1;
            if (compile_stmt(c, node->as.if_stmt.then_branch) != 0) return -1;
            if (emit_op(c, JS_OP_JUMP) != 0) return -1;
            int end_patch = c->fn->chunk.code_count;
            if (emit_u16(c, 0) != 0) return -1;
            patch_u16(c, else_patch, (uint16_t)(c->fn->chunk.code_count - else_patch - 2));
            if (emit_op(c, JS_OP_POP) != 0) return -1;
            if (node->as.if_stmt.else_branch != NULL) {
                if (compile_stmt(c, node->as.if_stmt.else_branch) != 0) return -1;
            }
            patch_u16(c, end_patch, (uint16_t)(c->fn->chunk.code_count - end_patch - 2));
            return 0;
        }
        case JS_AST_WHILE_STMT: {
            int loop_start = c->fn->chunk.code_count;
            if (compile_expr(c, node->as.while_stmt.cond) != 0) return -1;
            if (emit_op(c, JS_OP_JUMP_IF_FALSE) != 0) return -1;
            int exit_patch = c->fn->chunk.code_count;
            if (emit_u16(c, 0) != 0) return -1;
            if (emit_op(c, JS_OP_POP) != 0) return -1;
            if (compile_stmt(c, node->as.while_stmt.body) != 0) return -1;
            if (emit_op(c, JS_OP_JUMP) != 0) return -1;
            if (emit_u16(c, (uint16_t)(loop_start - c->fn->chunk.code_count - 2)) != 0) return -1;
            patch_u16(c, exit_patch, (uint16_t)(c->fn->chunk.code_count - exit_patch - 2));
            return emit_op(c, JS_OP_POP);
        }
        case JS_AST_FUNC_DECL: {
            js_compile_result_t nested = compile_function_body(
                node->as.func_decl.name,
                node->as.func_decl.params,
                node->as.func_decl.param_count,
                node->as.func_decl.body);
            if (nested.error != NULL) {
                compiler_error(c, nested.error);
                free(nested.error);
                return -1;
            }
            free(nested.function->name);
            nested.function->name = js_strdup(node->as.func_decl.name);
            nested.function->arity = node->as.func_decl.param_count;
            int idx = add_constant(c, js_runtime_make_function(nested.function));
            if (idx < 0) return -1;
            if (emit_op(c, JS_OP_CONSTANT) != 0) return -1;
            if (emit_u16(c, (uint16_t)idx) != 0) return -1;
            int name_idx = add_constant(c, js_runtime_make_string(node->as.func_decl.name));
            if (name_idx < 0) return -1;
            if (emit_op(c, JS_OP_DEFINE_GLOBAL) != 0) return -1;
            return emit_u16(c, (uint16_t)name_idx);
        }
        default:
            compiler_error(c, "unsupported statement");
            return -1;
    }
}

static void compiler_free(js_compiler_t *c) {
    for (int i = 0; i < c->local_count; i++) free(c->locals[i].name);
    free(c->locals);
}

js_compile_result_t js_compile_program(js_ast_t *program) {
    js_compile_result_t out;
    memset(&out, 0, sizeof(out));
    js_function_t *fn = (js_function_t *)calloc(1, sizeof(*fn));
    if (fn == NULL) {
        out.error = js_strdup("out of memory");
        return out;
    }
    js_chunk_init(&fn->chunk);

    js_compiler_t c;
    memset(&c, 0, sizeof(c));
    c.fn = fn;

    if (program->kind == JS_AST_BLOCK || program->kind == JS_AST_PROGRAM) {
        /* ok */
    }

    if (compile_stmt(&c, program) != 0) {
        out.error = js_strdup(c.error != NULL ? c.error : "compile failed");
        compiler_free(&c);
        js_function_free(fn);
        free(c.error);
        return out;
    }
    emit_op(&c, JS_OP_UNDEFINED);
    emit_op(&c, JS_OP_RETURN);
    compiler_free(&c);
    free(c.error);
    out.function = fn;
    return out;
}

static js_compile_result_t compile_function_body(const char *name,
                                                 char **params, int param_count,
                                                 js_ast_t *body) {
    js_compile_result_t out;
    memset(&out, 0, sizeof(out));
    js_function_t *fn = (js_function_t *)calloc(1, sizeof(*fn));
    if (fn == NULL) {
        out.error = js_strdup("out of memory");
        return out;
    }
    fn->name = js_strdup(name);
    fn->arity = param_count;
    js_chunk_init(&fn->chunk);

    js_compiler_t c;
    memset(&c, 0, sizeof(c));
    c.fn = fn;
    for (int i = 0; i < param_count; i++) {
        if (add_local(&c, params[i], 0) < 0) {
            out.error = js_strdup("out of memory");
            compiler_free(&c);
            js_function_free(fn);
            return out;
        }
    }

    if (compile_stmt(&c, body) != 0) {
        out.error = js_strdup(c.error != NULL ? c.error : "compile failed");
        compiler_free(&c);
        js_function_free(fn);
        free(c.error);
        return out;
    }
    emit_op(&c, JS_OP_UNDEFINED);
    emit_op(&c, JS_OP_RETURN);
    compiler_free(&c);
    free(c.error);
    out.function = fn;
    return out;
}
