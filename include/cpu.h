#pragma once
#include <types.h>
#include <sched.h>


#ifdef __cplusplus
extern "C" {
#endif

void cpu_init(void);
void gdt_init(void);
void idt_init(void);
void pit_init(u32 hz);
u64  rdmsr(u32 msr);
void wrmsr(u32 msr, u64 v);
u64  rdtsc(void);

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

/* input queue (drivers/ps2kbd.c feeds it; serial too) */
void kbd_irq_handler(struct intr_frame *f);
void serial_rx_handler(struct intr_frame *f);
void input_push(char c);
int  input_pop(void);               /* -1 if empty */

extern volatile u64 jiffies;
#define HZ 100


#ifdef __cplusplus
}
#endif
