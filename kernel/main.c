/* lnxrm C entry point: subsystem bring-up in dependency order. */
#include <boot.h>
#include <console.h>
#include <mm.h>
#include <cpu.h>
#include <sched.h>
#include <vfs.h>
#include <ahci.h>
#include <syscall.h>
#include <io.h>
#include <apic.h>
#include <percpu.h>
#include <smp.h>
#include <framebuffer.h>

struct boot_info bootinfo;

/* C++ global constructors */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

static void do_global_ctors(void)
{
    for (size_t i = 0; &__init_array_start[i] < __init_array_end; i++)
        __init_array_start[i]();
}

/* Rust side hooks (rust/lib.rs) */
extern void rust_hello(u64 version);
extern long rust_selftest(void);

int kernel_spawn(const char *path);
void ps_dump(void);

bool kern_text_ptr(u64 p)
{
    extern char __kernel_start[];
    extern char __kernel_end[];
    return p >= (u64)__kernel_start && p < (u64)__kernel_end;
}

static inline void dbgmark(char c)
{
    __asm__ volatile("outb %0, $0xE9" :: "a"(c));
}

/* setup.asm stashes the raw int15-e820 table here */
#define E820_COUNT_ADDR 0x6FFCUL
#define E820_TABLE_ADDR 0x7000UL

/* setup.asm stores VBE framebuffer info here (phys) */
#define VBE_FB_INFO_ADDR 0x8C00UL

void start_kernel(struct boot_params *bp)
{
    (void)bp;

    /* ---- copy the memory map collected by our own real-mode stub ---- */
    bootinfo.map_len = 0;
    u32 n = *(volatile u32 *)E820_COUNT_ADDR;
    if (n > 64)
        n = 64;
    const volatile struct e820_entry *src =
        (const volatile struct e820_entry *)E820_TABLE_ADDR;
    for (u32 i = 0; i < n; i++) {
        if (!src[i].size)
            continue;
        bootinfo.map[bootinfo.map_len].addr = src[i].addr;
        bootinfo.map[bootinfo.map_len].size = src[i].size;
        bootinfo.map[bootinfo.map_len].type = src[i].type;
        bootinfo.map_len++;
    }
    
    console_init();
  kprintf("\nlnxrm v1.0 -- x86-64\n");
  kprintf("[boot] %d usable e820 entries\n", bootinfo.map_len);

    /* read VBE framebuffer info stored by setup.asm */
    {
        volatile u8 *vbe = (volatile u8 *)VBE_FB_INFO_ADDR;
        bootinfo.fb.phys_addr = *(volatile u32 *)(vbe + 0);
        bootinfo.fb.pitch     = *(volatile u16 *)(vbe + 4);
        bootinfo.fb.width     = *(volatile u16 *)(vbe + 6);
        bootinfo.fb.height    = *(volatile u16 *)(vbe + 8);
        bootinfo.fb.bpp       = vbe[0x0A];
        bootinfo.fb.ok        = vbe[0x0B];
    }

    /* init per-cpu data for BSP (cpu 0) */
    cpu_init_percpu(0, 0);

    pmm_init();
    vmm_init();
    kheap_init();
    cpu_init();
    pit_init(HZ);

    /* Map framebuffer into kernel VA (needs pmm + vmm ready). */
    if (bootinfo.fb.ok && bootinfo.fb.phys_addr) {
        fb_init();
        kprintf("[fb] %dx%d %d bpp, phys=0x%x, pitch=%d\n",
                bootinfo.fb.width, bootinfo.fb.height,
                bootinfo.fb.bpp, (u32)bootinfo.fb.phys_addr,
                bootinfo.fb.pitch);
    }

    /* PS/2 keyboard IRQ1 setup */
    while (inb(0x64) & 1) inb(0x60);
    outb(0x64, 0x20);
    while (!(inb(0x64) & 1));
    u8 cmd = inb(0x60);
    cmd |= 0x01;
    outb(0x64, 0x60);
    while (inb(0x64) & 2);
    outb(0x60, cmd);

    /* route PS/2 keyboard through IOAPIC (IRQ1 = vector 33) */
    ioapic_set_irq(1, 33, 0);
    ioapic_unmask_irq(1);
    irq_install(1, kbd_irq_handler);

    /* route COM1 serial through IOAPIC (IRQ4 = vector 36) */
    ioapic_set_irq(4, 36, 0);
    ioapic_unmask_irq(4);
    irq_install(4, serial_rx_handler);
    extern int lnxrm_uart_irq_enable(void);
    lnxrm_uart_irq_enable();

    rust_hello(rust_selftest());

    vfs_init();
    vfs_mount_root();

    /* try IDE first (QEMU built-in), then AHCI */
    extern void ide_init(void);
    ide_init();
    ahci_init();
    if (vfs_try_mount_disk() == 0)
        kprintf("[vfs] disk mounted at /mnt\n");
    else
        kprintf("[vfs] no disk found, running from initramfs only\n");

    do_global_ctors();

    sched_init();
    kernel_spawn("/bin/init");
    {
        extern void pid_canary_sync(void);
        pid_canary_sync();
    }

    /* ---- start SMP: launch Application Processors ---- */
    smp_init();

    /* hand control to the first user process */
    __asm__ volatile("sti");
    schedule();
    panic("schedule returned during boot");
}
