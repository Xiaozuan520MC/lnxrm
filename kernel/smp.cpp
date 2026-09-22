/* SMP initialization: AP startup via INIT-SIPI-SIPI, per-CPU setup.
 * C++ rewrite with robust AP startup, per-CPU IDT/GDT, cross-core IPI. */
#include "smp.hpp"
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

/* ---- Per-AP kernel stacks ---- */
static u8 ap_stacks[MAX_CPUS][KSTACK_SIZE] __attribute__((aligned(PAGE_SIZE)));

/* ---- Spinlock for serializing AP startups ---- */
static spinlock_t smp_lock = SPINLOCK_INIT;

/* ---- SMP state ---- */
static u32 s_online_count = 1;  /* BSP is always online */
static u32 s_next_cpu_id = 1;

/* ---- Rust FFI: trampoline data management ---- */
extern "C" {
    void trampoline_setup(u64 pml4, u64 stack_top, u32 ap_id,
                          u64 gdt_packed, u64 ap_main_phys);
    u32  trampoline_is_ready(void);
    void trampoline_signal_ready(void);
    void trampoline_clear(void);
}

/* ---- Helpers ---- */

static void mdelay(u32 ms)
{
    u64 start = rdtsc();
    u64 ticks = (u64)ms * 2000000ULL;  /* rough: ~2 GHz TSC */
    while (rdtsc() - start < ticks)
        ;
}

/* Build the AP GDT at physical 0x8800 (for trampoline transition only). */
static void setup_ap_gdt(void)
{
    u64 *gdt = (u64 *)AP_GDT_PHYS;
    gdt[0] = 0x0000000000000000ULL; /* null */
    gdt[1] = 0x00CF9A000000FFFFULL; /* 32-bit kernel code */
    gdt[2] = 0x00CF92000000FFFFULL; /* kernel data */
    gdt[3] = 0x00AF9A000000FFFFULL; /* 64-bit kernel code */
    gdt[4] = 0x00CF92000000FFFFULL; /* kernel data (LM) */
}

/* Copy trampoline code to low memory and set up shared data via Rust. */
static void copy_trampoline(void)
{
    u32 len = (u32)(trampoline_end - trampoline_start);
    memcpy((void *)TRAMPOLINE_CODE_PHYS, trampoline_start, len);
    trampoline_clear();
    setup_ap_gdt();
}

/* Send INIT-SIPI-SIPI to a specific AP and wait for readiness.
 * Returns true if AP became ready within timeout. */
static bool ap_startup(u32 apic_id, u32 cpu_id, u64 pml4_phys)
{
    u64 flags;
    spin_lock_irqsave(&smp_lock, &flags);

    /* Prepare shared data via Rust FFI */
    u64 stack_top = (u64)&ap_stacks[cpu_id][KSTACK_SIZE];
    u64 gdt_packed = (u64)AP_GDT_LIMIT | ((u64)AP_GDT_PHYS << 16);
    /* Pass the VIRTUAL address of ap_main — the function's code references
     * globals via high-half RIP-relative addressing, so calling via physical
     * address would break all data accesses. */
    u64 ap_main_va = (u64)ap_main;

    trampoline_setup(pml4_phys, stack_top, cpu_id, gdt_packed, ap_main_va);

    /* INIT IPI (level assert) */
    apic_send_init_ipi(apic_id);
    mdelay(10);

    /* SIPI: vector 0x08 => physical 0x8000 (trampoline start) */
    apic_send_sipi(apic_id, 0x08);
    mdelay(2);

    /* Second SIPI if AP hasn't responded yet */
    if (!trampoline_is_ready()) {
        apic_send_sipi(apic_id, 0x08);
        mdelay(2);
    }

    /* Third SIPI attempt (some hardware needs it) */
    if (!trampoline_is_ready()) {
        apic_send_sipi(apic_id, 0x08);
        mdelay(2);
    }

    spin_unlock_irqrestore(&smp_lock, flags);

    /* Wait for AP to signal ready (polling with timeout) */
    for (u32 i = 0; i < AP_STARTUP_TIMEOUT_MS; i++) {
        if (trampoline_is_ready())
            return true;
        mdelay(1);
    }

    return trampoline_is_ready() != 0;
}

/* ---- Reschedule IPI handler ---- */
static void reschedule_ipi_handler(void)
{
    /* Nothing to do — the IPI's purpose is just to wake the CPU from HLT.
     * When the interrupt returns, sched_maybe_preempt() will check the
     * per-CPU need_resched flag. */
}

/* ---- Stop IPI handler ---- */
static void stop_ipi_handler(void)
{
    /* Halt this CPU indefinitely. */
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

/* ---- SMP Manager ---- */

void smp::Manager::init(void)
{
    u64 pml4 = master_pml4_phys();
    copy_trampoline();

    /* Register IPI handlers */
    ipi_handler_install(IPI_VECTOR_RESCHEDULE, reschedule_ipi_handler);
    ipi_handler_install(IPI_VECTOR_STOP, stop_ipi_handler);

    kprintf("[smp] BSP online, APIC ID=%u\n", apic_get_id());

    s_next_cpu_id = 1;
    s_online_count = 1;

    /* Try to start each AP by APIC ID.
     * Stop after 2 consecutive failures — in QEMU (and most hardware),
     * APIC IDs are assigned sequentially. If ID N has no CPU, N+1 won't
     * either. */
    u32 consecutive_fails = 0;
    for (u32 try_id = 1; try_id < MAX_CPUS && consecutive_fails < 2; try_id++) {
        u32 apic_id = try_id;

        cpu_table[s_next_cpu_id].id = s_next_cpu_id;
        cpu_table[s_next_cpu_id].apic_id = apic_id;
        cpu_table[s_next_cpu_id].bsp = false;
        cpu_table[s_next_cpu_id].need_resched = false;

        if (ap_startup(apic_id, s_next_cpu_id, pml4)) {
            s_next_cpu_id++;
            s_online_count++;
            consecutive_fails = 0;
        } else {
            consecutive_fails++;
        }
    }

    kprintf("[smp] %u CPUs online\n", s_online_count);
}

u32 smp::Manager::online_count()
{
    return s_online_count;
}

void smp::Manager::ipi_reschedule(u32 cpu_id)
{
    if (cpu_id >= MAX_CPUS || !cpu_table[cpu_id].started)
        return;
    apic_send_ipi(cpu_table[cpu_id].apic_id, IPI_VECTOR_RESCHEDULE, 0, 1);
}

void smp::Manager::ipi_reschedule_all()
{
    for (u32 i = 0; i < MAX_CPUS; i++) {
        if (cpu_table[i].started && !cpu_table[i].bsp)
            ipi_reschedule(i);
    }
}

void smp::Manager::ipi_stop(u32 cpu_id)
{
    if (cpu_id >= MAX_CPUS || !cpu_table[cpu_id].started)
        return;
    apic_send_ipi(cpu_table[cpu_id].apic_id, IPI_VECTOR_STOP, 0, 1);
}

void smp::Manager::ipi_stop_all()
{
    for (u32 i = 0; i < MAX_CPUS; i++) {
        if (cpu_table[i].started && !cpu_table[i].bsp)
            ipi_stop(i);
    }
}

void smp::Manager::ipi_send(u32 cpu_id, u32 vector)
{
    if (cpu_id >= MAX_CPUS || !cpu_table[cpu_id].started)
        return;
    apic_send_ipi(cpu_table[cpu_id].apic_id, vector, 0, 1);
}

/* ---- C linkage entry points ---- */

extern "C" void smp_init(void)
{
    smp::Manager::init();
}

extern "C" void ap_main(u32 ap_id)
{
    struct cpu_info *c = &cpu_table[ap_id];
    c->id = ap_id;
    c->bsp = false;
    c->started = true;
    c->apic_id = apic_get_id();

    cpu_set_gs_base((u64)c);

    gdt_reload(get_bsp_gdtr());

    idt_init();

    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 &= ~(1UL << 2);
    cr0 |= (1UL << 1) | (1UL << 5);
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0));
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4));

    c->runq_head.pid = 0;
    c->runq_head.state = T_UNUSED;
    c->runq_head.rq_next = &c->runq_head;
    c->need_resched = false;
    spin_init(&c->task_lock);

    apic_enable();
    apic_write(LAPIC_TPR, 0);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    trampoline_signal_ready();

    kprintf("[smp] AP%u online (APIC ID=%u)\n", ap_id, c->apic_id);

    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}
