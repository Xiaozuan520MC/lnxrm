/* System call dispatch (int 0x80, nr in rax, args in rdi rsi r10). */
#include <sys/vfs.h>
#include <sys/sched.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/cpu.h>
#include <framebuffer.h>

int copy_from_user(void *, const void *, size_t);
int copy_to_user(void *, const void *, size_t);
bool user_ptr_ok(u64 p, u64 n);
long sys_getdent(int fd, void *ubuf, size_t len);
void ps_dump(void);
int blk_list_all(void *ubuf, int max);

static long sys_brk(u64 newbrk)
{
    if (!newbrk) return (long)current->brk_cur;
    if ((u64)newbrk < (u64)current->brk_base || newbrk > USER_STACK_TOP - (1 << 20))
        return LNXRM_ENOMEM;
    u64 old = ALIGN_UP((u64)current->brk_cur, PAGE_SIZE);
    u64 tgt = ALIGN_UP(newbrk, PAGE_SIZE);
    if (tgt > old) {
        for (u64 va = old; va < tgt; va += PAGE_SIZE) {
            u64 pa = pmm_alloc();
            if (!pa) return LNXRM_ENOMEM;
            memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
            vmm_map_user(current->pml4, va, pa, true, true);
        }
    } else {
        for (u64 va = tgt; va < old; va += PAGE_SIZE) {
            u64 pa = vmm_unmap_user(current->pml4, va);
            if (pa) pmm_free(pa);
        }
    }
    current->brk_cur = (void *)newbrk;
    return (long)current->brk_cur;
}

/* ---- kill(pid, sig) ---- */
static long sys_kill(int pid, int sig)
{
    if (sig < 0 || sig >= NR_SIGNALS) return LNXRM_EINVAL; /* EINVAL */

    if (pid > 0) {
        /* send to specific process */
        struct task *t = find_task(pid);
        if (!t) return LNXRM_ESRCH; /* ESRCH */
        send_signal(t, sig);
        return 0;
    } else if (pid == -1) {
        /* send to all processes (except idle) */
        int i = 0;
        for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
            if (t->pid > 0) send_signal(t, sig);
        }
        return 0;
    }
    return LNXRM_EINVAL;
}

/* ---- sigaction(sig, act, oldact) ---- */
static long sys_sigaction(int sig, const struct lnxrm_sigaction *uact,
                          struct lnxrm_sigaction *uoldact)
{
    if (sig < 1 || sig >= NR_SIGNALS) return LNXRM_EINVAL;

    /* save old handler */
    if (uoldact && user_ptr_ok((u64)uoldact, sizeof(*uoldact))) {
        struct lnxrm_sigaction old;
        old.sa_handler = current->sa[sig].sa_handler;
        old.sa_mask = current->sa[sig].sa_mask;
        old.sa_flags = current->sa[sig].sa_flags;
        copy_to_user(uoldact, &old, sizeof(old));
    }

    /* set new handler */
    if (uact && user_ptr_ok((u64)uact, sizeof(*uact))) {
        struct lnxrm_sigaction act;
        copy_from_user(&act, uact, sizeof(act));
        current->sa[sig].sa_handler = act.sa_handler;
        current->sa[sig].sa_mask = act.sa_mask;
        current->sa[sig].sa_flags = act.sa_flags;
    }

    return 0;
}

/* ---- sigprocmask(how, set, oldset) ---- */
static long sys_sigprocmask(int how, const u64 *uset, u64 *uoldset)
{
    u64 old_mask = current->signal_mask;

    if (uoldset && user_ptr_ok((u64)uoldset, sizeof(u64)))
        copy_to_user(uoldset, &old_mask, sizeof(u64));

    if (uset && user_ptr_ok((u64)uset, sizeof(u64))) {
        u64 new_set;
        copy_from_user(&new_set, uset, sizeof(u64));

        switch (how) {
        case 0: /* SIG_BLOCK */
            current->signal_mask |= new_set;
            break;
        case 1: /* SIG_UNBLOCK */
            current->signal_mask &= ~new_set;
            break;
        case 2: /* SIG_SETMASK */
            current->signal_mask = new_set;
            break;
        default:
            return LNXRM_EINVAL;
        }
        /* cannot block SIGKILL or SIGSTOP */
        current->signal_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    }

    return 0;
}

/* ---- getcpu() ---- */
static long sys_getcpu(void)
{ return this_cpu_data()->id; }

/* ---- getkey(flags): read one raw key event from the PS/2 keyboard ----
 * Returns ascii | scancode<<8 | extended<<16, blocks (or returns
 * -EAGAIN with LNXRM_KBD_NONBLOCK) when no key is pending. */
static long sys_getkey(int flags)
{
    for (;;) {
        int ev = keyq_pop();
        if (ev >= 0) return ev;
        if (flags & LNXRM_KBD_NONBLOCK) return LNXRM_EAGAIN;

        /* No key available.  A signal (EINTR) beats sleeping. */
        if (current && current->signal_pending) return LNXRM_EINTR;
        if (!current) return LNXRM_EIO;

        /* Sleep until the next tick (jiffy safety net) or until
         * keyq_push() wakes us via key_waiter.  Arm the waiter AFTER
         * setting T_SLEEPING so a racing push either sees T_SLEEPING and
         * wakes us, or leaves an event the recheck below picks up. */
        current->state = T_SLEEPING;
        current->sleep_until = jiffies + 1;
        key_waiter_arm(current);
        ev = keyq_pop();
        if (ev >= 0) {
            /* The wake may have enqueued us before we saw the event. */
            key_waiter_disarm(current);
            runqueue_remove(current);
            current->state = T_RUNNING;
            return ev;
        }

        runqueue_remove(current); /* off-queue invariant before sleep */
        schedule();
        key_waiter_disarm(current);
        current->state = T_RUNNING;
    }
}

/* ---- framebuffer syscalls ---- */
static long sys_fb_info(struct lnxrm_fb_info *uinfo)
{
    struct lnxrm_fb_info info;
    info.width = fb_width;
    info.height = fb_height;
    info.bpp = fb_bpp;
    info.pitch = fb_pitch;
    if (copy_to_user(uinfo, &info, sizeof(info)) < 0) return LNXRM_EFAULT;
    return 0;
}

static long sys_fb_clear(u32 color)
{
    fb_clear((u8)color);
    return 0;
}

static long sys_fb_fill(struct lnxrm_fb_rect *urect)
{
    struct lnxrm_fb_rect r;
    if (!user_ptr_ok((u64)urect, sizeof(r))) return LNXRM_EFAULT;
    copy_from_user(&r, urect, sizeof(r));
    fb_fill_rect(r.x, r.y, r.w, r.h, (u8)r.color);
    return 0;
}

static long sys_fb_char(struct lnxrm_fb_char *uch)
{
    struct lnxrm_fb_char c;
    if (!user_ptr_ok((u64)uch, sizeof(c))) return LNXRM_EFAULT;
    copy_from_user(&c, uch, sizeof(c));
    fb_draw_char(c.x, c.y, (char)c.ch, (u8)c.fg, (u8)c.bg);
    return 0;
}

static long sys_fb_puts(struct lnxrm_fb_str *ustr)
{
    struct lnxrm_fb_str s;
    if (!user_ptr_ok((u64)ustr, sizeof(s))) return LNXRM_EFAULT;
    copy_from_user(&s, ustr, sizeof(s));
    char kbuf[256];
    if (copy_user_str(kbuf, (u64)s.str, sizeof(kbuf)) < 0) return LNXRM_EFAULT;
    fb_puts(s.x, s.y, kbuf, (u8)s.fg, (u8)s.bg);
    return 0;
}

/* Copy a NUL-terminated path out of user space and run a path syscall on it. */
static long path_syscall(u64 upath, long (*fn)(const char *))
{
    char path[128];
    if (copy_user_str(path, upath, sizeof(path)) != 0) return LNXRM_EFAULT;
    return fn(path);
}

/* Same, for the two-path syscalls (rename / move). */
static long path2_syscall(u64 upath, u64 upath2, long (*fn)(const char *, const char *))
{
    char path[128], path2[128];
    if (copy_user_str(path, upath, sizeof(path)) != 0) return LNXRM_EFAULT;
    if (copy_user_str(path2, upath2, sizeof(path2)) != 0) return LNXRM_EFAULT;
    return fn(path, path2);
}

/* Read the syscall arguments out of the trap frame and run fn.
 * This is the whole dispatch table: register ABI plumbing lives in
 * syscall_entry(), everything else is a plain C call. */
static long syscall_do(struct intr_frame *f, u64 nr, u64 a3)
{
    long ret = LNXRM_ENOSYS;

    switch (nr) {
    case SYS_read:
        ret = sys_read((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_write:
        ret = sys_write((int)f->rdi, (const void *)f->rsi, a3);
        break;
    case SYS_open: {
        char path[128];
        ret = copy_user_str(path, f->rdi, sizeof(path)) == 0 ? sys_open(path, (int)f->rsi)
                                                             : LNXRM_EFAULT;
        break;
    }
    case SYS_close:
        ret = sys_close((int)f->rdi);
        break;
    case SYS_lseek:
        ret = sys_lseek((int)f->rdi, (long)f->rsi, (int)a3);
        break;
    case SYS_getdent:
        ret = sys_getdent((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_dup2:
        ret = sys_dup2((int)f->rdi, (int)f->rsi);
        break;
    case SYS_brk:
        ret = sys_brk(f->rdi);
        break;
    case SYS_fork:
        ret = sys_fork();
        break;
    case SYS_execve:
        ret = sys_execve((const char *)f->rdi, (char *const *)f->rsi, (char *const *)a3);
        break;
    case SYS_exit:
        sys_exit((int)f->rdi);
        break;
    case SYS_getpid:
        ret = current->pid;
        break;
    case SYS_getppid:
        ret = current->parent ? current->parent->pid : 0;
        break;
    case SYS_wait4:
        ret = sys_waitpid((int)f->rdi, (int *)f->rsi, (int)a3);
        break;
    case SYS_nanosleep:
        ret = sys_nanosleep(f->rdi / 1000000 ? f->rdi / 1000000 : 1);
        break;
    case SYS_uname: {
        static struct lnxrm_utsname un = {"LNXRM", "lnxrm", "1.0.0", "lnxrm #1", "x86_64"};
        if (copy_to_user((void *)f->rdi, &un, sizeof(un)) < 0)
            ret = LNXRM_EFAULT;
        else
            ret = 0;
        break;
    }
    case SYS_ps:
        ps_dump();
        ret = 0;
        break;
    /* signal syscalls */
    case SYS_kill:
        ret = sys_kill((int)f->rdi, (int)f->rsi);
        break;
    case SYS_sigaction:
        ret = sys_sigaction((int)f->rdi, (const struct lnxrm_sigaction *)f->rsi,
                            (struct lnxrm_sigaction *)a3);
        break;
    case SYS_sigprocmask:
        ret = sys_sigprocmask((int)f->rdi, (const u64 *)f->rsi, (u64 *)a3);
        break;
    case SYS_sigreturn:
        ret = sys_sigreturn();
        break;
    /* SMP syscalls */
    case SYS_getcpu:
        ret = sys_getcpu();
        break;
    case SYS_diskinfo:
        ret = blk_list_all((void *)f->rdi, (int)f->rsi);
        break;
    case SYS_mkdir:
        ret = path_syscall(f->rdi, sys_mkdir);
        break;
    case SYS_unlink:
        ret = path_syscall(f->rdi, sys_unlink);
        break;
    case SYS_rmdir:
        ret = path_syscall(f->rdi, sys_rmdir);
        break;
    /* keyboard / namespace syscalls */
    case SYS_getkey:
        ret = sys_getkey((int)f->rdi);
        break;
    case SYS_rename:
        ret = path2_syscall(f->rdi, f->rsi, sys_rename);
        break;
    case SYS_move:
        ret = path2_syscall(f->rdi, f->rsi, sys_move);
        break;
    /* framebuffer syscalls */
    case SYS_fb_info:
        ret = sys_fb_info((struct lnxrm_fb_info *)f->rdi);
        break;
    case SYS_fb_clear:
        ret = sys_fb_clear((u32)f->rdi);
        break;
    case SYS_fb_fill:
        ret = sys_fb_fill((struct lnxrm_fb_rect *)f->rdi);
        break;
    case SYS_fb_char:
        ret = sys_fb_char((struct lnxrm_fb_char *)f->rdi);
        break;
    case SYS_fb_puts:
        ret = sys_fb_puts((struct lnxrm_fb_str *)f->rdi);
        break;
    default:
        kprintf("[sys] unknown syscall %d from pid %u\n", nr, current->pid);
        ret = LNXRM_ENOSYS;
    }
    return ret;
}

void syscall_entry(struct intr_frame *f)
{
    if (current) current->tf = f;

    long ret = syscall_do(f, f->rax, f->r10);

    f->rax = (u64)(long)ret;
    f->rflags |= 0x200;
    /* Deliver pending signals before iretq back to user mode.  The return
     * value is already in f->rax, so the signal frame captures it and
     * sigreturn restores it for the interrupted syscall. */
    if (current && (f->cs & 3) == 3) do_signal_check(current);
}
