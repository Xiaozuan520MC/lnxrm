; usr/crt0.S -- user program entry. Kernel pushes argc at [rsp], argv follows.
BITS 64
global _start
extern main

section .text
_start:
    xor ebp, ebp
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    call main
    mov rdi, rax                ; exit code
    mov eax, 60                 ; SYS_exit
    int 0x80
.hang:
    jmp .hang
