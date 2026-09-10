/* Signal delivery, signal trampoline management. */
#include <signal.h>
#include <sched.h>
#include <mm.h>
#include <console.h>
#include <syscall.h>

/* Map the signal trampoline at a fixed address in user space. */
void signal_init_trampoline(u64 pml4)
{
    extern u8 _signal_trampoline[];
    extern u8 _signal_trampoline_size[];
    u64 code_size = *(u64 *)_signal_trampoline_size;

    /* allocate a physical page for the trampoline code */
    u64 pa = pmm_alloc();
    if (!pa)
        return;

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
    if (sig < 1 || sig >= _NSIG || !t)
        return;

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

    /* ignore if handler is SIG_IGN or SIG_DFL (terminate) */
    if (t->sa[sig].sa_handler == SIG_IGN)
        return;

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
    if (!t || t->pid == 0)
        return;

    u64 pending = t->signal_pending & ~t->signal_mask;
    if (!pending)
        return;

    /* SIGSTOP: stop the process */
    if (pending & (1ULL << SIGSTOP)) {
        t->signal_pending &= ~(1ULL << SIGSTOP);
        t->state = T_STOPPED;
        return;
    }

    /* SIGCONT: continue a stopped process */
    if (pending & (1ULL << SIGCONT)) {
        t->signal_pending &= ~(1ULL << SIGCONT);
        if (t->state == T_STOPPED) {
            t->state = T_RUNNABLE;
        }
        return;
    }

    /* find first pending signal */
    for (int sig = 1; sig < _NSIG; sig++) {
        if (!(pending & (1ULL << sig)))
            continue;

        t->signal_pending &= ~(1ULL << sig);

        /* SIGKILL: terminate immediately */
        if (sig == SIGKILL) {
            sys_exit(-SIGKILL);
            /* not reached */
        }

        /* SIGTERM/SIGINT/etc with SIG_DFL => terminate */
        if (t->sa[sig].sa_handler == SIG_DFL ||
            t->sa[sig].sa_handler == NULL) {
            if (sig == SIGINT || sig == SIGTERM || sig == SIGQUIT ||
                sig == SIGILL || sig == SIGABRT || sig == SIGSEGV) {
                sys_exit(-sig);
            }
            continue;
        }

        /* deliver to user handler via signal frame */
        struct intr_frame *tf = t->tf;
        if (!tf)
            continue;

        /* save original context on user stack */
        u64 usp = tf->ussp;

        /* build signal frame on user stack:
         * [rsp+0]   sig_number
         * [rsp+8]   original rip (return address)
         * [rsp+16]  original cs
         * [rsp+24]  original rflags
         * [rsp+32]  original rsp
         * [rsp+40]  original ss
         * [rsp+48]  rax, rbx, rcx, rdx, rsi, rdi, rbp
         * [rsp+104] r8..r15
         * total: 176 bytes */
        u64 frame_size = 176;
        usp -= frame_size;

        /* zero the frame */
        memset((void *)usp, 0, frame_size);

        u64 *frame = (u64 *)usp;
        frame[0]  = sig;                        /* sig number */
        frame[1]  = tf->rip;                    /* return address */
        frame[2]  = tf->cs;
        frame[3]  = tf->rflags;
        frame[4]  = tf->ussp;
        frame[5]  = tf->usss;
        frame[6]  = tf->rax;
        frame[7]  = tf->rbx;
        frame[8]  = tf->rcx;
        frame[9]  = tf->rdx;
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

        /* redirect execution to signal trampoline */
        tf->rip = SIG_TRAMPOLINE_VA;
        tf->ussp = usp;
        tf->rdi = sig;      /* first argument = signal number */
        tf->rflags &= ~0x200;  /* disable IF during signal delivery */

        return;  /* deliver one signal at a time */
    }
}

/* Restore from signal frame. Called via SIG_sigreturn syscall. */
long sys_sigreturn(void)
{
    struct task *t = current;
    if (!t || !t->tf)
        return -1;

    /* the signal frame is at the user rsp */
    u64 usp = t->tf->ussp;
    u64 *frame = (u64 *)usp;

    /* restore registers from frame */
    t->tf->rip   = frame[1];
    t->tf->cs    = frame[2];
    t->tf->rflags = frame[3] | 0x200;  /* re-enable IF */
    t->tf->ussp  = frame[4];
    t->tf->usss  = frame[5];
    t->tf->rax   = frame[6];
    t->tf->rbx   = frame[7];
    t->tf->rcx   = frame[8];
    t->tf->rdx   = frame[9];
    t->tf->rsi   = frame[10];
    t->tf->rdi   = frame[11];
    t->tf->rbp   = frame[12];
    t->tf->r8    = frame[13];
    t->tf->r9    = frame[14];
    t->tf->r10   = frame[15];
    t->tf->r11   = frame[16];
    t->tf->r12   = frame[17];
    t->tf->r13   = frame[18];
    t->tf->r14   = frame[19];
    t->tf->r15   = frame[20];

    return 0;
}
