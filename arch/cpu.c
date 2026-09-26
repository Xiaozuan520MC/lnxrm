/* CPU bring-up: GDT/TSS, IDT, APIC, PIT timer, MSR helpers. */
#include <sys/cpu.h>
#include <sys/apic.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <io.h>
#include <sys/percpu.h>

/* ---- selectors (keep in sync with setup.asm / entry64.S) ----
 * 0x08 kernel code, 0x10 kernel data, 0x18 user code, 0x20 user data,
 * 0x28 TSS */
#define NGDT 8

struct tss_entry {
    u32 rsv0;
    u64 rsp0;
    u64 rsp1, rsp2;
    u64 rsv1;
    u64 ist[7];
    u64 rsv2;
    u16 iopb;
    u16 rsv3;
} __attribute__((packed));

/* One GDT + TSS + GDTR per CPU. Each CPU must load its own TSS so
 * tss_set_rsp0() only affects the calling CPU — a shared global TSS
 * made every AP's ring0->ring3 return land on the BSP's kstack. */
struct cpu_gdt {
    u64 gdt[NGDT];
    struct tss_entry tss;
    struct gdtr gdtr;
};

static struct cpu_gdt cpu_gdt_tab[MAX_CPUS];

static void gdt_fill_one(struct cpu_gdt *cg)
{
    u64 *e = cg->gdt;
    e[0] = 0;
    e[1] = 0x00AF9A000000FFFF; /* 0x08 KCODE L=1 */
    e[2] = 0x00CF92000000FFFF; /* 0x10 KDATA */
    e[3] = 0x00AFFA000000FFFF; /* 0x18 UCODE L=1 DPL=3 */
    e[4] = 0x00CFF2000000FFFF; /* 0x20 UDATA DPL=3 */

    u64 b = (uptr)&cg->tss;
    e[5] = ((sizeof(cg->tss) - 1) & 0xFFFFULL) | ((b & 0xFFFFULL) << 16) |
           (((b >> 16) & 0xFFULL) << 32) | (0x89ULL << 40) | (((b >> 24) & 0xFFULL) << 56);
    e[6] = b >> 32;

    memset(&cg->tss, 0, sizeof(cg->tss));
    cg->tss.iopb = sizeof(cg->tss);
    cg->gdtr.limit = sizeof(cg->gdt) - 1;
    cg->gdtr.base = (uptr)cg->gdt;
}

void gdt_init_cpu(int cpu)
{
    if (cpu < 0 || cpu >= MAX_CPUS) cpu = 0;
    struct cpu_gdt *cg = &cpu_gdt_tab[cpu];
    gdt_fill_one(cg);
    gdt_reload(&cg->gdtr);
    tss_load(0x28);
}

void gdt_init(void)
{ gdt_init_cpu(0); }

void tss_set_rsp0(u64 rsp)
{
    struct cpu_info *ci = this_cpu_data();
    if (!ci)
        cpu_gdt_tab[0].tss.rsp0 = rsp;
    else
        cpu_gdt_tab[ci->id].tss.rsp0 = rsp;
}

/* ---- IDT ---- */
struct idt_gate {
    u16 off_lo;
    u16 sel;
    u8 ist;
    u8 type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed));

static struct idt_gate idt[256];
static struct gdtr idtr;

extern u64 isr_stub_table[];
/* IPI stubs in entry64.S (push the real vector, not the isr19 alias). */
extern void isr240(void); /* 0xF0 reschedule */
extern void isr241(void); /* 0xF1 */
extern void isr242(void); /* 0xF2 stop */

static void set_gate(int v, u64 handler, u8 dpl)
{
    idt[v].off_lo = handler & 0xffff;
    idt[v].sel = 0x08;
    idt[v].ist = 0;
    idt[v].type = 0x8E | (dpl << 5);
    idt[v].off_mid = (handler >> 16) & 0xffff;
    idt[v].off_hi = handler >> 32;
    idt[v].zero = 0;
}

void idt_init(void)
{
    for (int v = 0; v <= 48; v++) set_gate(v, isr_stub_table[v], v == 48 ? 3 : 0);
    for (int v = 49; v < 256; v++) set_gate(v, isr_stub_table[19], 0); /* spurious -> catch-all */
    set_gate(128, isr_stub_table[48], 3);                              /* int 0x80 from ring 3 */
    /* Real IPI gates (must override the isr19 catch-all above). */
    set_gate(0xF0, (u64)isr240, 0);
    set_gate(0xF1, (u64)isr241, 0);
    set_gate(0xF2, (u64)isr242, 0);
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uptr)idt;
    __asm__ volatile("lidt %0" ::"m"(idtr));
}

/* ---- IRQ handler table (dispatched by kernel/isr.c) ---- */
static irq_handler_t handlers[16];

void irq_install(int irq, irq_handler_t h)
{ handlers[irq] = h; }

irq_handler_t irq_get_handler(int irq)
{
    if (irq >= 0 && irq < 16) return handlers[irq];
    return NULL;
}

/* ---- IPI handlers (vectors 0xF0..0xF2) ---- */
static ipi_handler_t ipi_handlers[3];

void ipi_handler_install(u32 vector, ipi_handler_t h)
{
    if (vector >= 0xF0 && vector <= 0xF2) ipi_handlers[vector - 0xF0] = h;
}

void ipi_dispatch(u32 vector)
{
    if (vector >= 0xF0 && vector <= 0xF2 && ipi_handlers[vector - 0xF0])
        ipi_handlers[vector - 0xF0]();
}

/* ---- PIT (programmed for calibration; IRQ0 does not reach the LAPIC) ---- */
#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_HZ  1193182

volatile u64 jiffies;

static void pit_handler(struct intr_frame *f)
{
    jiffies++;
    sched_tick();
    sched_maybe_preempt(f);
}

static u32 pit_count(void)
{
    outb(0x43, 0x00); /* latch channel 0 */
    u8 l = inb(PIT_CH0);
    u8 h = inb(PIT_CH0);
    return ((u32)h << 8) | l;
}

void pit_init(u32 hz)
{
    u32 div = PIT_HZ / hz;
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, div & 0xff);
    outb(PIT_CH0, (div >> 8) & 0xff);
    /* vector 32 -> pit_handler via isr_common's irq path.
     * PIT->IOAPIC IRQ0 never sets LAPIC IRR on this platform, so the
     * periodic tick is driven by the BSP LAPIC timer instead. */
    irq_install(0, pit_handler);
}

/* Calibrate the LAPIC timer against one full PIT period (1/hz seconds)
 * and start it in periodic mode on vector `vector`. */
void lapic_timer_start(u32 vector, u32 hz)
{
    u32 div = PIT_HZ / hz;

    apic_write(LAPIC_LVT_TIMER, 0x10000); /* masked while calibrating */
    apic_write(LAPIC_TIMER_DCR, 0x3);     /* divide by 16 */
    apic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Align to a PIT reload edge (count: div -> 1 -> reload). */
    while (pit_count() > 1000);
    while (pit_count() < div - 1000);
    u32 lap0 = apic_read(LAPIC_TIMER_CCR);

    while (pit_count() > 1000);
    while (pit_count() < div - 1000);
    u32 lap1 = apic_read(LAPIC_TIMER_CCR);

    u32 period = lap0 - lap1;
    if (period < 1000) period = 100000; /* sanity fallback */

    apic_write(LAPIC_LVT_TIMER, vector | LVT_TIMER_PERIODIC);
    apic_write(LAPIC_TIMER_ICR, period);
    kprintf("[lapic] timer vec=%u hz=%u period=%u\n", vector, hz, period);
}

/* ---- msr ---- */
u64 rdmsr(u32 msr)
{
    u32 a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(msr));
    return ((u64)d << 32) | a;
}

void wrmsr(u32 msr, u64 v)
{ __asm__ volatile("wrmsr" ::"a"((u32)v), "d"((u32)(v >> 32)), "c"(msr)); }

u64 rdtsc(void)
{
    u32 a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((u64)d << 32) | a;
}

void cpu_init(void)
{
    gdt_init();
    /* Re-set GS base after GDT reload — gdt_reload() does mov gs,ax
     * which in QEMU's TCG resets the hidden base from the GDT entry,
     * clobbering the MSR we set in cpu_init_percpu(). */
    u64 gs_val = (u64)&cpu_table[0];
    cpu_set_gs_base(gs_val);
    kprintf("[cpu] GS base set to %p, rdmsr reads %p\n", gs_val, rdmsr(0xC0000101));
    idt_init();
    /* init LAPIC + IOAPIC (replaces PIC) */
    apic_init();
    ioapic_init();
    /* enable SSE so any FP codegen does not #UD */
    u64 cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 &= ~((1UL << 2) | (1UL << 3)); /* EM=0 TS=0 */
    cr0 |= (1UL << 1) | (1UL << 5);    /* MP=1 NE=1 */
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0));
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10); /* OSFXSR OSXMMEXCPT */
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4));
}
