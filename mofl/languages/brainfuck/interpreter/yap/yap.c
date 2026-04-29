#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* tokens[10] = {
    "STRING",
    "INTEGER",
    "IDENTIFIER",
    "PRINT",
    "DECLARE",
    "VAR",
    "SET",
    "NFORMAT",
    "GOTO",
    "RETURN"
};




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


struct lexer {
    char* text;
    char* token_list[450];
    int token_count;
};


char* substring(char* str, int start, int end) {
    int len = end - start;
    char* result = (char*)malloc(len + 1);
    for (int i = 0; i < len; i++) {
        result[i] = str[start + i];
    }
    result[len] = '\0';
    return result;
}

char* delete_comments(char* token_list) {
    int len = strlen(token_list);
    int instr = 0;
    for (int i = 0; i < len; i++) {
        if (token_list[i] == '"') {
            instr = !instr;
        }
        if (!instr && token_list[i] == ';') {
            return substring(token_list, 0, i);
        }
    }
    return substring(token_list, 0, len);
}

char* lstrip(char* str) {
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') {
        str++;
    }
    return str;
}

int starts_with(char* str, char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

char* get_next_token(struct lexer* lex) {
    char* text = delete_comments(lex->text);
    text = lstrip(text);
    if (strlen(text) == 0) {
        return NULL;
    }
    
    if (starts_with(text, "PRINT")) {
        lex->text = text + 5;
        return "PRINT";
    }
    
    if (starts_with(text, "DECLARE")) {
        lex->text = text + 7;
        return "DECLARE";
    }
    
    if (starts_with(text, "VAR")) {
        lex->text = text + 3;
        return "VAR";
    }
    
    if (starts_with(text, "SET")) {
        lex->text = text + 3;
        return "SET";
    }
    
    if (starts_with(text, "NFORMAT")) {
        lex->text = text + 7;
        return "NFORMAT";
    }
    
    if (starts_with(text, "GOTO")) {
        lex->text = text + 4;
        return "GOTO";
    }
    
    if (starts_with(text, "RETURN")) {
        lex->text = text + 6;
        return "RETURN";
    }
    
    if (text[0] == '"') {
        int i = 1;
        while (text[i] != '"' && text[i] != '\0') {
            i++;
        }
        if (text[i] == '"') {
            lex->text = text + i + 1;
            return substring(text, 0, i + 1);
        }
    }
    
    if (text[0] >= '0' && text[0] <= '9') {
        int i = 0;
        while (text[i] >= '0' && text[i] <= '9') {
            i++;
        }
        lex->text = text + i;
        return substring(text, 0, i);
    }
    
    if ((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= 'A' && text[0] <= 'Z') || text[0] == '_') {
        int i = 0;
        while ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') || 
               (text[i] >= '0' && text[i] <= '9') || text[i] == '_') {
            i++;
        }
        if (text[i] == ':') {
            i++;
        }
        lex->text = text + i;
        return substring(text, 0, i);
    }
    
    printf("unknown token: %s\n", text);
    return NULL;
}



char** tokenize(struct lexer* lex) {
    while (lex->text && strlen(lex->text) > 0) {
        char* token = get_next_token(lex);
        if (token) {
            lex->token_list[lex->token_count++] = token;
        } else {
            break;
        }
    }
    return lex->token_list;
}

#define MVARS 100

struct variable {
    char* name;
    char* value;
    int is_string;
};

struct variable vars[MVARS];
int var_count = 0;

char* get_var(char* name) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            return vars[i].value;
        }
    }
    return NULL;
}

void set_var(char* name, char* value, int is_string) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            vars[i].value = value;
            vars[i].is_string = is_string;
            return;
        }
    }
    vars[var_count].name = name;
    vars[var_count].value = value;
    vars[var_count].is_string = is_string;
    var_count++;
}

int is_integer(char* str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;
    }
    return 1;
}

int is_string(char* str) {
    return str[0] == '"';
}

char* extract_string_value(char* str) {
    int len = strlen(str);
    if (len >= 2 && str[0] == '"' && str[len-1] == '"') {
        return substring(str, 1, len - 1);
    }
    return str;
}

char* format_value(char* value, int use_nformat) {
    if (use_nformat) {
        if (is_string(value)) {
            return extract_string_value(value);
        } else if (is_integer(value)) {
            return value;
        } else {
            return value;
        }
    } else {
        if (is_string(value)) {
            return extract_string_value(value);
        } else if (is_integer(value)) {
            return value;
        } else {
            char* var_value = get_var(value);
            if (var_value) {
                return var_value;
            } else {
                return value;
            }
        }
    }
}

#define MAXLINES 1000
#define MAXCALLSTACK 100

char* lines[MAXLINES];
int line_tokens_start[MAXLINES];
int line_count = 0;
int current_line = 0;

int call_stack_line[MAXCALLSTACK];
int call_stack_top = 0;
char* retval = NULL;

void exec_line(char** tokens, int token_count);

int return_flag = 0;

void split_lines(char* code) {
    line_count = 0;
    char* line = strtok(code, "\n");
    while (line != NULL && line_count < MAXLINES) {
        lines[line_count++] = line;
        line = strtok(NULL, "\n");
    }
}

void exec(char* code) {
    split_lines(code);
    
    call_stack_top = 0;
    retval = NULL;
    
    current_line = 0;
    while (current_line < line_count) {
        struct lexer lex;
        lex.text = lines[current_line];
        lex.token_count = 0;
        
        tokenize(&lex);
        
        if (lex.token_count > 0) {
            exec_line(lex.token_list, lex.token_count);
        }
        
        current_line++;
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
            } else if (is_integer(value)) {
                set_var(var_name, value, 0);
            } else {
                char* var_value = get_var(value);
                set_var(var_name, var_value, 1);
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
            int target_line = atoi(tokens[pos]);
            pos++;
            
            call_stack_line[call_stack_top++] = current_line + 1;
            current_line = target_line - 1; 
            return;
        }
        else if (strcmp(token, "RETURN") == 0) {
            pos++;
            char* value = tokens[pos];
            
            if (is_string(value)) {
                retval = extract_string_value(value);
            } else if (is_integer(value)) {
                retval = value;
            } else {
                char* var_value = get_var(value);
                if (var_value) {
                    retval = var_value;
                } else {
                    retval = value;
                }
            }
            pos++;
            
            if (call_stack_top > 0) {
                current_line = call_stack_line[--call_stack_top] - 1;
            }
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
