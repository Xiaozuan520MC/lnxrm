; lnxrm -- bzImage real-mode setup
; Layout follows the Linux x86 boot protocol (Documentation/arch/x86/boot.rst):
;   offset 0x1F1 setup_sects, 0x202 "HdrS", 0x200 entry point.
; Loaded by SeaBIOS/QEMU at 0x9000:0000, entered at 0x9000:0200.
; Job: enable A20 -> protected mode -> build temporary page tables ->
;      activate long mode -> far jump to the 64-bit kernel at 0x100000.

BITS 16
ORG 0

; No boot sector is required for -kernel loading; QEMU/SeaBIOS enter at
; offset 0x200. Pad sector 0 so the header lands at the protocol offsets.
times 0x1F1-($-$$) db 0
setup_sects:    db 0                ; patched by scripts/patch_bzimage.py
                dw 0                ; 0x1F2 root_flags
syssize:        dd 0                ; 0x1F4 payload size /16, patched
                dw 0                ; 0x1F8 ram_size
                dw 0xFFFF           ; 0x1FA vid_mode = ask
                dw 0                ; 0x1FC root_dev
boot_flag:      dw 0xAA55           ; 0x1FE

entry_jump:     jmp short real_start    ; 0x200: loader enters here
                                ; 0x202 header signature
                db 'H','d','r','S'
version:        dw 0x0208           ; protocol 2.08
realmode_swtch: dd 0                ; none
start_sys_seg:  dw 0x1000
kernel_version: dw kvstr - $$       ; offset of version string
type_of_loader: db 0
loadflags:      db 0x81             ; LOADED_HIGH | CAN_USE_HEAP
setup_move_size: dw 0x8000
code32_start:   dd 0x100000         ; payload load address
ramdisk_image:  dd 0
ramdisk_size:   dd 0
bootsect_kludge: dd 0
heap_end_ptr:   dw 0xEE00
ext_loader_ver: db 0
ext_loader_type: db 0
cmd_line_ptr:   dd 0                ; filled by loader
initrd_addr_max: dd 0x7FFFFFFF
kernel_alignment: dd 16
relocatable_kernel: db 0
min_alignment:  db 0
xloadflags:     dw 1                ; XLF_KERNEL_64
cmdline_size:   dd 2048
hardware_subarch: dd 0
hw_subarch_data: dq 0
payload_offset: dd 0
payload_length: dd 0
setup_data:     dq 0
pref_address:   dq 0
init_size:      dd 0x400000
handover_offset: dd 0

times 0x270-($-$$) db 0             ; canary: overrunning the header fails here

; ------------------------------------------------------------ real mode ---
%macro DBG 1
    mov al, %1
    out 0xE9, al
%endmacro

real_start:
    cli
    cld
    DBG '1'
    mov ax, cs          ; debug: dump CS
    out 0xE9, al
    mov al, ah
    out 0xE9, al 
    ; Derive our own segment: we were entered at SEG:0x200 where SEG holds
    ; file offset 0, so the setup base segment is CS-0x20.
    mov ax, cs
    sub ax, 0x20
    mov ds, ax
    mov es, ax

    ; Linear address of the zero page handed over in ds:si (protocol >= 2.02).
    xor eax, eax
    mov ax, ds
    shl eax, 4
    movzx esi, si
    add eax, esi
    mov [zp_linear], eax

    mov ss, ax                      ; stack inside the setup segment
    mov sp, 0xEE00                  ; below heap_end_ptr, linear 0x9EE00

    ; ---- enable A20 ----
    DBG '2'
    mov ax, 0x2401                  ; BIOS: enable A20
    int 0x15                        ; clobbers registers -- already stashed
    in  al, 0x92                    ; fast A20 gate as fallback
    or  al, 2
    and al, 0xFE
    out 0x92, al
    DBG '3'

    ; ---- capture the E820 memory map ourselves -------------------------
    ; table stored at linear 0x7000, dword count at 0x6FFC
    cli
    xor eax, eax
    mov es, ax                      ; ES = 0 for the buffer
    mov di, 0x7000
    xor ebx, ebx                    ; continuation
    mov DWORD [es:0x6FFC], 0        ; counter
.e820_loop:
    mov eax, 0xE820
    mov edx, 0x534D4150             ; 'SMAP'
    mov ecx, 20
    int 0x15
    jc  .e820_done
    cmp eax, 0x534D4150
    jne .e820_done
    add di, 20
    inc DWORD [es:0x6FFC]
    test ebx, ebx
    jz  .e820_done
    cmp DWORD [es:0x6FFC], 64
    jb  .e820_loop
.e820_done:
    ; NOTE: interrupts stay OFF all the way until start_kernel's final sti

    ; ---- protected mode ----
    cli
    mov ebx, [zp_linear]
    DBG '4'
    ; patch the GDT descriptor's base with the linear address of |gdt|
    xor eax, eax
    mov ax, ds
    shl eax, 4
    add [gdtdesc+2], eax
    lgdt [gdtdesc]
    DBG '5'
    mov eax, cr0
    or  eax, 1                      ; CR0.PE
    mov cr0, eax
    ; Patch the protected-mode far jump: NASM emits a link-time offset,
    ; but we need the runtime-linear address (setup base + offset).
    xor eax, eax
    mov ax, ds
    shl eax, 4
    add eax, pm_entry
    mov [pm_jmp+2], eax
    DBG '6'
pm_jmp:
    jmp dword 0x08:0xF00DFACE       ; patched above

BITS 32
pm_entry:
    DBG '7'
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, 0x7C00

    ; ---- temporary page tables @0x60000 -----------------------------------
    ; pml4[0]  -> pdpt (identity 1 GiB)
    ; pml4[511]-> same pdpt, whose entry 510 maps the high half (2 MiB pages)
    ; pdpt@0x61000, pd_low@0x62000, pd_high@0x63000
    mov DWORD [0x60000+0*8],   0x61003
    mov DWORD [0x60000+511*8], 0x61003
    mov DWORD [0x61000+0*8],   0x62003
    mov DWORD [0x61000+510*8], 0x63003

    ; identity map low 1 GiB with 2 MiB pages
    mov edi, 0x62000
    mov eax, 0x83                   ; P|W|PS, phys 0
    mov ecx, 512
.idloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .idloop

    ; high half: kernel image first 32 x 2 MiB => 64 MiB window
    mov edi, 0x63000
    mov eax, 0x83                   ; phys 0 (kernel is at 1 MiB)
    mov ecx, 32
.hiloop:
    mov [edi], eax
    mov DWORD [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .hiloop
    ; zero remaining pd_high entries
    mov ecx, 480
.zloop:
    mov DWORD [edi], 0
    mov DWORD [edi+4], 0
    add edi, 8
    loop .zloop

    ; VGA text window (PD index 192) -> PT @0x64000 -> phys 0xB8000
    mov DWORD [0x63000+96*8], 0x64003
    mov DWORD [0x64000], 0xB8003

    ; ---- long mode ----
    mov eax, cr4
    or  eax, 0x20                   ; CR4.PAE
    mov cr4, eax
    mov edx, 0x60000
    mov cr3, edx
    mov ecx, 0xC0000080             ; EFER
    rdmsr
    or  eax, 0x100                  ; EFER.LME
    wrmsr
    mov eax, cr0
    or  eax, 0x80000000             ; CR0.PG
    mov cr0, eax

    jmp 0x18:0x100000               ; 64-bit code segment -> kernel entry

; ------------------------------------------------------------------ data ---
zp_linear:  dd 0
payload_base: dd 0x100000
align 4
scan_tbl:   dd 0x100000, 0x110000, 0x010600, 0x020000
            dd 0x300000, 0x400000, 0x500000, 0x600000
align 8
gdt:
    dq 0                            ; null
    dq 0x00CF9A000000FFFF           ; 0x08 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10 data
    dq 0x00209A0000000000           ; 0x18 64-bit code (L=1)
gdtdesc:
    dw gdtdesc - gdt - 1
    dd gdt

kvstr:      db "lnxrm v1.0 (x86_64)",0
