/* Signal delivery, signal trampoline management. */
#include <sys/sched.h>
#include <mm/mm.h>
#include <console.h>
#include <sys/vfs.h>

/* Map the signal trampoline at a fixed address in user space.
 * Idempotent: fork's dup_user_aspace() already copies the parent's
 * slot-255 subtree (including the trampoline page), so re-allocating
 * here would leak the copied page on every fork. */
void signal_init_trampoline(u64 pml4)
{
    extern u8 _signal_trampoline[];
    extern u8 _signal_trampoline_size[];

    /* Already mapped (copied by fork or installed earlier). A fresh
     * pml4 returns 0 from vmm_translate_in and never aliases a real
     * physical page at SIG_TRAMPOLINE_VA + non-zero offset. */
    if (vmm_translate_in(pml4, SIG_TRAMPOLINE_VA)) return;

    u64 code_size = *(u64 *)_signal_trampoline_size;

    /* allocate a physical page for the trampoline code */
    u64 pa = pmm_alloc();
    if (!pa) return;

    /* Map the page at FIXMAP_VA so we can safely memset/memcpy into it.
     * PHYS_TO_VIRT(pa) is not safe for freshly allocated pages because
     * the virtual alias may conflict with existing mappings (e.g. VGA). */
    vmm_map_kernel_page(FIXMAP_VA, pa, 0);
    memset((void *)FIXMAP_VA, 0, PAGE_SIZE);
    memcpy((void *)FIXMAP_VA, _signal_trampoline, code_size);

    /* map at fixed address in user space (slot 255) */
    vmm_map_user(pml4, SIG_TRAMPOLINE_VA, pa, true, true);
}

/* Send signal `sig` to task `t`. */
void send_signal(struct task *t, int sig)
{
    if (sig < 1 || sig >= _NSIG || !t) return;

    /* SIGKILL and SIGSTOP cannot be ignored/blocked */
    if (sig == SIGKILL) {
        t->signal_pending |= (1ULL << SIGKILL);
        /* wake if sleeping so it can be killed */
        if (t->state == T_SLEEPING) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
        return;
    }

    if (sig == SIGSTOP) {
        t->signal_pending |= (1ULL << SIGSTOP);
        if (t->state == T_SLEEPING) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
        return;
    }

    /* SIGCONT must wake a stopped task, otherwise it stays off every
     * runqueue forever (STOPPED is not SLEEPING, so the generic wake
     * below would not catch it). */
    if (sig == SIGCONT) {
        t->signal_pending |= (1ULL << SIGCONT);
        if (t->state == T_STOPPED || t->state == T_SLEEPING) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
        return;
    }

    /* ignore if handler is SIG_IGN or SIG_DFL (terminate) */
    if (t->sa[sig].sa_handler == SIG_IGN) return;

    /* set pending bit */
    t->signal_pending |= (1ULL << sig);

    /* if task is sleeping, wake it up to deliver the signal */
    if (t->state == T_SLEEPING) {
        t->state = T_RUNNABLE;
        runqueue_add(t);
    }
}

/* Check for pending signals and deliver. Called on return to user space. */
void do_signal_check(struct task *t)
{
    if (!t || t->pid == 0) return;

    u64 pending = t->signal_pending & ~t->signal_mask;
    /* unmaskable, like POSIX */
    pending |= t->signal_pending & ((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    if (!pending) return;

    /* SIGSTOP: stop the process at a scheduling point */
    if (pending & (1ULL << SIGSTOP)) {
        t->signal_pending &= ~(1ULL << SIGSTOP);
        t->state = T_STOPPED;
        /* Park immediately: schedule() will not requeue a STOPPED task.
         * send_signal(SIGCONT) puts it back on a runqueue later. */
        schedule();
        return;
    }

    /* SIGCONT: continue a stopped process */
    if (pending & (1ULL << SIGCONT)) {
        t->signal_pending &= ~(1ULL << SIGCONT);
        if (t->state == T_STOPPED) {
            t->state = T_RUNNABLE;
            if (t != current) runqueue_add(t);
        }
        return;
    }

    /* find first pending signal */
    for (int sig = 1; sig < _NSIG; sig++) {
        if (!(pending & (1ULL << sig))) continue;

        t->signal_pending &= ~(1ULL << sig);

        /* SIGKILL: terminate immediately */
        if (sig == SIGKILL) {
            sys_exit(-SIGKILL);
            /* not reached */
        }

        /* SIGTERM/SIGINT/etc with SIG_DFL => terminate */
        if (t->sa[sig].sa_handler == SIG_DFL || t->sa[sig].sa_handler == NULL) {
            if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT || sig == SIGILL ||
                sig == SIGABRT || sig == SIGSEGV) {
                sys_exit(-sig);
            }
            continue;
        }

        /* deliver to user handler via signal frame */
        struct intr_frame *tf = t->tf;
        if (!tf) continue;

        u64 handler = (u64)t->sa[sig].sa_handler;
        u64 orig_sp = tf->ussp;

        /* Build the frame below the user stack pointer:
         *   [new_sp]      return address -> signal trampoline (sigreturn)
         *   [new_sp+8..]  176-byte sigreturn frame
         * At handler entry RSP must be 8-mod-16 (SysV call alignment) and
         * must point at the return address so the handler's `ret` lands on
         * the trampoline.  Everything is validated before any store: a
         * broken user stack must kill the process, not fault the kernel. */
        u64 frame_size = 176 + 8; /* retaddr + frame */
        u64 new_sp = ((orig_sp - frame_size) & ~15UL) - 8;
        if (!user_ptr_ok(handler, 1) || !user_ptr_ok(new_sp, frame_size)) {
            sys_exit(-SIGSEGV);
            /* not reached */
        }

        u64 *slot = (u64 *)new_sp;
        slot[0] = SIG_TRAMPOLINE_VA; /* return address */
        u64 *frame = slot + 1;

        memset(frame, 0, 176);

        frame[0] = sig;     /* sig number */
        frame[1] = tf->rip; /* interrupted rip */
        frame[2] = tf->cs;
        frame[3] = tf->rflags;
        frame[4] = tf->ussp;
        frame[5] = tf->usss;
        frame[6] = tf->rax;
        frame[7] = tf->rbx;
        frame[8] = tf->rcx;
        frame[9] = tf->rdx;
        frame[10] = tf->rsi;
        frame[11] = tf->rdi;
        frame[12] = tf->rbp;
        frame[13] = tf->r8;
        frame[14] = tf->r9;
        frame[15] = tf->r10;
        frame[16] = tf->r11;
        frame[17] = tf->r12;
        frame[18] = tf->r13;
        frame[19] = tf->r14;
        frame[20] = tf->r15;

        /* redirect execution into the user handler */
        tf->rip = handler;
        tf->ussp = new_sp;
        tf->rdi = sig;       /* first argument = signal number */
        tf->rflags |= 0x200; /* run the handler with interrupts enabled */

        return; /* deliver one signal at a time */
    }
}

/* Restore from signal frame. Called via SIG_sigreturn syscall. */
long sys_sigreturn(void)
{
    struct task *t = current;
    if (!t || !t->tf) return LNXRM_EFAIL;

    /* The frame sits where RSP was when the trampoline issued sigreturn.
     * Validate it fully: without this a forged frame could load kernel
     * CS and turn sigreturn into a ring-0 escape. */
    u64 usp = t->tf->ussp;
    if (!user_ptr_ok(usp, 176)) {
        sys_exit(-SIGSEGV);
        return LNXRM_EFAIL; /* not reached */
    }
    u64 *frame = (u64 *)usp;

    u64 rip = frame[1];
    u64 cs = frame[2];
    u64 rsp = frame[4];
    u64 ss = frame[5];

    /* Only the user code/data selectors from our GDT (0x18/0x20, RPL 3),
     * and both rip/rsp must resolve to mapped user pages. */
    if ((cs & 3UL) != 3 || (cs & ~3UL) != 0x18 || (ss & 3UL) != 3 || (ss & ~3UL) != 0x20 ||
        !user_ptr_ok(rip, 1) || !user_ptr_ok(rsp, 1)) {
        sys_exit(-SIGSEGV);
        return LNXRM_EFAIL; /* not reached */
    }

    /* restore registers from frame */
    t->tf->rip = rip;
    t->tf->cs = cs;
    /* sanitize rflags: reserved bit set, IOPL cleared, IF restored */
    t->tf->rflags = (frame[3] | 0x2 | 0x200) & ~(3UL << 12);
    t->tf->ussp = rsp;
    t->tf->usss = ss;
    t->tf->rax = frame[6];
    t->tf->rbx = frame[7];
    t->tf->rcx = frame[8];
    t->tf->rdx = frame[9];
    t->tf->rsi = frame[10];
    t->tf->rdi = frame[11];
    t->tf->rbp = frame[12];
    t->tf->r8 = frame[13];
    t->tf->r9 = frame[14];
    t->tf->r10 = frame[15];
    t->tf->r11 = frame[16];
    t->tf->r12 = frame[17];
    t->tf->r13 = frame[18];
    t->tf->r14 = frame[19];
    t->tf->r15 = frame[20];

    return 0;
}
