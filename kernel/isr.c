/* Interrupt dispatch: exception reporting, IRQ routing, IPI dispatch. */
#include <sys/cpu.h>
#include <sys/apic.h>
#include <io.h>
#include <console.h>
#include <sys/sched.h>
#include <sys/percpu.h>

/* ---- keyboard: scancode set 1 -> ASCII ---- */
/* clang-format off */   /* raw lookup table */
static const char kmap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0, '*', 0, ' ', 0,
};

static const char kmap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0, '*', 0, ' ', 0,
};
/* clang-format on */

#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPSLOCK 0x3A

static bool shift_pressed = false;
static bool capslock_on = false;

#define KBD_DATA 0x60
#define KBD_STAT 0x64

static bool kbd_ext; /* file-scope so kbd_poll can share it */

static void kbd_process_sc(u8 sc)
{
    if (sc == 0xE0) {
        kbd_ext = true;
        return;
    }
    bool release = !!(sc & 0x80);
    bool ext = kbd_ext;
    u8 code = sc & 0x7f;
    kbd_ext = false; /* the E0 prefix belongs to this code only */

    if (code == SC_LSHIFT || code == SC_RSHIFT) {
        shift_pressed = !release;
        return;
    }
    if (code == SC_CAPSLOCK) {
        if (!release) capslock_on = !capslock_on;
        return;
    }
    if (release) return;

    char c = 0;
    if (!ext && code < 128 && kmap[code]) {
        if (shift_pressed)
            c = kmap_shift[code];
        else
            c = kmap[code];
        if (capslock_on && c >= 'a' && c <= 'z')
            c = c - 'a' + 'A';
        else if (capslock_on && c >= 'A' && c <= 'Z')
            c = c - 'A' + 'a';
        input_push(c);
    }
    /* Every press also lands in the raw key queue consumed by SYS_getkey,
     * so key events without an ASCII mapping (arrows, F-keys, ...) stay
     * visible to user space as scancode + extended flag. */
    keyq_push(((u32)ext << 16) | ((u32)code << 8) | (u8)c);
}

void kbd_irq_handler(struct intr_frame *f)
{
    u8 sc = inb(KBD_DATA);
    kbd_process_sc(sc);
}

/* ---- serial COM1 receive ---- */
extern int lnxrm_uart_trygetc(void);
void serial_rx_handler(struct intr_frame *f)
{
    int c;
    while ((c = lnxrm_uart_trygetc()) >= 0) input_push((char)c);
}

/* ---- ring buffer for console input ---- */
#define INBUF_SZ 256
static volatile char inbuf[INBUF_SZ];
static volatile u32 in_r, in_w;

/* Single console reader blocked in console_read(); input_push() wakes
 * it (state T_SLEEPING -> T_RUNNABLE + runqueue_add) after enqueueing. */
static struct task *volatile con_waiter;

void console_waiter_arm(struct task *t)
{ con_waiter = t; }

void console_waiter_disarm(struct task *t)
{
    if (con_waiter == t) con_waiter = NULL;
}

void input_push(char c)
{
    if ((in_w + 1) % INBUF_SZ == in_r) return;
    inbuf[in_w] = c;
    __atomic_store_n(&in_w, (in_w + 1) % INBUF_SZ, __ATOMIC_SEQ_CST);

    struct task *w = con_waiter;
    if (w && w->state == T_SLEEPING) {
        con_waiter = NULL;
        w->state = T_RUNNABLE;
        runqueue_add(w);
    }
}

int input_pop(void)
{
    u32 r = __atomic_load_n(&in_r, __ATOMIC_SEQ_CST);
    if (r == in_w) return LNXRM_EFAIL;
    int c = inbuf[r];
    __atomic_store_n(&in_r, (r + 1) % INBUF_SZ, __ATOMIC_SEQ_CST);
    return c;
}

/* ---- ring buffer for raw key events (SYS_getkey) ----
 * Unlike inbuf this one carries the scancode and the E0-extended flag as
 * well as the ASCII translation, and it is drained by getkey() only, so a
 * raw-mode reader never steals characters from console_read(). */
#define KEYQ_SZ 64
static volatile u32 keyq[KEYQ_SZ];
static volatile u32 keyq_r, keyq_w;

/* The single task blocked in sys_getkey(); keyq_push() wakes it. */
static struct task *volatile key_waiter;

void key_waiter_arm(struct task *t)
{ key_waiter = t; }

void key_waiter_disarm(struct task *t)
{
    if (key_waiter == t) key_waiter = NULL;
}

void keyq_push(u32 ev)
{
    if ((keyq_w + 1) % KEYQ_SZ == keyq_r) return; /* drop when full */
    keyq[keyq_w] = ev;
    __atomic_store_n(&keyq_w, (keyq_w + 1) % KEYQ_SZ, __ATOMIC_SEQ_CST);

    struct task *w = key_waiter;
    if (w && w->state == T_SLEEPING) {
        key_waiter = NULL;
        w->state = T_RUNNABLE;
        runqueue_add(w);
    }
}

int keyq_pop(void)
{
    u32 r = __atomic_load_n(&keyq_r, __ATOMIC_SEQ_CST);
    if (r == keyq_w) return -1;
    int ev = (int)keyq[r];
    __atomic_store_n(&keyq_r, (r + 1) % KEYQ_SZ, __ATOMIC_SEQ_CST);
    return ev;
}

/* ---- central dispatch ---- */
static const char *exc_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM", "#DF", "?9",  "#TS",
    "#NP", "#SS", "#GP", "#PF", "?15", "#MF", "#AC", "#MC", "#XM", "#VE", "?21",
    "?22", "?23", "?24", "?25", "?26", "?27", "?28", "?29", "?30", "?31",
};

extern void ipi_dispatch(u32 vector);

void isr_common(struct intr_frame *f)
{
    /* ---- IPI vectors (0xF0..0xF2) ---- */
    if (f->intno >= 0xF0 && f->intno <= 0xF2) {
        ipi_dispatch((u32)f->intno);
        apic_eoi();
        /* Reschedule IPI (runqueue_add on this CPU) must preempt a
         * returning-to-user task; without this, APs have no PIT and
         * would only schedule when the current task voluntarily blocks. */
        if (current && (f->cs & 3) == 3) sched_maybe_preempt(f);
        return;
    }

    if (f->intno < 32) {
        u64 cr2 = 0;
        if (f->intno == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("\n[exception] %s (%d) err=%#lx rip=%#lx rsp=%#lx cr2=%#lx\n",
                exc_names[f->intno & 31], f->intno, f->err, f->rip, f->ussp, cr2);
        if (current)
            kprintf("[exception] task pid=%u name=%s cs=%#lx ss=%#lx\n", current->pid,
                    current->name, f->cs, f->usss);

        /* A fault from user mode must kill the offending process, not
         * panic the whole kernel (this is what SIGSEGV/SIGILL/... are for). */
        if (current && (f->cs & 3) == 3) {
            int sig = SIGSEGV;
            if (f->intno == 0 || f->intno == 16)
                sig = SIGFPE;
            else if (f->intno == 6)
                sig = SIGILL;
            else if (f->intno == 7 || f->intno == 17)
                sig = SIGBUS;
            kprintf("[exception] user fault -> kill pid=%u with sig %d\n", current->pid, sig);
            sys_exit(-sig);
            /* not reached */
        }

        {
            u64 gs_rd = rdmsr(0xC0000101);
            struct cpu_info *ci = this_cpu_data();
            kprintf("[exception] GS_MSR=%p this_cpu_data=%p\n", gs_rd, (void *)ci);
        }
        {
            u64 *sp = (u64 *)f;
            kprintf("[stack]");
            for (int q = 22; q < 30; q++) kprintf(" %lx", sp[q]);
            kprintf("\n");
        }
        panic("unrecoverable exception");
    }

    int irq = f->intno - 32;
    if (current && current->pid != 0) current->tf = f;

    if (irq >= 0 && irq < 16) {
        /* spurious IRQ7/15 check (still valid with IOAPIC) */
        if (irq == 7 || irq == 15) {
            if (f->intno == 0xFF) return;
        }
        /* EOI BEFORE the handler: handlers such as the PIT tick may call
         * schedule() and switch away; a delayed EOI would leave the LAPIC
         * ISR bit set and stall this vector until the interrupted task
         * resumes (losing ticks / hanging idle's hlt). */
        apic_eoi();
        irq_handler_t h = irq_get_handler(irq);
        if (h) h(f);
    } else {
        kprintf("stray vector %d\n", f->intno);
    }

    /* deliver pending signals on every return path to user mode */
    if (current && (f->cs & 3) == 3) do_signal_check(current);
}
