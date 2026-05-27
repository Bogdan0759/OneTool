#include "../src/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void parser_bump(js_parser_t *p) {
    p->cur = p->next;
    js_lex_next(&p->lexer, &p->next);
}

static int parser_accept(js_parser_t *p, js_token_kind_t kind) {
    if (p->cur.kind != kind) return 0;
    parser_bump(p);
    return 1;
}

static int parser_expect(js_parser_t *p, js_token_kind_t kind, const char *msg) {
    if (p->cur.kind == kind) {
        parser_bump(p);
        return 1;
    }
    free(p->error);
    p->error = js_strdup(msg);
    return 0;
}

static char *tokdup(const js_token_t *tok) {
    return js_strndup(tok->start, tok->len);
}

static js_ast_t *parse_stmt(js_parser_t *p);
static js_ast_t *parse_expr(js_parser_t *p);

void js_parser_init(js_parser_t *p, const char *src) {
    memset(p, 0, sizeof(*p));
    js_lex_init(&p->lexer, src);
    js_lex_next(&p->lexer, &p->cur);
    js_lex_next(&p->lexer, &p->next);
}

void js_parser_deinit(js_parser_t *p) {
    free(p->error);
}

static js_ast_t *parse_block(js_parser_t *p) {
    if (!parser_expect(p, JS_TOK_LBRACE, "expected '{'")) return NULL;
    js_ast_t *node = js_ast_new(JS_AST_BLOCK);
    if (node == NULL) return NULL;
    while (p->cur.kind != JS_TOK_RBRACE && p->cur.kind != JS_TOK_EOF) {
        js_ast_t *stmt = parse_stmt(p);
        if (stmt == NULL) { js_ast_free(node); return NULL; }
        if (js_ast_list_push(&node->as.list, stmt) != 0) {
            js_ast_free(stmt);
            js_ast_free(node);
            return NULL;
        }
    }
    if (!parser_expect(p, JS_TOK_RBRACE, "expected '}'")) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_object_literal(js_parser_t *p) {
    if (!parser_expect(p, JS_TOK_LBRACE, "expected '{'")) return NULL;
    js_ast_t *node = js_ast_new(JS_AST_OBJECT_LITERAL);
    if (node == NULL) return NULL;
    while (p->cur.kind != JS_TOK_RBRACE && p->cur.kind != JS_TOK_EOF) {
        if (p->cur.kind != JS_TOK_IDENT && p->cur.kind != JS_TOK_STRING) {
            free(p->error);
            p->error = js_strdup("expected object key");
            js_ast_free(node);
            return NULL;
        }
        char *key = tokdup(&p->cur);
        parser_bump(p);
        if (!parser_expect(p, JS_TOK_COLON, "expected ':' after object key")) {
            free(key);
            js_ast_free(node);
            return NULL;
        }
        js_ast_t *value = parse_expr(p);
        if (value == NULL) {
            free(key);
            js_ast_free(node);
            return NULL;
        }
        int want = node->as.object.count + 1;
        char **keys = (char **)realloc(node->as.object.keys, (size_t)want * sizeof(*keys));
        js_ast_t **values = (js_ast_t **)realloc(node->as.object.values, (size_t)want * sizeof(*values));
        if (keys == NULL || values == NULL) {
            free(keys);
            free(values);
            free(key);
            js_ast_free(value);
            js_ast_free(node);
            return NULL;
        }
        node->as.object.keys = keys;
        node->as.object.values = values;
        node->as.object.keys[node->as.object.count] = key;
        node->as.object.values[node->as.object.count++] = value;
        if (!parser_accept(p, JS_TOK_COMMA)) break;
    }
    if (!parser_expect(p, JS_TOK_RBRACE, "expected '}' after object literal")) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_primary(js_parser_t *p) {
    js_ast_t *node = NULL;
    if (p->cur.kind == JS_TOK_NUMBER) {
        node = js_ast_new(JS_AST_NUMBER);
        if (node != NULL) node->as.number.value = p->cur.number;
        parser_bump(p);
        return node;
    }
    if (p->cur.kind == JS_TOK_STRING) {
        node = js_ast_new(JS_AST_STRING);
        if (node != NULL) node->as.string.value = tokdup(&p->cur);
        parser_bump(p);
        return node;
    }
    if (p->cur.kind == JS_TOK_IDENT) {
        node = js_ast_new(JS_AST_IDENT);
        if (node != NULL) node->as.ident.name = tokdup(&p->cur);
        parser_bump(p);
        return node;
    }
    if (p->cur.kind == JS_TOK_KW_TRUE || p->cur.kind == JS_TOK_KW_FALSE) {
        node = js_ast_new(JS_AST_BOOL);
        if (node != NULL) node->as.boolean.value = (p->cur.kind == JS_TOK_KW_TRUE);
        parser_bump(p);
        return node;
    }
    if (p->cur.kind == JS_TOK_KW_NULL) {
        parser_bump(p);
        return js_ast_new(JS_AST_NULL);
    }
    if (p->cur.kind == JS_TOK_KW_UNDEFINED) {
        parser_bump(p);
        return js_ast_new(JS_AST_UNDEFINED);
    }
    if (parser_accept(p, JS_TOK_LPAREN)) {
        node = parse_expr(p);
        if (node == NULL) return NULL;
        if (!parser_expect(p, JS_TOK_RPAREN, "expected ')'")) {
            js_ast_free(node);
            return NULL;
        }
        return node;
    }
    if (p->cur.kind == JS_TOK_LBRACE) {
        return parse_object_literal(p);
    }
    free(p->error);
    p->error = js_strdup("expected expression");
    return NULL;
}

static js_ast_t *parse_call(js_parser_t *p) {
    js_ast_t *expr = parse_primary(p);
    if (expr == NULL) return NULL;
    for (;;) {
        if (p->cur.kind == JS_TOK_DOT) {
            parser_bump(p);
            if (p->cur.kind != JS_TOK_IDENT) {
                free(p->error);
                p->error = js_strdup("expected property name after '.'");
                js_ast_free(expr);
                return NULL;
            }
            js_ast_t *member = js_ast_new(JS_AST_MEMBER_EXPR);
            if (member == NULL) {
                js_ast_free(expr);
                return NULL;
            }
            member->as.member.object = expr;
            member->as.member.property = tokdup(&p->cur);
            parser_bump(p);
            expr = member;
            continue;
        }
        if (p->cur.kind != JS_TOK_LPAREN) return expr;
        parser_bump(p);
        js_ast_t *call = js_ast_new(JS_AST_CALL_EXPR);
        if (call == NULL) { js_ast_free(expr); return NULL; }
        call->as.call.callee = expr;
        while (p->cur.kind != JS_TOK_RPAREN && p->cur.kind != JS_TOK_EOF) {
            js_ast_t *arg = parse_expr(p);
            if (arg == NULL) { js_ast_free(call); return NULL; }
            int want = call->as.call.arg_count + 1;
            js_ast_t **args = (js_ast_t **)realloc(call->as.call.args,
                                                   (size_t)want * sizeof(*args));
            if (args == NULL) {
                js_ast_free(arg);
                js_ast_free(call);
                return NULL;
            }
            call->as.call.args = args;
            call->as.call.args[call->as.call.arg_count++] = arg;
            if (!parser_accept(p, JS_TOK_COMMA)) break;
        }
        if (!parser_expect(p, JS_TOK_RPAREN, "expected ')' after args")) {
            js_ast_free(call);
            return NULL;
        }
        expr = call;
    }
}

static js_ast_t *parse_unary(js_parser_t *p) {
    if (p->cur.kind == JS_TOK_BANG || p->cur.kind == JS_TOK_MINUS || p->cur.kind == JS_TOK_PLUS) {
        js_ast_t *node = js_ast_new(JS_AST_UNARY_EXPR);
        if (node == NULL) return NULL;
        node->as.unary.op = tokdup(&p->cur);
        parser_bump(p);
        node->as.unary.expr = parse_unary(p);
        if (node->as.unary.expr == NULL) { js_ast_free(node); return NULL; }
        return node;
    }
    return parse_call(p);
}

static js_ast_t *parse_binary_chain(js_parser_t *p,
                                    js_ast_t *(*sub)(js_parser_t *),
                                    const js_token_kind_t *ops, int op_count) {
    js_ast_t *left = sub(p);
    if (left == NULL) return NULL;
    for (;;) {
        int match = 0;
        for (int i = 0; i < op_count; i++) {
            if (p->cur.kind == ops[i]) { match = 1; break; }
        }
        if (!match) return left;
        js_ast_t *node = js_ast_new(JS_AST_BINARY_EXPR);
        if (node == NULL) { js_ast_free(left); return NULL; }
        node->as.binary.op = tokdup(&p->cur);
        parser_bump(p);
        node->as.binary.left = left;
        node->as.binary.right = sub(p);
        if (node->as.binary.right == NULL) {
            js_ast_free(node);
            return NULL;
        }
        left = node;
    }
}

static js_ast_t *parse_mul(js_parser_t *p) {
    static const js_token_kind_t ops[] = { JS_TOK_STAR, JS_TOK_SLASH, JS_TOK_PERCENT };
    return parse_binary_chain(p, parse_unary, ops, 3);
}

static js_ast_t *parse_add(js_parser_t *p) {
    static const js_token_kind_t ops[] = { JS_TOK_PLUS, JS_TOK_MINUS };
    return parse_binary_chain(p, parse_mul, ops, 2);
}

static js_ast_t *parse_cmp(js_parser_t *p) {
    static const js_token_kind_t ops[] = {
        JS_TOK_LT, JS_TOK_LTE, JS_TOK_GT, JS_TOK_GTE
    };
    return parse_binary_chain(p, parse_add, ops, 4);
}

static js_ast_t *parse_eq(js_parser_t *p) {
    static const js_token_kind_t ops[] = { JS_TOK_EQEQ, JS_TOK_NEQ };
    return parse_binary_chain(p, parse_cmp, ops, 2);
}

static js_ast_t *parse_logic_and(js_parser_t *p) {
    static const js_token_kind_t ops[] = { JS_TOK_ANDAND };
    return parse_binary_chain(p, parse_eq, ops, 1);
}

static js_ast_t *parse_logic_or(js_parser_t *p) {
    static const js_token_kind_t ops[] = { JS_TOK_OROR };
    return parse_binary_chain(p, parse_logic_and, ops, 1);
}

static js_ast_t *parse_assign(js_parser_t *p) {
    js_ast_t *left = parse_logic_or(p);
    if (left == NULL) return NULL;
    if (p->cur.kind != JS_TOK_EQ) return left;
    if (left->kind != JS_AST_IDENT && left->kind != JS_AST_MEMBER_EXPR) {
        js_ast_free(left);
        free(p->error);
        p->error = js_strdup("invalid assignment target");
        return NULL;
    }
    js_ast_t *node = js_ast_new(JS_AST_ASSIGN_EXPR);
    if (node == NULL) { js_ast_free(left); return NULL; }
    if (left->kind == JS_AST_IDENT) {
        node->as.assign.name = js_strdup(left->as.ident.name);
    } else {
        size_t need = strlen(left->as.member.property) + 64;
        node->as.assign.name = (char *)malloc(need);
        if (node->as.assign.name == NULL) {
            js_ast_free(left);
            js_ast_free(node);
            return NULL;
        }
        if (left->as.member.object->kind == JS_AST_IDENT) {
            snprintf(node->as.assign.name, need, "%s.%s",
                     left->as.member.object->as.ident.name,
                     left->as.member.property);
        } else {
            free(node->as.assign.name);
            node->as.assign.name = NULL;
            js_ast_free(left);
            js_ast_free(node);
            free(p->error);
            p->error = js_strdup("unsupported member assignment target");
            return NULL;
        }
    }
    js_ast_free(left);
    parser_bump(p);
    node->as.assign.value = parse_assign(p);
    if (node->as.assign.value == NULL) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_expr(js_parser_t *p) {
    return parse_assign(p);
}

static js_ast_t *parse_var_decl(js_parser_t *p, int is_const) {
    parser_bump(p);
    if (p->cur.kind != JS_TOK_IDENT) {
        free(p->error);
        p->error = js_strdup("expected variable name");
        return NULL;
    }
    js_ast_t *node = js_ast_new(JS_AST_VAR_DECL);
    if (node == NULL) return NULL;
    node->as.var_decl.name = tokdup(&p->cur);
    node->as.var_decl.is_const = is_const;
    parser_bump(p);
    if (parser_accept(p, JS_TOK_EQ)) {
        node->as.var_decl.init = parse_expr(p);
        if (node->as.var_decl.init == NULL) {
            js_ast_free(node);
            return NULL;
        }
    }
    if (!parser_expect(p, JS_TOK_SEMI, "expected ';' after declaration")) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_func_decl(js_parser_t *p) {
    parser_bump(p);
    if (p->cur.kind != JS_TOK_IDENT) {
        free(p->error);
        p->error = js_strdup("expected function name");
        return NULL;
    }
    js_ast_t *node = js_ast_new(JS_AST_FUNC_DECL);
    if (node == NULL) return NULL;
    node->as.func_decl.name = tokdup(&p->cur);
    parser_bump(p);
    if (!parser_expect(p, JS_TOK_LPAREN, "expected '(' after function name")) {
        js_ast_free(node);
        return NULL;
    }
    while (p->cur.kind != JS_TOK_RPAREN && p->cur.kind != JS_TOK_EOF) {
        if (p->cur.kind != JS_TOK_IDENT) {
            free(p->error);
            p->error = js_strdup("expected parameter name");
            js_ast_free(node);
            return NULL;
        }
        int want = node->as.func_decl.param_count + 1;
        char **params = (char **)realloc(node->as.func_decl.params,
                                         (size_t)want * sizeof(*params));
        if (params == NULL) {
            js_ast_free(node);
            return NULL;
        }
        node->as.func_decl.params = params;
        node->as.func_decl.params[node->as.func_decl.param_count++] = tokdup(&p->cur);
        parser_bump(p);
        if (!parser_accept(p, JS_TOK_COMMA)) break;
    }
    if (!parser_expect(p, JS_TOK_RPAREN, "expected ')' after params")) {
        js_ast_free(node);
        return NULL;
    }
    node->as.func_decl.body = parse_block(p);
    if (node->as.func_decl.body == NULL) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_if(js_parser_t *p) {
    parser_bump(p);
    if (!parser_expect(p, JS_TOK_LPAREN, "expected '(' after if")) return NULL;
    js_ast_t *node = js_ast_new(JS_AST_IF_STMT);
    if (node == NULL) return NULL;
    node->as.if_stmt.cond = parse_expr(p);
    if (node->as.if_stmt.cond == NULL) { js_ast_free(node); return NULL; }
    if (!parser_expect(p, JS_TOK_RPAREN, "expected ')' after if condition")) {
        js_ast_free(node);
        return NULL;
    }
    node->as.if_stmt.then_branch = parse_stmt(p);
    if (node->as.if_stmt.then_branch == NULL) { js_ast_free(node); return NULL; }
    if (parser_accept(p, JS_TOK_KW_ELSE)) {
        node->as.if_stmt.else_branch = parse_stmt(p);
        if (node->as.if_stmt.else_branch == NULL) { js_ast_free(node); return NULL; }
    }
    return node;
}

static js_ast_t *parse_while(js_parser_t *p) {
    parser_bump(p);
    if (!parser_expect(p, JS_TOK_LPAREN, "expected '(' after while")) return NULL;
    js_ast_t *node = js_ast_new(JS_AST_WHILE_STMT);
    if (node == NULL) return NULL;
    node->as.while_stmt.cond = parse_expr(p);
    if (node->as.while_stmt.cond == NULL) { js_ast_free(node); return NULL; }
    if (!parser_expect(p, JS_TOK_RPAREN, "expected ')' after while condition")) {
        js_ast_free(node);
        return NULL;
    }
    node->as.while_stmt.body = parse_stmt(p);
    if (node->as.while_stmt.body == NULL) { js_ast_free(node); return NULL; }
    return node;
}

static js_ast_t *parse_return(js_parser_t *p) {
    parser_bump(p);
    js_ast_t *node = js_ast_new(JS_AST_RETURN_STMT);
    if (node == NULL) return NULL;
    if (p->cur.kind != JS_TOK_SEMI) {
        node->as.return_stmt.value = parse_expr(p);
        if (node->as.return_stmt.value == NULL) { js_ast_free(node); return NULL; }
    }
    if (!parser_expect(p, JS_TOK_SEMI, "expected ';' after return")) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_expr_stmt(js_parser_t *p) {
    js_ast_t *node = js_ast_new(JS_AST_EXPR_STMT);
    if (node == NULL) return NULL;
    node->as.expr_stmt.expr = parse_expr(p);
    if (node->as.expr_stmt.expr == NULL) { js_ast_free(node); return NULL; }
    if (!parser_expect(p, JS_TOK_SEMI, "expected ';' after expression")) {
        js_ast_free(node);
        return NULL;
    }
    return node;
}

static js_ast_t *parse_stmt(js_parser_t *p) {
    switch (p->cur.kind) {
        case JS_TOK_LBRACE: return parse_block(p);
        case JS_TOK_KW_LET: return parse_var_decl(p, 0);
        case JS_TOK_KW_CONST: return parse_var_decl(p, 1);
        case JS_TOK_KW_FUNCTION: return parse_func_decl(p);
        case JS_TOK_KW_IF: return parse_if(p);
        case JS_TOK_KW_WHILE: return parse_while(p);
        case JS_TOK_KW_RETURN: return parse_return(p);
        default: return parse_expr_stmt(p);
    }
}

js_ast_t *js_parse_program(js_parser_t *p) {
    js_ast_t *node = js_ast_new(JS_AST_PROGRAM);
    if (node == NULL) return NULL;
    while (p->cur.kind != JS_TOK_EOF) {
        js_ast_t *stmt = parse_stmt(p);
        if (stmt == NULL) {
            js_ast_free(node);
            return NULL;
        }
        if (js_ast_list_push(&node->as.list, stmt) != 0) {
            js_ast_free(stmt);
            js_ast_free(node);
            return NULL;
        }
    }
    return node;
}
