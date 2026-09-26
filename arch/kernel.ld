/* eck-system-kernel link script.
 * One contiguous PROGBITS image so VMA/LMA stay perfectly in step:
 * every byte satisfies VMA = LMA + (KERNEL_VMA - KERNEL_LMA).
 * objcopy -O binary dumps by LMA => payload starts at physical 0x100000. */

ENTRY(_start64)

KERNEL_LMA = 0x100000;
KERNEL_VMA = 0xffffffff80100000;

SECTIONS
{
    . = KERNEL_VMA;

    .image : AT(KERNEL_LMA)
    {
        __kernel_start = .;
        */entry64.o(.text.prologue)
        *(.text .text.*)

        *(.rodata .rodata.*)

        __init_array_start = .;
        KEEP(*(.init_array*))
        KEEP(*(.fini_array*))
        __init_array_end = .;

        *(.data .data.*)

        . = ALIGN(16);
    }

    .bss : AT(LOADADDR(.image) + SIZEOF(.image))
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
        . += 0x4000;                 /* boot stack */
        __boot_stack_top = .;
        __bss_end = .;
    }

    __kernel_end_lma = LOADADDR(.bss) + SIZEOF(.bss);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
        *(.comment)
    }
}
