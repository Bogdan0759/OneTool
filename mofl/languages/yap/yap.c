#include <stdio.h>
#include <stdlib.h>
#include <string.h>




/*
YAP LANGUAGE DOCUMENTATION

comment start with ;

vars hello world by name
DECLARE VAR name
SET name "John"
PRINT name

print options
PRINT NFORMAT hello        ; print raw value
PRINT "hello"              ; print string
PRINT 123                  ; print number

math operations
ADD 2, 3
PRINT OPRES

SUB 10 4
SET result OPRES

MUL result 2
PRINT OPRES

DIV 20 5
GOTO OPRES

math result is stored in system variable OPRES
return result is stored in system variable RETVAL

if blocks
IF name E "John"
PRINT "user is John"
ENDIF

IF 10 B 3
PRINT "ten is bigger"
ENDIF

IF NFORMAT name E "name"
PRINT "compare raw token without variable lookup"
ENDIF

supported compare operators:
B = bigger
S = smaller
E = equal

you can also write operator at the end:
IF 10 3 B
PRINT "same meaning as IF 10 B 3"
ENDIF

line numbers its adress (starting from 1)
each line is an address you can jump to

GOTO jumps to a line number
After GOTO finish (see RETURN) execution continues on next line after GOTO

1:  PRINT "start"
2:  GOTO 5
3:  PRINT "skipped"
4:  RETURN "done"           ; return to GOTO LINE + 1 line
5:  PRINT "subroutine"
6:  RETURN "back"           ; jump back to line 3
7:  PRINT RETVAL            ; print back str

*/


#define MAXTOKENS 450

struct lexer {
    char* text;
    char** token_list;
    int token_count;
    int token_capacity;
};

struct parsed_line {
    char* tokens[MAXTOKENS];
    int token_count;
};


void strip_comments_in_place(char* text) {
    int instr = 0;
    for (int i = 0; text[i] != '\0'; i++) {
        if (text[i] == '"') {
            instr = !instr;
        }
        if (!instr && text[i] == ';') {
            text[i] = '\0';
            return;
        }
    }
}

char* lstrip(char* str) {
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r' || *str == ',') {
        str++;
    }
    return str;
}

int is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

int match_keyword(char* text, const char* keyword) {
    size_t len = strlen(keyword);
    if (strncmp(text, keyword, len) != 0) {
        return 0;
    }
    return text[len] == '\0' || !is_ident_char(text[len]);
}

char* scan_word_token(struct lexer* lex, char* text) {
    int i = 0;
    while (is_ident_char(text[i])) {
        i++;
    }
    if (text[i] == ':') {
        i++;
    }
    if (text[i] != '\0') {
        text[i] = '\0';
        lex->text = text + i + 1;
    } else {
        lex->text = text + i;
    }
    return text;
}

char* scan_number_token(struct lexer* lex, char* text) {
    int i = 0;
    if (text[0] == '-') {
        i = 1;
    }
    while (text[i] >= '0' && text[i] <= '9') {
        i++;
    }
    if (text[i] != '\0') {
        text[i] = '\0';
        lex->text = text + i + 1;
    } else {
        lex->text = text + i;
    }
    return text;
}

char* scan_string_token(struct lexer* lex, char* text) {
    int i = 1;
    while (text[i] != '"' && text[i] != '\0') {
        i++;
    }
    if (text[i] != '"') {
        return NULL;
    }
    text[i] = '\0';
    if (text[i + 1] != '\0') {
        lex->text = text + i + 1;
    } else {
        lex->text = text + i;
    }
    return text;
}

char* get_next_token(struct lexer* lex) {
    char* text = lstrip(lex->text);
    lex->text = text;
    if (*text == '\0') {
        return NULL;
    }

    if (match_keyword(text, "PRINT")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "DECLARE")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "VAR")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "SET")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "NFORMAT")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "GOTO")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "RETURN")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "IF")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "ENDIF")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "ADD")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "SUB")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "MUL")) {
        return scan_word_token(lex, text);
    }

    if (match_keyword(text, "DIV")) {
        return scan_word_token(lex, text);
    }

    if (text[0] == '"') {
        return scan_string_token(lex, text);
    }

    if ((text[0] >= '0' && text[0] <= '9') ||
        (text[0] == '-' && text[1] >= '0' && text[1] <= '9')) {
        return scan_number_token(lex, text);
    }

    if ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_') {
        return scan_word_token(lex, text);
    }

    printf("unknown token: %s\n", text);
    return NULL;
}



char** tokenize(struct lexer* lex) {
    strip_comments_in_place(lex->text);
    while (lex->text && *lex->text != '\0') {
        char* token = get_next_token(lex);
        if (token) {
            if (lex->token_count >= lex->token_capacity) {
                printf("too many tokens in line\n");
                break;
            }
            lex->token_list[lex->token_count++] = token;
        } else {
            break;
        }
    }
    return lex->token_list;
}

#define MVARS 100
#define MAXVALUE 256

struct variable {
    char* name;
    char* value;
    char value_buffer[MAXVALUE];
    int is_string;
};

struct variable vars[MVARS];
int var_count = 0;
char opres_buffer[64] = "0";
extern char* retval;

int find_var_index(char* name) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

char* get_var(char* name) {
    if (strcmp(name, "RETVAL") == 0) {
        return retval ? retval : "";
    }
    if (strcmp(name, "OPRES") == 0) {
        return opres_buffer;
    }
    int index = find_var_index(name);
    if (index >= 0) {
        return vars[index].value;
    }
    return NULL;
}

void set_var(char* name, char* value, int is_string) {
    char temp_value[MAXVALUE];

    if (strcmp(name, "RETVAL") == 0 || strcmp(name, "OPRES") == 0) {
        printf("cannot assign system variable: %s\n", name);
        return;
    }
    if (value) {
        strncpy(temp_value, value, MAXVALUE - 1);
        temp_value[MAXVALUE - 1] = '\0';
    }

    int index = find_var_index(name);
    if (index >= 0) {
        if (value) {
            strcpy(vars[index].value_buffer, temp_value);
            vars[index].value = vars[index].value_buffer;
        } else {
            vars[index].value_buffer[0] = '\0';
            vars[index].value = NULL;
        }
        vars[index].is_string = is_string;
        return;
    }
    if (var_count >= MVARS) {
        printf("too many variables\n");
        return;
    }
    vars[var_count].name = name;
    if (value) {
        strcpy(vars[var_count].value_buffer, temp_value);
        vars[var_count].value = vars[var_count].value_buffer;
    } else {
        vars[var_count].value_buffer[0] = '\0';
        vars[var_count].value = NULL;
    }
    vars[var_count].is_string = is_string;
    var_count++;
}

int is_integer(char* str) {
    int i = 0;
    if (str[0] == '-') {
        if (str[1] == '\0') return 0;
        i = 1;
    }
    for (; str[i]; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;
    }
    return 1;
}

int is_string(char* str) {
    return str[0] == '"';
}

char* extract_string_value(char* str) {
    if (str[0] == '"') {
        return str + 1;
    }
    return str;
}

int is_compare_operator(char* token) {
    return strcmp(token, "B") == 0 ||
           strcmp(token, "S") == 0 ||
           strcmp(token, "E") == 0;
}

char* resolve_token_value(char* value, int use_nformat) {
    if (is_string(value)) {
        return extract_string_value(value);
    }
    if (is_integer(value)) {
        return value;
    }
    if (use_nformat) {
        return value;
    }

    char* var_value = get_var(value);
    if (var_value) {
        return var_value;
    }
    return value;
}

int resolve_integer_token(char* value, int use_nformat, int* out) {
    char* resolved = resolve_token_value(value, use_nformat);
    if (!is_integer(resolved)) {
        return 0;
    }
    *out = atoi(resolved);
    return 1;
}

void set_opres_int(int value) {
    snprintf(opres_buffer, sizeof(opres_buffer), "%d", value);
}

int compare_values(char* left, char* op, char* right) {
    if (is_integer(left) && is_integer(right)) {
        int left_num = atoi(left);
        int right_num = atoi(right);

        if (strcmp(op, "B") == 0) return left_num > right_num;
        if (strcmp(op, "S") == 0) return left_num < right_num;
        if (strcmp(op, "E") == 0) return left_num == right_num;
        return 0;
    }

    int cmp = strcmp(left, right);
    if (strcmp(op, "B") == 0) return cmp > 0;
    if (strcmp(op, "S") == 0) return cmp < 0;
    if (strcmp(op, "E") == 0) return cmp == 0;
    return 0;
}

int evaluate_if_condition(char** tokens, int token_count, int* is_valid) {
    int pos = 1;
    int left_nformat = 0;
    int right_nformat = 0;
    char* left = NULL;
    char* right = NULL;
    char* op = NULL;

    *is_valid = 0;

    if (pos >= token_count) {
        return 0;
    }

    if (strcmp(tokens[pos], "NFORMAT") == 0) {
        left_nformat = 1;
        pos++;
    }
    if (pos >= token_count) {
        return 0;
    }
    left = tokens[pos++];

    if (pos >= token_count) {
        return 0;
    }

    if (is_compare_operator(tokens[pos])) {
        op = tokens[pos++];
        if (pos < token_count && strcmp(tokens[pos], "NFORMAT") == 0) {
            right_nformat = 1;
            pos++;
        }
        if (pos >= token_count) {
            return 0;
        }
        right = tokens[pos++];
    } else {
        if (strcmp(tokens[pos], "NFORMAT") == 0) {
            right_nformat = 1;
            pos++;
        }
        if (pos >= token_count) {
            return 0;
        }
        right = tokens[pos++];
        if (pos >= token_count || !is_compare_operator(tokens[pos])) {
            return 0;
        }
        op = tokens[pos++];
    }

    if (pos != token_count) {
        return 0;
    }

    *is_valid = 1;
    return compare_values(
        resolve_token_value(left, left_nformat),
        op,
        resolve_token_value(right, right_nformat)
    );
}

char* format_value(char* value, int use_nformat) {
    return resolve_token_value(value, use_nformat);
}

#define MAXLINES 1000
#define MAXCALLSTACK 100

char* lines[MAXLINES];
struct parsed_line parsed_lines[MAXLINES];
int line_count = 0;
int current_line = 0;

int call_stack_line[MAXCALLSTACK];
int call_stack_top = 0;
char* retval = NULL;

void exec_line(char** tokens, int token_count);

int find_matching_endif(int start_line) {
    int depth = 0;

    for (int i = start_line + 1; i < line_count; i++) {
        int token_count = parsed_lines[i].token_count;
        if (token_count == 0) {
            continue;
        }

        char* token = parsed_lines[i].tokens[0];
        if (strcmp(token, "IF") == 0) {
            depth++;
        } else if (strcmp(token, "ENDIF") == 0) {
            if (depth == 0) {
                return i;
            }
            depth--;
        }
    }

    return -1;
}

int return_flag = 0;

void split_lines(char* code) {
    line_count = 0;
    char* line = strtok(code, "\n");
    while (line != NULL && line_count < MAXLINES) {
        lines[line_count++] = line;
        line = strtok(NULL, "\n");
    }
}

void parse_lines(void) {
    for (int i = 0; i < line_count; i++) {
        struct lexer lex;
        lex.text = lines[i];
        lex.token_list = parsed_lines[i].tokens;
        lex.token_count = 0;
        lex.token_capacity = MAXTOKENS;

        tokenize(&lex);
        parsed_lines[i].token_count = lex.token_count;
    }
}

void exec(char* code) {
    split_lines(code);
    parse_lines();
    
    call_stack_top = 0;
    retval = NULL;
    set_opres_int(0);
    
    current_line = 0;
    while (current_line < line_count) {
        int executed_line = current_line;

        if (parsed_lines[current_line].token_count > 0) {
            exec_line(parsed_lines[current_line].tokens, parsed_lines[current_line].token_count);
        }

        if (current_line == executed_line) {
            current_line++;
        }
    }
}

void exec_line(char** tokens, int token_count) {
    int pos = 0;
    while (pos < token_count) {
        char* token = tokens[pos];
        
        if (return_flag) {
            return_flag = 0;
            return;
        }
        
        if (strcmp(token, "DECLARE") == 0) {
            pos++;
            if (strcmp(tokens[pos], "VAR") == 0) {
                pos++;
                char* var_name = tokens[pos];
                set_var(var_name, NULL, 0);
                pos++;
            }
        }
        else if (strcmp(token, "SET") == 0) {
            pos++;
            char* var_name = tokens[pos];
            pos++;
            char* value = tokens[pos];
            
            if (is_string(value)) {
                set_var(var_name, extract_string_value(value), 1);
            } else {
                char* resolved = resolve_token_value(value, 0);
                set_var(var_name, resolved, !is_integer(resolved));
            }
            pos++;
        }
        else if (strcmp(token, "PRINT") == 0) {
            pos++;
            int use_nformat = 0;
            if (strcmp(tokens[pos], "NFORMAT") == 0) {
                use_nformat = 1;
                pos++;
            }
            
            char* value = tokens[pos];
            char* formatted = format_value(value, use_nformat);
            printf("%s\n", formatted);
            pos++;
        }
        else if (strcmp(token, "GOTO") == 0) {
            pos++;
            char* target_value = resolve_token_value(tokens[pos], 0);
            if (!is_integer(target_value)) {
                printf("invalid goto target: %s\n", target_value);
                current_line = line_count;
                return;
            }
            int target_line = atoi(target_value);
            pos++;

            if (call_stack_top >= MAXCALLSTACK) {
                printf("call stack overflow\n");
                current_line = line_count;
                return;
            }
            if (target_line < 1 || target_line > line_count) {
                printf("invalid goto target: %d\n", target_line);
                current_line = line_count;
                return;
            }
            call_stack_line[call_stack_top++] = current_line + 1;
            current_line = target_line - 1;
            return;
        }
        else if (strcmp(token, "RETURN") == 0) {
            pos++;
            char* value = tokens[pos];

            retval = resolve_token_value(value, 0);
            pos++;
            
            if (call_stack_top > 0) {
                current_line = call_stack_line[--call_stack_top];
            }
            return;
        }
        else if (strcmp(token, "ADD") == 0 ||
                 strcmp(token, "SUB") == 0 ||
                 strcmp(token, "MUL") == 0 ||
                 strcmp(token, "DIV") == 0) {
            int left = 0;
            int right = 0;
            int result = 0;

            if (token_count != 3) {
                printf("invalid %s syntax\n", token);
                current_line = line_count;
                return;
            }
            if (!resolve_integer_token(tokens[1], 0, &left) ||
                !resolve_integer_token(tokens[2], 0, &right)) {
                printf("%s expects integer values\n", token);
                current_line = line_count;
                return;
            }

            if (strcmp(token, "ADD") == 0) {
                result = left + right;
            } else if (strcmp(token, "SUB") == 0) {
                result = left - right;
            } else if (strcmp(token, "MUL") == 0) {
                result = left * right;
            } else {
                if (right == 0) {
                    printf("division by zero\n");
                    current_line = line_count;
                    return;
                }
                result = left / right;
            }

            set_opres_int(result);
            return;
        }
        else if (strcmp(token, "IF") == 0) {
            int is_valid = 0;
            if (!evaluate_if_condition(tokens, token_count, &is_valid)) {
                if (!is_valid) {
                    printf("invalid IF syntax\n");
                    current_line = line_count;
                    return;
                }
                int endif_line = find_matching_endif(current_line);
                if (endif_line < 0) {
                    printf("missing ENDIF\n");
                    current_line = line_count;
                    return;
                }
                current_line = endif_line + 1;
                return;
            }
            return;
        }
        else if (strcmp(token, "ENDIF") == 0) {
            return;
        }
        else {
            pos++;
        }
    }
}

char* read_file(char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        printf("error\n");
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = malloc(size + 1);
    fread(buffer, 1, size, file);
    buffer[size] = '\0';
    fclose(file);
    return buffer;
}

int main(int argc, char* argv[]) {
    char path[256];
    
    if (argc > 1) {
        strncpy(path, argv[1], 255);
        path[255] = '\0';
    } else {
        printf("path to file");
        scanf("%255s", path);
    }
    
    char* code = read_file(path);
    if (code) {
        exec(code);
        free(code);
    }
    return 0;
}
