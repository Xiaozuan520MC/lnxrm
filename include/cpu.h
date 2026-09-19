/* CPU bring-up: GDT/TSS, IDT, APIC, PIT timer, MSR helpers. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GDT register structure (matches x86 LGDT format) */
struct gdtr {
    u16 limit;
    u64 base;
} __attribute__((packed));

void cpu_init(void);
void gdt_init(void);
void idt_init(void);
void pit_init(u32 hz);
u64  rdmsr(u32 msr);
void wrmsr(u32 msr, u64 v);
u64  rdtsc(void);

/* GDT/TSS reload (entry64.S) */
void gdt_reload(const struct gdtr *gdtr);
void tss_load(u16 sel);
void tss_set_rsp0(u64 rsp);

/* Get pointer to BSP's GDTR (for AP reload). */
const struct gdtr *get_bsp_gdtr(void);

/* Forward-declare interrupt frame (defined in sched.h). */
struct intr_frame;

/* irq registration (kernel/isr.c) */
typedef void (*irq_handler_t)(struct intr_frame *);
void isr_common(struct intr_frame *f);
void irq_install(int irq, irq_handler_t h);
irq_handler_t irq_get_handler(int irq);

/* APIC IRQ routing (replaces 8259 PIC) */
void apic_irq_unmask(u32 irq);
void apic_irq_mask(u32 irq);
void apic_irq_eoi(void);
void apic_irq_install(int irq, irq_handler_t h);

/* IPI handler registration (for vectors 0xF0..0xF2) */
typedef void (*ipi_handler_t)(void);
void ipi_handler_install(u32 vector, ipi_handler_t h);
void ipi_dispatch(u32 vector);

/* input queue (kernel/isr.c feeds it; serial too) */
void kbd_irq_handler(struct intr_frame *f);
void kbd_poll(void);                /* drain PS/2 output buffer directly */
void serial_rx_handler(struct intr_frame *f);
void input_push(char c);
int  input_pop(void);               /* -1 if empty */

extern volatile u64 jiffies;
#define HZ 100


#ifdef __cplusplus
}
#endif
