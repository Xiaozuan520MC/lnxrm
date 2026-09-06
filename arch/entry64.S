; lnxrm -- 64-bit kernel entry, interrupt stubs, context switch
; Assembled with nasm -f elf64. Linked at VMA 0xffffffff80100000 (LMA 0x100000).

%define PML4_M    0x50000            ; master PML4 (permanent)
%define PDPT_M    0x51000            ; identity subtree PDPT
%define PD_M      0x52000            ; identity 1 GiB, 2 MiB pages
%define PDPT_HI   0x53000            ; high-half subtree PDPT (PML4[511])
%define PD_HI     0x54000            ; kernel/heap/dev/vga, 2 MiB pages
%define PT_VGA    0x55000            ; text framebuffer window
%define PT_DEV    0x56000            ; device MMIO window

BITS 64

section .text.prologue
global _start64
extern start_kernel
extern __bss_start
extern __bss_end
extern __boot_stack_top

global pd_hi                          ; C fills heap entries pre-switch
global pt_dev                         ; C fills AHCI ABAR mapping

; Entered from setup.asm in long mode, CS=0x18, low alias == high alias.
_start64:
    mov al, 'K'                     ; debugcon marker
    out 0xE9, al
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; ---- master page tables ----------------------------------------------
    xor ecx, ecx
.clr_pml4:
    mov DWORD [PML4_M + rcx*8], 0
    mov DWORD [PML4_M + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .clr_pml4

    mov QWORD [PML4_M + 0*8],   PDPT_M | 0x003
    mov QWORD [PML4_M + 511*8], PDPT_HI | 0x003

    xor ecx, ecx
.clr_pdptm:
    mov DWORD [PDPT_M + rcx*8], 0
    mov DWORD [PDPT_M + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .clr_pdptm
    mov DWORD [PDPT_M + 0*8], PD_M | 0x003

    xor ecx, ecx
.clr_pdpthi:
    mov DWORD [PDPT_HI + rcx*8], 0
    mov DWORD [PDPT_HI + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .clr_pdpthi
    mov DWORD [PDPT_HI + 510*8], PD_HI | 0x003

    mov al, 'b'
    out 0xE9, al
    ; identity map low 1 GiB (2 MiB pages)
    mov rdi, PD_M
    mov eax, 0x083
.idloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    cmp eax, 0x083 + 512*0x200000
    jb .idloop

    ; high half: kernel image window 0..32 x 2 MiB
    mov rdi, PD_HI
    mov eax, 0x083
.hiloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    cmp eax, 0x083 + 32*0x200000
    jb .hiloop
.zhi:                                   ; zero entries 32..511
    mov DWORD [edi], 0
    mov DWORD [edi+4], 0
    add edi, 8
    cmp edi, PD_HI + 512*8
    jb .zhi

    mov al, 'c'
    out 0xE9, al
    ; VGA window 0xffffffffb8000000 -> PD_HI[96] -> PT_VGA[0] = 0xB8000
    mov DWORD [PD_HI + 96*8], PT_VGA | 0x003
    mov rdi, PT_VGA
    xor ecx, ecx
.zvga:
    mov DWORD [rdi + rcx*8], 0
    mov DWORD [rdi + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .zvga
    mov DWORD [PT_VGA], 0xB8003

    ; device MMIO window -> PD_HI[98] -> PT_DEV (C fills in the PTE)
    mov DWORD [PD_HI + 128*8], PT_DEV | 0x003
    mov rdi, PT_DEV
    xor ecx, ecx
.zdev:
    mov DWORD [rdi + rcx*8], 0
    mov DWORD [rdi + rcx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb .zdev

.activate:
    mov al, 'd'
    out 0xE9, al
    mov rax, cr4
    or rax, 0x20                        ; PAE only
    mov cr4, rax
    mov al, 'f'
    out 0xE9, al
    mov rax, PML4_M
    mov cr3, rax                   ; switch to master tables
    mov al, 'e'
    out 0xE9, al
    ; ---- clear .bss ----
    movabs rdi, __bss_start
    movabs rcx, __bss_end
    sub rcx, rdi
    shr rcx, 3
    xor eax, eax
    rep stosq

    ; ---- boot stack, jump into C at the HIGH address ----
    mov al, 'M'
    out 0xE9, al
    movabs rsp, __boot_stack_top
    xor ebp, ebp
    mov edi, ebx                        ; struct boot_params * from setup.asm
    movabs rax, start_kernel
    call rax
.stop:
    cli
    hlt
    jmp .stop

; --------------------------------------------------------------------------
section .text

; void gdt_reload(const struct gdtr*);
; Reloads CS via a far return, other data segs + flags by hand.
global gdt_reload
gdt_reload:
    push rbx
    mov rbx, rdi
    lgdt [rbx]
    push QWORD 0x08                     ; KCODE
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    mov ax, 0x10                        ; KDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    pop rbx
    ret

global tss_load
tss_load:                               ; rdi = TSS selector
    mov ax, di
    ltr ax
    ret

; --------------------------------------------------------------------------
; Interrupt stubs. struct intr_frame (ascending addresses):
;   r15..r8, rbp,rdi,rsi,rdx,rcx,rax, intno, err, rip, cs, rflags, ussp, usss
extern isr_common
extern syscall_entry

%macro PUSHALL 0
    push rbx                       ; saved first (highest address)
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro POPALL 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    pop rbx
%endmacro

section .text.isr
%macro ISR_NOERR 1
isr%1:
    push QWORD 0                        ; synthetic err slot
    push QWORD %1                       ; vector
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16
    iretq
%endmacro

%macro ISR_ERR 1
isr%1:
    push QWORD %1                       ; CPU pushed err already
    PUSHALL
    mov rdi, rsp
    cld
    call isr_common
    POPALL
    add rsp, 16                         ; discard intno + err
    iretq
%endmacro

%assign v 0
%rep 8                                  ; 0..7 no error code
ISR_NOERR v
%assign v v+1
%endrep
ISR_ERR 8
ISR_NOERR 9
%assign v 10
%rep 5                                  ; 10..14 error code
ISR_ERR v
%assign v v+1
%endrep
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
%assign v 18
%rep 14                                 ; 18..31 no error code
ISR_NOERR v
%assign v v+1
%endrep
%assign v 32
%rep 16                                 ; IRQ0..IRQ15 -> vectors 32..47
ISR_NOERR v
%assign v v+1
%endrep

isr128:                                 ; syscall gate (ring 3)
    push QWORD 0
    push QWORD 128
    PUSHALL
    mov rdi, rsp
    cld
    call syscall_entry                  ; may block/schedule
    POPALL
    add rsp, 16
    iretq

global isr_stub_table
isr_stub_table:                         ; vectors 0..48 (48 == syscall gate)
%assign v 0
%rep 48
    dq isr%+v
%assign v v+1
%endrep
    dq isr128

; --------------------------------------------------------------------------
section .text

; void swtch(struct cpu_ctx **old, struct cpu_ctx *new);
; struct cpu_ctx = { r15,r14,r13,r12,rbp,rbx,rip }
global swtch
swtch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, [rsi]          ; new stack comes from *(next->ctx)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; First-entry glue for a brand-new task: its creator pushed the address of
; its interrupt frame onto its fresh kernel stack right below cpu_ctx.
global glue_first
glue_first:
    ; swtch()'s ret left rsp pointing at this task's interrupt frame
    POPALL
    add rsp, 16
    iretq

; void tf_exit(struct intr_frame *f): restore GP regs and iretq.
; Used by sched paths that resume a task through its stored frame.
global tf_exit
tf_exit:
    mov rsp, rdi
    POPALL
    add rsp, 16
    iretq

; uint64_t read_cr3(void) / void load_cr3(uint64_t)
global read_cr3
read_cr3:
    mov rax, cr3
    ret

global load_cr3
load_cr3:
    mov cr3, rdi
    ret

global invlpg
invlpg:                                 ; rdi = vaddr
    invlpg [rdi]
    ret
