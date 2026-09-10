; signal_trampoline.S -- user-space signal return trampoline.
; Mapped at a fixed address in each process. When a signal handler returns,
; it calls this code which issues sigreturn() syscall.

BITS 64
section .text
global _signal_trampoline
global _signal_trampoline_size

_signal_trampoline:
    ; signal handler returns here.
    ; The kernel pushed a signal frame on the user stack.
    ; We just need to call sigreturn() to restore the original context.
    mov rax, 201        ; SYS_sigreturn
    int 0x80
    ; should not return
.hang:
    jmp .hang

_signal_trampoline_size: dq _signal_trampoline_end - _signal_trampoline
_signal_trampoline_end:
