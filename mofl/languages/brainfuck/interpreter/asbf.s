# asbf.s - assembled brain fuck interprete

/* THIS FILE IS THE PART OF OneTool PROJECT (see github.com/bogdan0759/OneTool)




*/ 

.section .data
    usage_msg:    .ascii "asbf <file>\n"
    usage_len = . - usage_msg
    err_open_msg: .ascii "cant open file\n"
    err_open_len = . - err_open_msg
    err_bracket_msg: .ascii "mismatched brackets"
    err_bracket_len = . - err_bracket_msg
    
.section .bss
    .lcomm tape, 65536        # 64KB
    .lcomm stat_buf, 144      # fstat buff
    .lcomm stack, 131072      
    .lcomm ir_buf, 2097152    

.section .text
.global _start
_start:
    popq %rax        
    cmpq $2, %rax
    jl .usage_err
    popq %rax   
    popq %rdi    
    movq $2, %rax    
    xorq %rsi, %rsi
    syscall
    testq %rax, %rax
    js .open_err
    movq %rax, %r8   
    movq $5, %rax            
    movq %r8, %rdi
    movq $stat_buf, %rsi
    syscall
    movq 48(%rsi), %r9
    movq $9, %rax
    xorq %rdi, %rdi
    movq %r9, %rsi          
    movq $1, %rdx            
    movq $2, %r10            
    xorq %r11, %r11          
    movq %r11, %r9            
    syscall
    movq %rax, %r10       
    movq %rsi, %r11           
    addq %r10, %r11
    movq $ir_buf, %r13
    movq $stack, %r14         
.c_loop:
    cmpq %r11, %r10
    jae .c_done
    movb (%r10), %al
    cmpb $'+', %al
    je .c_val
    cmpb $'-', %al
    je .c_val
    cmpb $'>', %al
    je .c_ptr
    cmpb $'<', %al
    je .c_ptr
    cmpb $'[', %al
    je .c_jz
    cmpb $']', %al
    je .c_jnz
    cmpb $'.', %al
    je .c_out
    cmpb $',', %al
    je .c_in
    incq %r10
    jmp .c_loop
.c_val:
    xorq %rcx, %rcx
.c_val_l:
    cmpq %r11, %r10
    jae .c_val_e
    movb (%r10), %al
    cmpb $'+', %al
    je .c_val_inc
    cmpb $'-', %al
    je .c_val_dec
    jmp .c_val_e
.c_val_inc:
    incb %cl
    incq %r10
    jmp .c_val_l
.c_val_dec:
    decb %cl
    incq %r10
    jmp .c_val_l
.c_val_e:
    testb %cl, %cl
    jz .c_loop
    movq $.h_val_add, (%r13)
    movsbq %cl, %rax
    movq %rax, 8(%r13)
    addq $16, %r13
    jmp .c_loop
.c_ptr:
    xorq %rcx, %rcx
.c_ptr_l:
    cmpq %r11, %r10
    jae .c_ptr_e
    movb (%r10), %al
    cmpb $'>', %al
    je .c_ptr_inc
    cmpb $'<', %al
    je .c_ptr_dec
    jmp .c_ptr_e
.c_ptr_inc:
    incq %rcx
    incq %r10
    jmp .c_ptr_l
.c_ptr_dec:
    decq %rcx
    incq %r10
    jmp .c_ptr_l
.c_ptr_e:
    testq %rcx, %rcx
    jz .c_loop
    movq $.h_ptr_add, (%r13)
    movq %rcx, 8(%r13)
    addq $16, %r13
    jmp .c_loop
.c_out:
    movq $.h_out, (%r13)
    addq $16, %r13
    incq %r10
    jmp .c_loop
.c_in:
    movq $.h_in, (%r13)
    addq $16, %r13
    incq %r10
    jmp .c_loop
.c_jz:
    movq %r13, (%r14)          
    addq $8, %r14
    movq $.h_jz, (%r13)
    movq $0, 8(%r13)           
    addq $16, %r13
    incq %r10
    jmp .c_loop
.c_jnz:
    cmpq $stack, %r14
    je .bracket_err
    subq $8, %r14
    movq (%r14), %rbx          
    movq $.h_jnz, (%r13)
    movq %rbx, 8(%r13)         
    movq %r13, 8(%rbx)
    addq $16, %r13
    incq %r10
    jmp .c_loop
.c_done:
    cmpq $stack, %r14
    jne .bracket_err
    movq $.h_exit, (%r13)
    movq $ir_buf, %r12       
    movq $tape, %rbp       
    jmp *(%r12)                

.next:
    addq $16, %r12             
    jmp *(%r12)                

.h_ptr_add:
    addq 8(%r12), %rbp    
    jmp .next
    
.h_val_add:
    movq 8(%r12), %rax
    addb %al, (%rbp)        
    jmp .next

.h_out:
    movq $1, %rax             
    movq $1, %rdi             
    movq %rbp, %rsi           
    movq $1, %rdx              
    syscall
    jmp .next

.h_in:
    xorq %rax, %rax         
    xorq %rdi, %rdi           
    movq %rbp, %rsi         
    movq $1, %rdx    
    syscall
    testq %rax, %rax  
    jnz .next
    movb $0, (%rbp)            
    jmp .next

.h_jz:
    cmpb $0, (%rbp)
    jne .next
    movq 8(%r12), %r12         
    jmp .next

.h_jnz:
    cmpb $0, (%rbp)
    je .next
    movq 8(%r12), %r12         
    jmp .next

.h_exit:
    movq $60, %rax      
    xorq %rdi, %rdi
    syscall

.usage_err:
    movq $1, %rax          
    movq $2, %rdi        
    movq $usage_msg, %rsi
    movq $usage_len, %rdx
    syscall
    movq $60, %rax            
    movq $1, %rdi
    syscall

.open_err:
    movq $1, %rax          
    movq $2, %rdi             
    movq $err_open_msg, %rsi
    movq $err_open_len, %rdx
    syscall
    movq $60, %rax           
    movq $1, %rdi
    syscall

.bracket_err:
    movq $1, %rax            
    movq $2, %rdi             
    movq $err_bracket_msg, %rsi
    movq $err_bracket_len, %rdx
    syscall
    movq $60, %rax            
    movq $1, %rdi
    syscall
