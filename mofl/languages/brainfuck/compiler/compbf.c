#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* brainfuck compiler to gas in c
THIS FILE IS THE PART OF OneTool PROJECT (see github.com/bogdan0759/OneTool)
*/

typedef struct { int offset; int factor; } Move;
typedef struct {
    char type; long val; long target;
    Move *moves; int move_cnt;
} IR;

IR *ir = NULL;
int ir_cnt = 0, ir_cap = 0;

void add_ir(IR entry) {
    if (ir_cnt >= ir_cap) {
        ir_cap = ir_cap ? ir_cap * 2 : 1024;
        ir = realloc(ir, ir_cap * sizeof(IR));
    }
    ir[ir_cnt++] = entry;
}

void compile_to_ir(const char *code) {
    int len = strlen(code);
    int *stack = malloc(len * sizeof(int)), sp = 0;
    for (int i = 0; i < len; ) {
        char c = code[i];
        if (c == '+' || c == '-') {
            int n = 0;
            while (i < len && (code[i] == '+' || code[i] == '-')) n += (code[i++] == '+') ? 1 : -1;
            if (n % 256 != 0) add_ir((IR){'A', n, 0, NULL, 0});
        } 
        else if (c == '>' || c == '<') {
            int n = 0;
            while (i < len && (code[i] == '>' || code[i] == '<')) n += (code[i++] == '>') ? 1 : -1;
            if (n != 0) add_ir((IR){'S', n, 0, NULL, 0});
        }
        else if (c == '.') { add_ir((IR){'.', 0, 0, NULL, 0}); i++; }
        else if (c == ',') { add_ir((IR){',', 0, 0, NULL, 0}); i++; }
        else if (c == '[') {
            int j = i + 1;
            if (j < len && (code[j] == '-' || code[j] == '+')) {
                j++;
                if (j < len && code[j] == ']') { add_ir((IR){'C', 0, 0, NULL, 0}); i = j + 1; continue; }
                Move *ms = malloc(32 * sizeof(Move));
                int mc = 0, pt = 0, vld = 1;
                while (j < len && code[j] != ']') {
                    if (code[j] == '>') { pt++; j++; }
                    else if (code[j] == '<') { pt--; j++; }
                    else if (code[j] == '+' || code[j] == '-') {
                        int v = 0; while(j < len && (code[j] == '+' || code[j] == '-')) v += (code[j++] == '+') ? 1 : -1;
                        if (pt == 0) { vld = 0; break; }
                        ms[mc++] = (Move){pt, v};
                    } else { vld = 0; break; }
                }
                if (vld && j < len && code[j] == ']' && pt == 0) { add_ir((IR){'M', 0, 0, ms, mc}); i = j + 1; continue; }
                free(ms);
            }
            stack[sp++] = ir_cnt; add_ir((IR){'[', 0, 0, NULL, 0}); i++;
        }
        else if (c == ']') {
            int st = stack[--sp]; ir[st].target = ir_cnt;
            add_ir((IR){']', 0, st, NULL, 0}); i++;
        }
        else i++;
    }
    free(stack);
}

void emit_asm(FILE *out) {
    fprintf(out, ".section .bss\n    .lcomm tape, 1048576\n");
    fprintf(out, ".section .text\n.global _start\n");

    fprintf(out, ".macro write\n    mov $1, %%rax\n    mov $1, %%rdi\n    mov %%rbp, %%rsi\n    mov $1, %%rdx\n    syscall\n.endm\n");
    fprintf(out, ".macro read\n    xor %%rax, %%rax\n    xor %%rdi, %%rdi\n    mov %%rbp, %%rsi\n    mov $1, %%rdx\n    syscall\n    test %%rax, %%rax\n    jnz 1f\n    movb $0, (%%rbp)\n1:\n.endm\n\n");

    fprintf(out, "_start:\n    movq $tape, %%rbp\n");

    for (int i = 0; i < ir_cnt; i++) {
        switch (ir[i].type) {
            case 'A': fprintf(out, "    addb $%d, (%%rbp)\n", (unsigned char)ir[i].val); break;
            case 'S': fprintf(out, "    addq $%ld, %%rbp\n", ir[i].val); break;
            case '.': fprintf(out, "    write\n"); break;
            case ',': fprintf(out, "    read\n"); break;
            case 'C': fprintf(out, "    movb $0, (%%rbp)\n"); break;
            case 'M': 
                fprintf(out, "    movb (%%rbp), %%al\n    testb %%al, %%al\n    jz 1f\n");
                for (int m = 0; m < ir[i].move_cnt; m++) {
                    if (ir[i].moves[m].factor == 1) fprintf(out, "    addb %%al, %d(%%rbp)\n", ir[i].moves[m].offset);
                    else if (ir[i].moves[m].factor == -1) fprintf(out, "    subb %%al, %d(%%rbp)\n", ir[i].moves[m].offset);
                    else {
                        fprintf(out, "    push %%rax\n    movb $%d, %%cl\n    mulb %%cl\n    addb %%al, %d(%%rbp)\n    pop %%rax\n", (unsigned char)ir[i].moves[m].factor, ir[i].moves[m].offset);
                    }
                }
                fprintf(out, "    movb $0, (%%rbp)\n1:\n");
                break;
            case '[': fprintf(out, "L%d:\n    cmpb $0, (%%rbp)\n    je E%ld\n", i, ir[i].target); break;
            case ']': fprintf(out, "    jmp L%ld\nE%d:\n", ir[i].target, i); break;
        }
    }
    fprintf(out, "    mov $60, %%rax\n    xor %%rdi, %%rdi\n    syscall\n");
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "r");
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    char *code = malloc(size + 1); fread(code, 1, size, f); code[size] = 0; fclose(f);
    compile_to_ir(code);
    FILE *out = (argc > 2) ? fopen(argv[2], "w") : stdout;
    emit_asm(out);
    return 0;
}
