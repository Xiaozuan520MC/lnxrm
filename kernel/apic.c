/* Local APIC + IOAPIC driver for SMP.
 * Both are MMIO: map their physical pages into the DEV_VMA window. */
#include <apic.h>
#include <cpu.h>
#include <io.h>
#include <mm.h>
#include <console.h>

/* LAPIC mapped at DEV_VMA + 0, IOAPIC at DEV_VMA + 0x1000.
 * Each is a single 4 KiB MMIO page. */
#define LAPIC_VA   DEV_VMA           /* 0xFFFFFFFF90000000 */
#define IOAPIC_VA  (DEV_VMA + 0x1000) /* 0xFFFFFFFF90001000 */

volatile u32 *lapic_base;

/* ---- helpers ---- */

void apic_write(u32 reg, u32 val)
{
    lapic_base[reg / 4] = val;
}

u32 apic_read(u32 reg)
{
    return lapic_base[reg / 4];
}

void apic_eoi(void)
{
    lapic_base[LAPIC_EOI / 4] = 0;
}

void apic_enable(void)
{
    u64 base = rdmsr(MSR_IA32_APIC_BASE);
    base |= (1UL << 11);           /* APIC Global Enable */
    wrmsr(MSR_IA32_APIC_BASE, base);
    apic_write(LAPIC_SPURIOUS, 0x100 | 0xFF);  /* enable + spurious vector 0xFF */
}

u32 apic_get_id(void)
{
    return apic_read(LAPIC_ID) >> 24;
}

void apic_set_ldr(u32 id)
{
    apic_write(LAPIC_LDR, id << 24);
}

void apic_init(void)
{
    /* read MMIO base from MSR */
    u64 base = rdmsr(MSR_IA32_APIC_BASE);
    u64 phys = base & 0xFFFFFF000UL;

    /* map the LAPIC page into DEV_VMA */
    vmm_map_kernel_page(LAPIC_VA, phys, PG_PCD);
    lapic_base = (volatile u32 *)LAPIC_VA;

    /* disable PIC interrupts (mask all) */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* enable LAPIC */
    apic_enable();

    /* set task priority to 0 (accept all interrupts) */
    apic_write(LAPIC_TPR, 0);

    /* LVT: mask all except LINT0/LINT1 for now */
    apic_write(LAPIC_LVT_TIMER, 0x10000);     /* masked */
    apic_write(LAPIC_LVT_THERMAL, 0x10000);
    apic_write(LAPIC_LVT_PMC, 0x10000);
    apic_write(LAPIC_LVT_LINT0, 0x10000);
    apic_write(LAPIC_LVT_LINT1, 0x10000);
    apic_write(LAPIC_LVT_ERROR, 0x10000);

    /* spurious vector */
    apic_write(LAPIC_SPURIOUS, 0x100 | 0xFF);

    kprintf("[apic] MMIO base=%p ID=%u\n", (void *)phys, apic_get_id());
}

void apic_set_timer(u32 vector, u32 divide, u32 count)
{
    apic_write(LAPIC_TIMER_DCR, divide & 0x0F);
    apic_write(LAPIC_TIMER_ICR, count);
    apic_write(LAPIC_LVT_TIMER, vector);       /* periodic mode (bits 17-18=01) */
}

void apic_mask_lvt(u32 vector, bool mask)
{
    u32 val = apic_read(vector);
    if (mask)
        val |= (1 << 16);
    else
        val &= ~(1 << 16);
    apic_write(vector, val);
}

/* ---- IPI ---- */
void apic_send_ipi(u32 dest_apic_id, u32 vector, u32 delivery, u32 level)
{
    /* wait for send queue to be ready */
    while (apic_read(LAPIC_ICR_LOW) & (1 << 12))
        ;
    apic_write(LAPIC_ICR_HIGH, dest_apic_id << 24);
    apic_write(LAPIC_ICR_LOW, vector | (delivery << 8) | (level << 14));
}

void apic_send_init_ipi(u32 dest_apic_id)
{
    apic_send_ipi(dest_apic_id, 0, 5, 1);     /* INIT, level assert */
}

void apic_send_sipi(u32 dest_apic_id, u32 vector)
{
    apic_send_ipi(dest_apic_id, vector, 6, 1); /* SIPI */
}

void apic_send_init_deassert(void)
{
    apic_send_ipi(0, 0, 5, 0);                /* INIT, level deassert, broadcast */
}

/* ---- IOAPIC ---- */
static volatile u32 *ioapic_base;

static u32 ioapic_read(u32 reg)
{
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

static void ioapic_write(u32 reg, u32 val)
{
    ioapic_base[0] = reg;
    ioapic_base[4] = val;
}

void ioapic_init(void)
{
    /* map the IOAPIC page into DEV_VMA */
    vmm_map_kernel_page(IOAPIC_VA, IOAPIC_BASE, PG_PCD);
    ioapic_base = (volatile u32 *)IOAPIC_VA;

    u32 ver = ioapic_read(IOAPIC_VER);
    u32 max = (ver >> 16) & 0xFF;
    if (max == 0)
        max = 23;
    kprintf("[ioapic] version=%u max_irq=%u\n", ver & 0xFF, max);

    /* mask all IRQ lines */
    for (u32 i = 0; i <= max; i++)
        ioapic_mask_irq(i);
}

void ioapic_set_irq(u32 irq, u32 vector, u32 dest_apic_id)
{
    u32 base = IOAPIC_REDIR_BASE + irq * 2;
    ioapic_write(base, vector);                         /* low: vector */
    ioapic_write(base + 1, (u32)dest_apic_id << 24);   /* high: dest APIC ID */
}

void ioapic_mask_irq(u32 irq)
{
    u32 base = IOAPIC_REDIR_BASE + irq * 2;
    ioapic_write(base, ioapic_read(base) | (1 << 16));
}

void ioapic_unmask_irq(u32 irq)
{
    u32 base = IOAPIC_REDIR_BASE + irq * 2;
    ioapic_write(base, ioapic_read(base) & ~(1 << 16));
}
