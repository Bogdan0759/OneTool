#include "internal.h"

#include <stdlib.h>
#include <string.h>

js_ast_t *js_ast_new(js_ast_kind_t kind) {
    js_ast_t *node = (js_ast_t *)calloc(1, sizeof(*node));
    if (node == NULL) return NULL;
    node->kind = kind;
    return node;
}

int js_ast_list_push(js_ast_list_t *list, js_ast_t *node) {
    if (list->count == list->cap) {
        int want = list->cap == 0 ? 8 : list->cap * 2;
        js_ast_t **p = (js_ast_t **)realloc(list->items,
                                            (size_t)want * sizeof(*p));
        if (p == NULL) return -1;
        list->items = p;
        list->cap = want;
    }
    list->items[list->count++] = node;
    return 0;
}

static void free_string_array(char **items, int count) {
    if (items == NULL) return;
    for (int i = 0; i < count; i++) free(items[i]);
    free(items);
}

void js_ast_free(js_ast_t *node) {
    if (node == NULL) return;
    switch (node->kind) {
        case JS_AST_PROGRAM:
        case JS_AST_BLOCK:
            for (int i = 0; i < node->as.list.count; i++) {
                js_ast_free(node->as.list.items[i]);
            }
            free(node->as.list.items);
            break;
        case JS_AST_VAR_DECL:
            free(node->as.var_decl.name);
            js_ast_free(node->as.var_decl.init);
            break;
        case JS_AST_FUNC_DECL:
            free(node->as.func_decl.name);
            free_string_array(node->as.func_decl.params,
                              node->as.func_decl.param_count);
            js_ast_free(node->as.func_decl.body);
            break;
        case JS_AST_IF_STMT:
            js_ast_free(node->as.if_stmt.cond);
            js_ast_free(node->as.if_stmt.then_branch);
            js_ast_free(node->as.if_stmt.else_branch);
            break;
        case JS_AST_WHILE_STMT:
            js_ast_free(node->as.while_stmt.cond);
            js_ast_free(node->as.while_stmt.body);
            break;
        case JS_AST_RETURN_STMT:
            js_ast_free(node->as.return_stmt.value);
            break;
        case JS_AST_EXPR_STMT:
            js_ast_free(node->as.expr_stmt.expr);
            break;
        case JS_AST_ASSIGN_EXPR:
            free(node->as.assign.name);
            js_ast_free(node->as.assign.value);
            break;
        case JS_AST_BINARY_EXPR:
            free(node->as.binary.op);
            js_ast_free(node->as.binary.left);
            js_ast_free(node->as.binary.right);
            break;
        case JS_AST_UNARY_EXPR:
            free(node->as.unary.op);
            js_ast_free(node->as.unary.expr);
            break;
        case JS_AST_CALL_EXPR:
            js_ast_free(node->as.call.callee);
            for (int i = 0; i < node->as.call.arg_count; i++) {
                js_ast_free(node->as.call.args[i]);
            }
            free(node->as.call.args);
            break;
        case JS_AST_MEMBER_EXPR:
            js_ast_free(node->as.member.object);
            free(node->as.member.property);
            break;
        case JS_AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object.count; i++) {
                free(node->as.object.keys[i]);
                js_ast_free(node->as.object.values[i]);
            }
            free(node->as.object.keys);
            free(node->as.object.values);
            break;
        case JS_AST_IDENT:
            free(node->as.ident.name);
            break;
        case JS_AST_STRING:
            free(node->as.string.value);
            break;
        default:
            break;
    }
    free(node);
}
