/* lnxrm C entry point: subsystem bring-up in dependency order. */
#include <boot.h>
#include <console.h>
#include <mm.h>
#include <cpu.h>
#include <sched.h>
#include <vfs.h>
#include <ahci.h>
#include <syscall.h>

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

void start_kernel(struct boot_params *bp)
{
    dbgmark('N');
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
    kprintf("\nlnxrm v1.0 -- x86_64, built %s %s\n", __DATE__,
            __TIME__);
    kprintf("[boot] %d usable e820 entries\n", bootinfo.map_len);
    pmm_init();
    vmm_init();                     /* switches CR3 to master tables */
    kheap_init();
    cpu_init();
    pit_init(HZ);

    irq_install(1, kbd_irq_handler);        /* PS/2 keyboard */
    extern void serial_rx_handler(struct intr_frame *);
    irq_install(4, serial_rx_handler);      /* COM1 */
    extern int lnxrm_uart_irq_enable(void);
    lnxrm_uart_irq_enable();

    rust_hello(rust_selftest());

    vfs_init();
    vfs_mount_root();
    ahci_init();
    if (vfs_try_mount_disk() == 0)
        kprintf("[vfs] FAT32 disk mounted at /mnt\n");
    else
        kprintf("[vfs] no disk found, running from initramfs only\n");

    do_global_ctors();

    sched_init();
    kernel_spawn("/bin/init");
    {
        extern void pid_canary_sync(void);
        pid_canary_sync();
    }

    /* hand control to the first user process */
    __asm__ volatile("sti");
    schedule();
    panic("schedule returned during boot");
}
