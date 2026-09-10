/* SMP initialization: AP startup via INIT-SIPI-SIPI, per-CPU setup. */
#include <smp.h>
#include <apic.h>
#include <percpu.h>
#include <mm.h>
#include <cpu.h>
#include <console.h>
#include <sched.h>
#include <io.h>
#include <spinlock.h>

/* Trampoline entry/exit (arch/trampoline.S) */
extern u8 trampoline_start[];
extern u8 trampoline_end[];

/* Master PML4 (set during vmm_init) */
extern u64 master_pml4_phys(void);

static struct trampoline_data *tdata =
    (struct trampoline_data *)TRAMPOLINE_DATA_PHYS;

/* Per-AP kernel stacks (in .bss, high-half virtual address). */
static u8 ap_stacks[MAX_CPUS][KSTACK_SIZE] __attribute__((aligned(PAGE_SIZE)));

/* Spinlock for serializing AP startups. */
static spinlock_t smp_lock = SPINLOCK_INIT;

/* Build the AP GDT at physical 0x8800 and populate shared data. */
static void setup_ap_gdt(void)
{
    u64 *gdt = (u64 *)AP_GDT_PHYS;

    gdt[0] = 0x0000000000000000ULL; /* 0x00: null              */
    gdt[1] = 0x00CF9A000000FFFFULL; /* 0x08: 32-bit kern code  */
    gdt[2] = 0x00CF92000000FFFFULL; /* 0x10: kernel data        */
    gdt[3] = 0x00AF9A000000FFFFULL; /* 0x18: 64-bit kern code  */
    gdt[4] = 0x00CF92000000FFFFULL; /* 0x20: kernel data (LM)  */

    /* Pack for trampoline: bits[15:0] = limit, bits[63:16] = base >> 16
     * lgdt reads 6 bytes at [0x9020]: u16 limit + u32 base (little-endian) */
    tdata->gdt_packed = (u64)AP_GDT_LIMIT | ((u64)AP_GDT_PHYS << 16);
}

/* Copy trampoline to low physical memory (below 1 MiB) and set up shared data. */
static void copy_trampoline(void)
{
    u32 len = (u32)(trampoline_end - trampoline_start);
    memcpy((void *)TRAMPOLINE_CODE_PHYS, trampoline_start, len);

    /* Zero the shared data region */
    memset((void *)TRAMPOLINE_DATA_PHYS, 0, sizeof(struct trampoline_data));

    /* Fill shared data */
    tdata->page_dir = master_pml4_phys();

    /* Store physical address of ap_main (linked at high VMA) */
    extern void ap_main(u32);
    tdata->ap_main_phys = VIRT_TO_PHYS((u64)ap_main);

    /* Build AP GDT and fill gdt_packed */
    setup_ap_gdt();
}

/* Wait N milliseconds. */
static void mdelay(u32 ms)
{
    u64 start = rdtsc();
    /* rough estimate: ~2 GHz TSC => ~2M ticks/ms */
    u64 ticks = (u64)ms * 2000000ULL;
    while (rdtsc() - start < ticks)
        ;
}

/* Send INIT IPI, wait, then SIPI x2. */
static void ap_startup(u32 apic_id, u32 ap_id, u32 page_dir_phys)
{
    u64 flags;
    spin_lock_irqsave(&smp_lock, &flags);

    /* prepare per-AP shared data */
    tdata->page_dir = page_dir_phys;
    tdata->stack_top = (u64)&ap_stacks[ap_id][KSTACK_SIZE];
    tdata->ready = 0;
    tdata->ap_id = ap_id;

    /* INIT IPI */
    apic_send_init_ipi(apic_id);
    mdelay(10);

    /* SIPI (vector 0x08 => phys 0x8000) */
    apic_send_sipi(apic_id, 0x08);
    mdelay(1);

    /* second SIPI if needed */
    if (!tdata->ready) {
        apic_send_sipi(apic_id, 0x08);
        mdelay(1);
    }

    spin_unlock_irqrestore(&smp_lock, flags);
}

/* Called by each AP from the trampoline (arch/trampoline.S). */
void ap_main(u32 ap_id)
{
    struct cpu_info *c = &cpu_table[ap_id];
    c->id = ap_id;
    c->bsp = false;
    c->started = true;

    c->apic_id = apic_get_id();

    /* set GS base for this-cpu access */
    cpu_set_gs_base((u64)c);

    /* init per-cpu run queue */
    c->runq_head.pid = 0;
    c->runq_head.state = T_UNUSED;
    c->runq_head.rq_next = &c->runq_head;
    spin_init(&c->task_lock);

    /* enable local APIC on this core */
    apic_enable();
    apic_write(LAPIC_TPR, 0);

    /* mask PIC */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* mark ready (visible to BSP polling tdata->ready) */
    __sync_synchronize();
    tdata->ready = 1;

    kprintf("[smp] AP%u online (APIC ID=%u)\n", ap_id, c->apic_id);

    __sync_fetch_and_add(&cpu_count, 1);

    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

/* Called from start_kernel() after BSP is fully initialized. */
void smp_init(void)
{
    u64 pml4 = master_pml4_phys();

    /* copy trampoline code + set up shared data + AP GDT */
    copy_trampoline();

    kprintf("[smp] BSP online, APIC ID=%u\n", apic_get_id());

    u32 next_id = 1;

    for (u32 ap = 0; ap < MAX_CPUS - 1; ap++) {
        u32 apic_id = ap + 1;

        cpu_table[next_id].id = next_id;
        cpu_table[next_id].apic_id = apic_id;
        cpu_table[next_id].bsp = false;

        kprintf("[smp] starting AP%u (APIC ID=%u)...\n", next_id, apic_id);
        ap_startup(apic_id, next_id, (u32)pml4);

        u32 timeout = 10000;
        while (!tdata->ready && timeout--)
            mdelay(1);

        if (tdata->ready) {
            next_id++;
        } else {
            kprintf("[smp] AP%u (APIC ID=%u) did not respond\n",
                    next_id, apic_id);
            break;
        }
    }

    kprintf("[smp] %u CPUs online\n", cpu_count);
}
