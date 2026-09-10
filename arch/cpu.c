/* CPU bring-up: GDT/TSS, IDT, APIC, PIT timer, MSR helpers. */
#include <cpu.h>
#include <apic.h>
#include <console.h>
#include <mm.h>
#include <sched.h>
#include <io.h>
#include <percpu.h>

/* ---- selectors (keep in sync with setup.asm / entry64.S) ----
 * 0x08 kernel code, 0x10 kernel data, 0x18 user code, 0x20 user data,
 * 0x28 TSS */
struct gdtr {
    u16 limit;
    u64 base;
} __attribute__((packed));

static u64 gdt[8];
static struct gdtr gdtr;

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

static struct tss_entry tss;

extern void gdt_reload(const struct gdtr *);
extern void tss_load(u64 sel);

void gdt_init(void)
{
    u64 *e = gdt;
    e[0] = 0;
    e[1] = 0x00AF9A000000FFFF;      /* 0x08 KCODE L=1 */
    e[2] = 0x00CF92000000FFFF;      /* 0x10 KDATA */
    e[3] = 0x00AFFA000000FFFF;      /* 0x18 UCODE L=1 DPL=3 */
    e[4] = 0x00CFF2000000FFFF;      /* 0x20 UDATA DPL=3 */

    u64 b = (uptr)&tss;
    e[5] = ((sizeof(tss) - 1) & 0xFFFFULL)
         | ((b & 0xFFFFULL) << 16)
         | (((b >> 16) & 0xFFULL) << 32)
         | (0x89ULL << 40)
         | (((b >> 24) & 0xFFULL) << 56);
    e[6] = b >> 32;

    memset(&tss, 0, sizeof(tss));
    tss.iopb = sizeof(tss);
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (uptr)gdt;
    gdt_reload(&gdtr);
    tss_load(0x28);
}

void tss_set_rsp0(u64 rsp)
{
    tss.rsp0 = rsp;
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
    for (int v = 0; v <= 48; v++)
        set_gate(v, isr_stub_table[v], v == 48 ? 3 : 0);
    for (int v = 49; v < 256; v++)
        set_gate(v, isr_stub_table[19], 0); /* spurious -> catch-all */
    set_gate(128, isr_stub_table[48], 3);   /* int 0x80 from ring 3 */
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uptr)idt;
    __asm__ volatile("lidt %0" ::"m"(idtr));
}

/* ---- APIC IRQ routing (replaces 8259 PIC) ---- */
static irq_handler_t handlers[16];

void irq_install(int irq, irq_handler_t h)
{
    handlers[irq] = h;
}

irq_handler_t irq_get_handler(int irq)
{
    if (irq >= 0 && irq < 16)
        return handlers[irq];
    return NULL;
}

void apic_irq_install(int irq, irq_handler_t h)
{
    handlers[irq] = h;
    ioapic_set_irq(irq, irq + 32, 0);
    ioapic_unmask_irq(irq);
}

void apic_irq_unmask(u32 irq)
{
    ioapic_unmask_irq(irq);
}

void apic_irq_mask(u32 irq)
{
    ioapic_mask_irq(irq);
}

void apic_irq_eoi(void)
{
    apic_eoi();
}

/* ---- PIT ---- */
#define PIT_CH0 0x40
#define PIT_CMD 0x43
#define PIT_HZ 1193182

volatile u64 jiffies;

static void pit_handler(struct intr_frame *f)
{
    jiffies++;
    sched_tick();
    sched_maybe_preempt(f);
}

void pit_init(u32 hz)
{
    u32 div = PIT_HZ / hz;
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, div & 0xff);
    outb(PIT_CH0, (div >> 8) & 0xff);
    /* route PIT through IOAPIC vector 32 */
    ioapic_set_irq(0, 32, 0);
    ioapic_unmask_irq(0);
}

/* ---- msr ---- */
u64 rdmsr(u32 msr)
{
    u32 a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(msr));
    return ((u64)d << 32) | a;
}

void wrmsr(u32 msr, u64 v)
{
    __asm__ volatile("wrmsr" :: "a"((u32)v), "d"((u32)(v >> 32)), "c"(msr));
}

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
    cr0 &= ~(1UL << 2);             /* EM=0 */
    cr0 |= (1UL << 1) | (1UL << 5); /* MP=1 NE=1 */
    __asm__ volatile("mov %0,%%cr0" ::"r"(cr0));
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);/* OSFXSR OSXMMEXCPT */
    __asm__ volatile("mov %0,%%cr4" ::"r"(cr4));
}
