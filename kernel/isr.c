/* Interrupt dispatch: exception reporting, IRQ routing, input queue. */
#include <cpu.h>
#include <io.h>
#include <console.h>
#include <sched.h>

static irq_handler_t handlers[16];

void irq_install(int irq, irq_handler_t h)
{
    handlers[irq] = h;
    irq_unmask(irq);
}

/* ---- keyboard: scancode set 1 -> ASCII ---- */
static const char kmap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, '*', 0, ' ', 0,
};

#define KBD_DATA 0x60
#define KBD_STAT 0x64

void kbd_irq_handler(struct intr_frame *f)
{
    u8 sc = inb(KBD_DATA);
    static bool ext;
    if (sc == 0xE0) {
        ext = true;
        return;
    }
    bool release = !!(sc & 0x80);
    sc &= 0x7f;
    if (release || ext) {
        ext = false;
        return;
    }
    if (sc < 128 && kmap[sc])
        input_push(kmap[sc]);
}

/* ---- serial COM1 receive ---- */
extern int lnxrm_uart_trygetc(void);
void serial_rx_handler(struct intr_frame *f)
{
    int c;
    while ((c = lnxrm_uart_trygetc()) >= 0)
        input_push((char)c);
}

/* ---- ring buffer for console input ---- */
#define INBUF_SZ 256
static volatile char inbuf[INBUF_SZ];
static volatile u32 in_r, in_w;

void input_push(char c)
{
    if ((in_w + 1) % INBUF_SZ == in_r)
        return;                     /* full, drop */
    inbuf[in_w] = c;
    __atomic_store_n(&in_w, (in_w + 1) % INBUF_SZ, __ATOMIC_SEQ_CST);
}

int input_pop(void)
{
    u32 r = __atomic_load_n(&in_r, __ATOMIC_SEQ_CST);
    if (r == in_w)
        return -1;
    int c = inbuf[r];
    __atomic_store_n(&in_r, (r + 1) % INBUF_SZ, __ATOMIC_SEQ_CST);
    return c;
}

/* ---- central dispatch ---- */
static const char *exc_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", "?9", "#TS", "#NP", "#SS", "#GP", "#PF", "?15",
    "#MF", "#AC", "#MC", "#XM", "#VE", "?21", "?22", "?23",
    "?24", "?25", "?26", "?27", "?28", "?29", "?30", "?31",
};

void isr_common(struct intr_frame *f)
{
    if (f->intno < 32) {
        u64 cr2 = 0;
        if (f->intno == 14)
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("\n[exception] %s (%d) err=%#lx rip=%#lx rsp=%#lx cr2=%#lx\n",
                exc_names[f->intno & 31], f->intno, f->err, f->rip, f->ussp,
                cr2);
        if (current) {
            kprintf("[exception] task pid=%u name=%s cs=%#lx ss=%#lx\n",
                    current->pid, current->name, f->cs, f->usss);
        }
        {
            u64 *sp = (u64 *)f;
            kprintf("[stack]");
            for (int q = 22; q < 30; q++)
                kprintf(" %lx", sp[q]);
            kprintf("\n");
        }
        panic("unrecoverable exception");
    }

    int irq = f->intno - 32;
    extern void pic_send_eoi(int);
    if (current && current->pid != 0)
        current->tf = f;        /* keep the live frame pointer fresh */
    if (irq >= 0 && irq < 16) {
        /* spurious IRQ7/15 check */
        if (irq == 7 || irq == 15) {
            u16 port = irq == 7 ? 0x20 : 0xA0;
            u8 isr_reg;
            outb(port, 0x0B);
            isr_reg = inb(port);
            if (!(isr_reg & (1 << (irq & 7)))) {
                pic_send_eoi(irq);      /* EOI only */
                return;
            }
        }
        if (handlers[irq])
            handlers[irq](f);
        else
            irq_mask(irq);
        pic_send_eoi(irq);

        __asm__ volatile("outb %0,$0xE9" :: "a"((char)'T'));
#if 0
        /* preemptive path (disabled for now -- cooperative scheduling) */
        extern void sched_maybe_preempt(struct intr_frame *f);
        sched_maybe_preempt(f);
#endif
    } else {
        kprintf("stray vector %d\n", f->intno);
    }
}
